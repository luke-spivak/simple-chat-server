#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "client.h"
#include "handlers.h"
#include "protocol.h"

/**
 * Parse a CLI port string into a validated TCP port number.
 */
static int parse_port(const char *text, unsigned short *out_port) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    if (value < 1 || value > 65535 || value > USHRT_MAX) {
        return -1;
    }

    *out_port = (unsigned short)value;
    return 0;
}

/**
 * Create and initialize a listening IPv4 TCP socket.
 *
 * The socket is configured with SO_REUSEADDR, bound to INADDR_ANY:port,
 * and moved to listening state.
 */
static int setup_listen_socket(unsigned short port) {
    int fd;
    int opt;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * Ensure the pollfd array has room for at least one more entry.
 */
static int ensure_poll_capacity(struct pollfd **pfds, nfds_t *cap, nfds_t needed) {
    nfds_t new_cap;
    struct pollfd *new_pfds;

    if (*cap >= needed) {
        return 0;
    }

    new_cap = (*cap == 0) ? 8 : (*cap * 2);
    while (new_cap < needed) {
        new_cap *= 2;
    }

    new_pfds = realloc(*pfds, new_cap * sizeof(*new_pfds));
    if (new_pfds == NULL) {
        return -1;
    }

    *pfds = new_pfds;
    *cap = new_cap;
    return 0;
}

/**
 * Add a file descriptor to the poll set.
 */
static int add_poll_fd(struct pollfd **pfds, nfds_t *count, nfds_t *cap, int fd, short events) {
    if (ensure_poll_capacity(pfds, cap, *count + 1) != 0) {
        return -1;
    }

    (*pfds)[*count].fd = fd;
    (*pfds)[*count].events = events;
    (*pfds)[*count].revents = 0;
    *count += 1;
    return 0;
}

/**
 * Remove a client file descriptor from the poll set and close it.
 */
static void remove_client_fd(struct pollfd *pfds, nfds_t *count, nfds_t idx) {
    nfds_t i;

    close(pfds[idx].fd);
    for (i = idx; i + 1 < *count; i++) {
        pfds[i] = pfds[i + 1];
    }
    *count -= 1;
}

/**
 * Disconnect and remove a client by poll-array index.
 *
 * This removes both the client registry entry and the pollfd entry in a
 * synchronized way. The client's screen name/state is cleared first so the
 * name is immediately available for reuse.
 */
static void disconnect_client_at_poll_index(
    struct pollfd *pfds, nfds_t *poll_count, client_t *clients, nfds_t *client_count, nfds_t poll_idx
) {
    nfds_t client_idx;

    if (poll_idx == 0) {
        return;
    }

    client_idx = poll_idx - 1;
    clear_client_state(&clients[client_idx]);
    remove_client(clients, client_count, client_idx);
    remove_client_fd(pfds, poll_count, poll_idx);
}

/**
 * Register a newly accepted client in both poll and client registries.
 */
static int register_client(
    struct pollfd **pfds,
    nfds_t *poll_count,
    nfds_t *poll_cap,
    client_t **clients,
    nfds_t *client_count,
    nfds_t *client_cap,
    int fd
) {
    if (add_poll_fd(pfds, poll_count, poll_cap, fd, POLLIN) != 0) {
        return -1;
    }

    if (add_client(clients, client_count, client_cap, fd) != 0) {
        close(fd);
        *poll_count -= 1;
        return -1;
    }

    return 0;
}

/**
 * Remove a consumed byte prefix from a client's input buffer.
 */
static void consume_client_input(client_t *client, size_t consumed) {
    if (consumed >= client->input_len) {
        client->input_len = 0;
        return;
    }

    memmove(client->input_buffer, client->input_buffer + consumed, client->input_len - consumed);
    client->input_len -= consumed;
}

/**
 * Validate and dispatch complete frames currently buffered for a client.
 *
 * A frame is considered complete when the declared body length is present.
 * The final byte of that body must be a '|' delimiter.
 */
static int validate_and_consume_frames(client_t *client, const client_t *clients, nfds_t client_count) {
    protocol_header_t header;
    int header_parse_result;
    size_t total_frame_len;
    size_t body_len;
    const char *body;

    while (1) {
        header_parse_result = parse_protocol_header(client->input_buffer, client->input_len, &header);
        if (header_parse_result == HEADER_PARSE_INCOMPLETE) {
            return 0;
        }
        if (header_parse_result == HEADER_PARSE_INVALID) {
            return fatal_unreadable(client);
        }

        if (header.version != 1U) {
            return fatal_unreadable(client);
        }

        if (header.header_len > sizeof(client->input_buffer)) {
            return fatal_unreadable(client);
        }

        body_len = (size_t)header.body_len;
        if (body_len == 0 || body_len > sizeof(client->input_buffer) - header.header_len) {
            return fatal_unreadable(client);
        }

        total_frame_len = header.header_len + body_len;
        if (client->input_len < total_frame_len) {
            return 0;
        }

        if (client->input_buffer[total_frame_len - 1] != '|') {
            return fatal_unreadable(client);
        }

        body = client->input_buffer + header.header_len;
        if (dispatch_client_command(client, clients, client_count, &header, body, body_len) != 0) {
            return -1;
        }

        consume_client_input(client, total_frame_len);
    }
}

/**
 * Read available bytes from a client socket into its input buffer.
 */
static int read_client_input(client_t *client) {
    size_t free_space;
    ssize_t bytes_read;

    free_space = sizeof(client->input_buffer) - client->input_len;
    if (free_space == 0) {
        return 1;
    }

    bytes_read = recv(client->fd, client->input_buffer + client->input_len, free_space, 0);
    if (bytes_read > 0) {
        client->input_len += (size_t)bytes_read;
        return 0;
    }

    if (bytes_read == 0) {
        return 1;
    }

    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }

    return -1;
}

