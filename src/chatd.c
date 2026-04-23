#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/**
 * Parse a CLI port string into a validated TCP port number.
 *
 * @param text Input string expected to contain only decimal digits.
 * @param out_port Output pointer for the parsed port on success.
 * @return 0 on success, -1 on parse/validation failure.
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
 *
 * @param port Local TCP port (host byte order).
 * @return Listening fd on success, -1 on error.
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
 * Run the server's top-level poll loop.
 *
 * This currently watches only the listening socket and serves as the
 * event-loop skeleton for future connection handling logic.
 *
 * @param listen_fd Listening socket file descriptor.
 * @return 0 on clean shutdown, -1 on fatal polling error.
 */
static int run_poll_loop(int listen_fd) {
    struct pollfd pfd;
    int ready;

    pfd.fd = listen_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (1) {
        ready = poll(&pfd, 1, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            errno = EIO;
            return -1;
        }

        if ((pfd.revents & POLLIN) != 0) {
            /* Connection handling is added in the next step. */
            continue;
        }
    }
}

/**
 * Program entrypoint for chatd.
 *
 * Expects exactly one argument: a TCP port number.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return 0 on success, non-zero on usage/setup/runtime errors.
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