/**
 * Run the server's top-level poll loop.
 *
 * This loop monitors the listening socket and accepts new clients,
 * registering each accepted socket in the poll set and buffering input.
 */
static int run_poll_loop(int listen_fd) {
    struct pollfd *pfds;
    client_t *clients;
    nfds_t count;
    nfds_t cap;
    nfds_t client_count;
    nfds_t client_cap;
    nfds_t i;
    int ready;
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    client_t *client;
    int read_result;

    pfds = NULL;
    clients = NULL;
    count = 0;
    cap = 0;
    client_count = 0;
    client_cap = 0;

    if (add_poll_fd(&pfds, &count, &cap, listen_fd, POLLIN) != 0) {
        return -1;
    }

    while (1) {
        /* Block until at least one monitored fd has activity. */
        ready = poll(pfds, count, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(clients);
            free(pfds);
            return -1;
        }

        for (i = 0; i < count && ready > 0; i++) {
            if (pfds[i].revents == 0) {
                continue;
            }
            ready -= 1;

            if (i == 0) {
                /* Slot 0 is always the listening socket. */
                if ((pfds[i].revents & (POLLERR | POLLNVAL)) != 0) {
                    free(clients);
                    free(pfds);
                    errno = EIO;
                    return -1;
                }

                if ((pfds[i].revents & POLLIN) != 0) {
                    /* Accept one pending connection and track it in poll(). */
                    client_len = sizeof(client_addr);
                    client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                    if (client_fd < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        free(clients);
                        free(pfds);
                        return -1;
                    }

                    if (register_client(
                            &pfds, &count, &cap, &clients, &client_count, &client_cap, client_fd
                        ) != 0) {
                        free(clients);
                        free(pfds);
                        return -1;
                    }
                }
                continue;
            }

            if ((pfds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                /* Close dead clients; decrement i because entries shift left. */
                disconnect_client_at_poll_index(pfds, &count, clients, &client_count, i);
                i -= 1;
                continue;
            }

            /* Read available client data into the per-client buffer; drop the client on EOF or invalid read state. */
            if ((pfds[i].revents & POLLIN) != 0) {
                client = &clients[i - 1];
                read_result = read_client_input(client);
                if (read_result < 0) {
                    free(clients);
                    free(pfds);
                    return -1;
                }
                if (read_result > 0) {
                    disconnect_client_at_poll_index(pfds, &count, clients, &client_count, i);
                    i -= 1;
                    continue;
                }

                /* Fatal parse/dispatch violations (including ERR 0) close this client immediately. */
                if (validate_and_consume_frames(client, clients, client_count) != 0) {
                    disconnect_client_at_poll_index(pfds, &count, clients, &client_count, i);
                    i -= 1;
                    continue;
                }
            }
        }
    }
}

/**
 * Program entrypoint for chatd.
 *
 * Expects exactly one argument: a TCP port number.
 */
int main(int argc, char **argv) {
    int listen_fd;
    unsigned short port;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 2;
    }

    if (parse_port(argv[1], &port) != 0) {
        fprintf(stderr, "Invalid port: %s\n", argv[1]);
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 2;
    }

    listen_fd = setup_listen_socket(port);
    if (listen_fd < 0) {
        perror("Failed to initialize listening socket");
        return 1;
    }

    if (run_poll_loop(listen_fd) != 0) {
        perror("poll loop failed");
        close(listen_fd);
        return 1;
    }

    close(listen_fd);
    return 0;
}
