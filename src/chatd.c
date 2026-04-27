#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_SCREEN_NAME_LEN 32
#define MAX_STATUS_LEN 64
#define MAX_USER_MESSAGE_LEN 80
#define CLIENT_INPUT_CAPACITY 4096
#define MAX_BODY_LENGTH_FIELD 99999

typedef struct {
    int fd;
    int is_authenticated;
    char screen_name[MAX_SCREEN_NAME_LEN + 1];
    char status[MAX_STATUS_LEN + 1];
    char input_buffer[CLIENT_INPUT_CAPACITY];
    size_t input_len;
} client_t;

typedef struct {
    unsigned int version;
    char code[4];
    unsigned int body_len;
    size_t header_len;
} protocol_header_t;

enum {
    HEADER_PARSE_INVALID = -1,
    HEADER_PARSE_INCOMPLETE = 0,
    HEADER_PARSE_COMPLETE = 1
};

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
 * Ensure the pollfd array has room for at least one more entry.
 *
 * @param pfds Pointer to the pollfd array pointer.
 * @param cap Pointer to current array capacity.
 * @param needed Minimum required capacity.
 * @return 0 on success, -1 on allocation failure.
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
 *
 * @param pfds Pointer to the pollfd array pointer.
 * @param count Pointer to number of active pollfd entries.
 * @param cap Pointer to pollfd array capacity.
 * @param fd File descriptor to add.
 * @param events Poll events to monitor.
 * @return 0 on success, -1 on allocation failure.
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
 * Ensure the client array has room for at least one more entry.
 *
 * @param clients Pointer to the client array pointer.
 * @param cap Pointer to current client array capacity.
 * @param needed Minimum required capacity.
 * @return 0 on success, -1 on allocation failure.
 */
static int ensure_client_capacity(client_t **clients, nfds_t *cap, nfds_t needed) {
    nfds_t new_cap;
    client_t *new_clients;

    if (*cap >= needed) {
        return 0;
    }

    new_cap = (*cap == 0) ? 8 : (*cap * 2);
    while (new_cap < needed) {
        new_cap *= 2;
    }

    new_clients = realloc(*clients, new_cap * sizeof(*new_clients));
    if (new_clients == NULL) {
        return -1;
    }

    *clients = new_clients;
    *cap = new_cap;
    return 0;
}

/**
 * Add a new client record to the client registry.
 *
 * @param clients Pointer to the client array pointer.
 * @param count Pointer to number of active clients.
 * @param cap Pointer to client array capacity.
 * @param fd Accepted client socket.
 * @return 0 on success, -1 on allocation failure.
 */
static int add_client(client_t **clients, nfds_t *count, nfds_t *cap, int fd) {
    client_t *client;

    if (ensure_client_capacity(clients, cap, *count + 1) != 0) {
        return -1;
    }

    client = &(*clients)[*count];
    memset(client, 0, sizeof(*client));
    client->fd = fd;
    client->is_authenticated = 0;

    *count += 1;
    return 0;
}

/**
 * Remove a client record from the client registry.
 *
 * @param clients Client array.
 * @param count Pointer to number of active clients.
 * @param idx Index to remove.
 */
static void remove_client(client_t *clients, nfds_t *count, nfds_t idx) {
    nfds_t i;

    for (i = idx; i + 1 < *count; i++) {
        clients[i] = clients[i + 1];
    }
    *count -= 1;
}

/**
 * Remove a client file descriptor from the poll set and close it.
 *
 * @param pfds Pollfd array.
 * @param count Pointer to number of active pollfd entries.
 * @param idx Index to remove.
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
 * Register a newly accepted client in both poll and client registries.
 *
 * @param pfds Pointer to pollfd array pointer.
 * @param poll_count Pointer to active poll entries count.
 * @param poll_cap Pointer to pollfd capacity.
 * @param clients Pointer to client array pointer.
 * @param client_count Pointer to active client count.
 * @param client_cap Pointer to client array capacity.
 * @param fd Accepted client socket.
 * @return 0 on success, -1 on allocation failure.
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
 * Parse an unsigned decimal ASCII field from a bounded byte span.
 *
 * @param field Raw field bytes (not necessarily NUL-terminated).
 * @param field_len Number of bytes in the field.
 * @param max_value Maximum permitted value.
 * @param out_value Parsed value on success.
 * @return 0 on success, -1 on invalid/non-decimal/out-of-range input.
 */
static int parse_decimal_field(
    const char *field, size_t field_len, unsigned int max_value, unsigned int *out_value
) {
    size_t i;
    unsigned int value;
    unsigned int digit;

    if (field_len == 0) {
        return -1;
    }

    value = 0;
    for (i = 0; i < field_len; i++) {
        if (field[i] < '0' || field[i] > '9') {
            return -1;
        }

        digit = (unsigned int)(field[i] - '0');
        if (value > (max_value - digit) / 10U) {
            return -1;
        }
        value = value * 10U + digit;
    }

    *out_value = value;
    return 0;
}

/**
 * Parse the protocol header fields (version|code|length|) from buffered input.
 *
 * @param client Client whose input buffer is inspected.
 * @param out_header Parsed header values on complete success.
 * @return HEADER_PARSE_COMPLETE if all three header fields are available and valid,
 *         HEADER_PARSE_INCOMPLETE if more bytes are needed,
 *         HEADER_PARSE_INVALID if available bytes violate header syntax.
 */
static int parse_protocol_header(const client_t *client, protocol_header_t *out_header) {
    const char *cursor;
    const char *sep;
    size_t remaining;
    size_t field_len;

    cursor = client->input_buffer;
    remaining = client->input_len;

    sep = memchr(cursor, '|', remaining);
    if (sep == NULL) {
        return HEADER_PARSE_INCOMPLETE;
    }
    field_len = (size_t)(sep - cursor);
    if (parse_decimal_field(cursor, field_len, UINT_MAX, &out_header->version) != 0) {
        return HEADER_PARSE_INVALID;
    }

    remaining -= field_len + 1;
    cursor = sep + 1;

    sep = memchr(cursor, '|', remaining);
    if (sep == NULL) {
        return HEADER_PARSE_INCOMPLETE;
    }
    field_len = (size_t)(sep - cursor);
    if (field_len != 3) {
        return HEADER_PARSE_INVALID;
    }
    memcpy(out_header->code, cursor, 3);
    out_header->code[3] = '\0';

    remaining -= field_len + 1;
    cursor = sep + 1;

    sep = memchr(cursor, '|', remaining);
    if (sep == NULL) {
        return HEADER_PARSE_INCOMPLETE;
    }
    field_len = (size_t)(sep - cursor);
    if (field_len == 0 || field_len > 5) {
        return HEADER_PARSE_INVALID;
    }
    if (parse_decimal_field(cursor, field_len, MAX_BODY_LENGTH_FIELD, &out_header->body_len) != 0) {
        return HEADER_PARSE_INVALID;
    }

    out_header->header_len = (size_t)(sep - client->input_buffer) + 1;
    return HEADER_PARSE_COMPLETE;
}

/**
 * Serialize a protocol v1 message into an allocated byte buffer.
 *
 * Output format: 1|CODE|LEN|field1|field2|...|
 * LEN is the number of bytes after the length delimiter and includes
 * the trailing '|' after the final field.
 *
 * @param code Three-character protocol message code.
 * @param fields Message body fields to serialize.
 * @param field_count Number of entries in fields.
 * @param out_message Allocated output buffer on success (caller frees).
 * @param out_len Output byte length on success.
 * @return 0 on success, -1 on invalid input or allocation failure.
 */
int build_protocol_message_v1(
    const char code[4],
    const char *const *fields,
    size_t field_count,
    char **out_message,
    size_t *out_len
) {
    size_t i;
    size_t field_len;
    size_t body_len;
    char header[32];
    int header_len;
    size_t total_len;
    size_t offset;
    char *message;

    if (code == NULL || out_message == NULL || out_len == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (strlen(code) != 3) {
        errno = EINVAL;
        return -1;
    }
    if (field_count > 0 && fields == NULL) {
        errno = EINVAL;
        return -1;
    }

    body_len = 0;
    for (i = 0; i < field_count; i++) {
        if (fields[i] == NULL) {
            errno = EINVAL;
            return -1;
        }
        field_len = strlen(fields[i]);
        if (field_len > SIZE_MAX - body_len - 1) {
            errno = EOVERFLOW;
            return -1;
        }
        body_len += field_len + 1;
    }

    if (body_len == 0 || body_len > MAX_BODY_LENGTH_FIELD) {
        errno = EINVAL;
        return -1;
    }

    header_len = snprintf(header, sizeof(header), "1|%s|%zu|", code, body_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header)) {
        errno = EINVAL;
        return -1;
    }

    if ((size_t)header_len > SIZE_MAX - body_len) {
        errno = EOVERFLOW;
        return -1;
    }
    total_len = (size_t)header_len + body_len;

    message = malloc(total_len);
    if (message == NULL) {
        return -1;
    }

    memcpy(message, header, (size_t)header_len);
    offset = (size_t)header_len;
    for (i = 0; i < field_count; i++) {
        field_len = strlen(fields[i]);
        memcpy(message + offset, fields[i], field_len);
        offset += field_len;
        message[offset] = '|';
        offset += 1;
    }

    *out_message = message;
    *out_len = total_len;
    return 0;
}

/**
 * Send an exact byte sequence to a socket.
 *
 * @param fd Socket file descriptor.
 * @param buffer Byte buffer to write.
 * @param len Number of bytes to send.
 * @return 0 on success, -1 on send failure.
 */
static int send_all_bytes(int fd, const char *buffer, size_t len) {
    size_t sent_total;
    ssize_t sent_now;

    sent_total = 0;
    while (sent_total < len) {
        sent_now = send(fd, buffer + sent_total, len - sent_total, 0);
        if (sent_now > 0) {
            sent_total += (size_t)sent_now;
            continue;
        }
        if (sent_now < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }

    return 0;
}

/**
 * Serialize and send a protocol v1 message.
 *
 * @param fd Destination socket file descriptor.
 * @param code Three-character protocol message code.
 * @param fields Message body fields.
 * @param field_count Number of message fields.
 * @return 0 on success, -1 on serialization/send failure.
 */
int send_protocol_message_v1(int fd, const char code[4], const char *const *fields, size_t field_count) {
    char *message;
    size_t message_len;
    int rc;

    message = NULL;
    message_len = 0;
    if (build_protocol_message_v1(code, fields, field_count, &message, &message_len) != 0) {
        return -1;
    }

    rc = send_all_bytes(fd, message, message_len);
    free(message);
    return rc;
}

/**
 * Send a protocol MSG frame (sender, recipient, body).
 *
 * @param fd Destination socket file descriptor.
 * @param sender Message sender field.
 * @param recipient Message recipient field.
 * @param body Message body field.
 * @return 0 on success, -1 on serialization/send failure.
 */
int send_protocol_msg_v1(int fd, const char *sender, const char *recipient, const char *body) {
    const char *fields[3];

    fields[0] = sender;
    fields[1] = recipient;
    fields[2] = body;
    return send_protocol_message_v1(fd, "MSG", fields, 3);
}

/**
 * Send a protocol ERR frame (numeric error code, explanation).
 *
 * @param fd Destination socket file descriptor.
 * @param error_code Numeric protocol error code.
 * @param explanation Human-readable error explanation.
 * @return 0 on success, -1 on serialization/send failure.
 */
int send_protocol_err_v1(int fd, unsigned int error_code, const char *explanation) {
    char code_field[16];
    int n;
    const char *fields[2];

    n = snprintf(code_field, sizeof(code_field), "%u", error_code);
    if (n <= 0 || (size_t)n >= sizeof(code_field)) {
        errno = EINVAL;
        return -1;
    }

    fields[0] = code_field;
    fields[1] = explanation;
    return send_protocol_message_v1(fd, "ERR", fields, 2);
}

/**
 * Send protocol error 0 ("Unreadable") to a client.
 *
 * @param client Destination client.
 * @return 0 on success, -1 on send failure.
 */
static int send_unreadable_error(const client_t *client) {
    return send_protocol_err_v1(client->fd, 0U, "Unreadable");
}

/**
 * Return whether a byte is legal in a screen name.
 *
 * Allowed characters: letters, digits, hyphen, underscore.
 *
 * @param c Character byte to check.
 * @return 1 if legal, 0 otherwise.
 */
static int is_screen_name_char(char c) {
    if (c >= 'a' && c <= 'z') {
        return 1;
    }
    if (c >= 'A' && c <= 'Z') {
        return 1;
    }
    if (c >= '0' && c <= '9') {
        return 1;
    }
    if (c == '-' || c == '_') {
        return 1;
    }

    return 0;
}

/**
 * Validate screen-name constraints from the protocol spec.
 *
 * @param name Candidate screen-name bytes.
 * @param name_len Length in bytes.
 * @return 1 if valid, 0 otherwise.
 */
static int validate_screen_name(const char *name, size_t name_len) {
    size_t i;

    if (name_len < 1 || name_len > MAX_SCREEN_NAME_LEN) {
        return 0;
    }

    for (i = 0; i < name_len; i++) {
        if (!is_screen_name_char(name[i])) {
            return 0;
        }
    }

    return 1;
}

/**
 * Return whether a byte is legal in a status string.
 *
 * Allowed range is ASCII 32..126 inclusive.
 *
 * @param c Character byte to check.
 * @return 1 if legal, 0 otherwise.
 */
static int is_status_char(char c) {
    unsigned char uc;

    uc = (unsigned char)c;
    return (uc >= 32U && uc <= 126U) ? 1 : 0;
}

/**
 * Validate status constraints from the protocol spec.
 *
 * @param status Candidate status bytes.
 * @param status_len Length in bytes.
 * @return 1 if valid, 0 otherwise.
 */
static int validate_status(const char *status, size_t status_len) {
    size_t i;

    if (status_len > MAX_STATUS_LEN) {
        return 0;
    }

    for (i = 0; i < status_len; i++) {
        if (!is_status_char(status[i])) {
            return 0;
        }
    }

    return 1;
}

/**
 * Validate recipient syntax for client MSG commands.
 *
 * Recipient must be "#all" or a valid screen name.
 *
 * @param recipient Recipient bytes.
 * @param recipient_len Recipient length in bytes.
 * @return 1 if syntactically valid, 0 otherwise.
 */
static int validate_msg_recipient(const char *recipient, size_t recipient_len) {
    static const char room_all[] = "#all";

    if (recipient_len == sizeof(room_all) - 1 && memcmp(recipient, room_all, sizeof(room_all) - 1) == 0) {
        return 1;
    }

    return validate_screen_name(recipient, recipient_len);
}

/**
 * Validate user-message body constraints from the protocol spec.
 *
 * @param message Message text bytes.
 * @param message_len Message length in bytes.
 * @return 1 if valid, 0 otherwise.
 */
static int validate_user_message(const char *message, size_t message_len) {
    size_t i;

    if (message_len < 1 || message_len > MAX_USER_MESSAGE_LEN) {
        return 0;
    }

    for (i = 0; i < message_len; i++) {
        if (!is_status_char(message[i])) {
            return 0;
        }
    }

    return 1;
}

/**
 * Placeholder NAM handler.
 *
 * @param client Requesting client.
 * @param body Message body bytes, including trailing delimiter.
 * @param body_len Length of body in bytes.
 * @return 0 on success, -1 on protocol/processing failure.
 */
static int is_name_in_use(
    const client_t *clients, nfds_t client_count, const client_t *self, const char *name, size_t name_len
) {
    nfds_t i;
    size_t existing_len;

    for (i = 0; i < client_count; i++) {
        if (&clients[i] == self) {
            continue;
        }
        if (clients[i].screen_name[0] == '\0') {
            continue;
        }

        existing_len = strlen(clients[i].screen_name);
        if (existing_len != name_len) {
            continue;
        }
        if (memcmp(clients[i].screen_name, name, name_len) == 0) {
            return 1;
        }
    }

    return 0;
}

static int handle_nam(client_t *client, const client_t *clients, nfds_t client_count, const char *body, size_t body_len) {
    size_t name_len;

    if (body_len < 1) {
        (void)send_unreadable_error(client);
        return -1;
    }

    /* NAM has one field, so the body format is "<screen_name>|". */
    if (body[body_len - 1] != '|') {
        (void)send_unreadable_error(client);
        return -1;
    }

    name_len = body_len - 1;
    if (!validate_screen_name(body, name_len)) {
        return -1;
    }

    if (is_name_in_use(clients, client_count, client, body, name_len)) {
        if (send_protocol_err_v1(client->fd, 1U, "Name in use") != 0) {
            return -1;
        }
        return 0;
    }

    memcpy(client->screen_name, body, name_len);
    client->screen_name[name_len] = '\0';
    /* In this project, authenticated users are members of the single room #all. */
    client->is_authenticated = 1;

    if (send_protocol_msg_v1(client->fd, "#all", client->screen_name, "Welcome to the chat!") != 0) {
        return -1;
    }

    return 0;
}

/**
 * Broadcast a status-change announcement to authenticated #all members.
 *
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param screen_name Name of the user whose status changed.
 * @param status New status text.
 * @return 0 on success, -1 on formatting failure.
 */
static int broadcast_status_change(
    const client_t *clients, nfds_t client_count, const char *screen_name, const char *status
) {
    nfds_t i;
    char announcement[160];
    int n;

    n = snprintf(announcement, sizeof(announcement), "%s is now \"%s\"", screen_name, status);
    if (n <= 0 || (size_t)n >= sizeof(announcement)) {
        return -1;
    }

    for (i = 0; i < client_count; i++) {
        if (!clients[i].is_authenticated) {
            continue;
        }

        /*
         * Best-effort broadcast: if one recipient socket is stale, keep
         * serving the sender and remaining recipients.
         */
        (void)send_protocol_msg_v1(clients[i].fd, "#all", "#all", announcement);
    }

    return 0;
}

/**
 * Validate SET payload constraints and apply status update.
 *
 * @param client Requesting client.
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param body Message body bytes, including trailing delimiter.
 * @param body_len Length of body in bytes.
 * @return 0 on success, -1 on protocol/processing failure.
 */
static int handle_set(
    client_t *client, const client_t *clients, nfds_t client_count, const char *body, size_t body_len
) {
    size_t status_len;

    if (body_len < 1) {
        (void)send_unreadable_error(client);
        return -1;
    }

    /* SET has one field, so the body format is "<status>|". */
    if (body[body_len - 1] != '|') {
        (void)send_unreadable_error(client);
        return -1;
    }

    status_len = body_len - 1;
    if (status_len > MAX_STATUS_LEN) {
        if (send_protocol_err_v1(client->fd, 4U, "Too long") != 0) {
            return -1;
        }
        return 0;
    }

    if (!validate_status(body, status_len)) {
        if (send_protocol_err_v1(client->fd, 3U, "Illegal character") != 0) {
            return -1;
        }
        return 0;
    }

    memcpy(client->status, body, status_len);
    client->status[status_len] = '\0';

    if (status_len > 0 && client->is_authenticated) {
        if (broadcast_status_change(clients, client_count, client->screen_name, client->status) != 0) {
            return -1;
        }
    }

    return 0;
}

/**
 * Broadcast a room message to authenticated #all members.
 *
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param sender Sender screen name.
 * @param message Message body.
 * @return 0 on success, -1 if message formatting/sending fails critically.
 */
static int broadcast_room_message(
    const client_t *clients, nfds_t client_count, const char *sender, const char *message
) {
    static const char room_all[] = "#all";
    nfds_t i;

    for (i = 0; i < client_count; i++) {
        if (!clients[i].is_authenticated) {
            continue;
        }

        /*
         * Best-effort fanout to room members; one stale socket should not
         * block delivery to others.
         */
        (void)send_protocol_msg_v1(clients[i].fd, sender, room_all, message);
    }

    return 0;
}

/**
 * Find an authenticated client by exact screen name.
 *
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param name Screen name bytes.
 * @param name_len Screen name length in bytes.
 * @return Matching client pointer, or NULL if no active user has that name.
 */
static const client_t *find_client_by_name(
    const client_t *clients, nfds_t client_count, const char *name, size_t name_len
) {
    nfds_t i;
    size_t existing_len;

    for (i = 0; i < client_count; i++) {
        if (!clients[i].is_authenticated) {
            continue;
        }

        existing_len = strlen(clients[i].screen_name);
        if (existing_len != name_len) {
            continue;
        }
        if (memcmp(clients[i].screen_name, name, name_len) == 0) {
            return &clients[i];
        }
    }

    return NULL;
}

/**
 * Validate MSG payload constraints and handle delivery.
 *
 * The client-provided sender field is treated as untrusted input and is
 * ignored when forwarding. Forwarded messages always use the authenticated
 * connection identity (client->screen_name).
 *
 * @param client Requesting client.
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param body Message body bytes, including trailing delimiter.
 * @param body_len Length of body in bytes.
 * @return 0 on success, -1 on protocol/processing failure.
 */
static int handle_msg(
    client_t *client, const client_t *clients, nfds_t client_count, const char *body, size_t body_len
) {
    static const char room_all[] = "#all";
    const char *first_sep;
    const char *second_sep;
    const char *claimed_sender;
    const char *recipient;
    const char *message;
    const char *body_end;
    size_t claimed_sender_len;
    size_t recipient_len;
    size_t message_len;
    char recipient_field[MAX_SCREEN_NAME_LEN + 1];
    char message_field[MAX_USER_MESSAGE_LEN + 1];
    const client_t *target_client;
    const char *effective_sender;

    if (body_len < 1) {
        (void)send_unreadable_error(client);
        return -1;
    }
    if (body[body_len - 1] != '|') {
        (void)send_unreadable_error(client);
        return -1;
    }

    body_end = body + body_len;
    first_sep = memchr(body, '|', body_len);
    if (first_sep == NULL) {
        (void)send_unreadable_error(client);
        return -1;
    }

    /*
     * MSG body is "sender|recipient|message|"; message may include '|',
     * so only the first two delimiters are structural.
     */
    second_sep = memchr(first_sep + 1, '|', (size_t)((body_end - 1) - (first_sep + 1)));
    if (second_sep == NULL) {
        (void)send_unreadable_error(client);
        return -1;
    }

    claimed_sender = body;
    claimed_sender_len = (size_t)(first_sep - claimed_sender);
    recipient = first_sep + 1;
    recipient_len = (size_t)(second_sep - recipient);
    message = second_sep + 1;
    message_len = (size_t)((body_end - 1) - message);

    /*
     * Spoof prevention: never trust the sender field supplied by the client.
     * We still parse it for framing correctness, but always forward using the
     * authenticated sender identity for this socket.
     */
    (void)claimed_sender;
    (void)claimed_sender_len;
    effective_sender = client->screen_name;

    if (!validate_msg_recipient(recipient, recipient_len)) {
        if (send_protocol_err_v1(client->fd, 3U, "Illegal character") != 0) {
            return -1;
        }
        return 0;
    }

    if (!validate_user_message(message, message_len)) {
        if (message_len > MAX_USER_MESSAGE_LEN) {
            if (send_protocol_err_v1(client->fd, 4U, "Too long") != 0) {
                return -1;
            }
            return 0;
        }

        if (send_protocol_err_v1(client->fd, 3U, "Illegal character") != 0) {
            return -1;
        }
        return 0;
    }

    memcpy(message_field, message, message_len);
    message_field[message_len] = '\0';

    if (recipient_len == sizeof(room_all) - 1 && memcmp(recipient, room_all, sizeof(room_all) - 1) == 0) {
        if (broadcast_room_message(clients, client_count, effective_sender, message_field) != 0) {
            return -1;
        }
        return 0;
    }

    memcpy(recipient_field, recipient, recipient_len);
    recipient_field[recipient_len] = '\0';

    target_client = find_client_by_name(clients, client_count, recipient, recipient_len);
    if (target_client == NULL) {
        if (send_protocol_err_v1(client->fd, 2U, "Unknown recipient") != 0) {
            return -1;
        }
        return 0;
    }

    if (send_protocol_msg_v1(target_client->fd, effective_sender, recipient_field, message_field) != 0) {
        return -1;
    }

    return 0;
}

/**
 * Handle WHO queries for a specific user or for #all.
 *
 * @param client Requesting client.
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param body Message body bytes, including trailing delimiter.
 * @param body_len Length of body in bytes.
 * @return 0 on success, -1 on protocol/processing failure.
 */
static int handle_who(
    client_t *client, const client_t *clients, nfds_t client_count, const char *body, size_t body_len
) {
    static const char room_all[] = "#all";
    const char *target;
    size_t target_len;
    const client_t *target_client;
    char response_body[128];
    char line[128];
    char *room_response;
    size_t room_len;
    size_t line_len;
    nfds_t i;
    int n;

    if (body_len < 1) {
        (void)send_unreadable_error(client);
        return -1;
    }
    if (body[body_len - 1] != '|') {
        (void)send_unreadable_error(client);
        return -1;
    }

    target = body;
    target_len = body_len - 1;
    if (target_len == 0) {
        (void)send_unreadable_error(client);
        return -1;
    }

    if (target_len == sizeof(room_all) - 1 && memcmp(target, room_all, sizeof(room_all) - 1) == 0) {
        room_response = malloc((size_t)MAX_BODY_LENGTH_FIELD + 1);
        if (room_response == NULL) {
            return -1;
        }
        room_response[0] = '\0';
        room_len = 0;

        for (i = 0; i < client_count; i++) {
            if (!clients[i].is_authenticated) {
                continue;
            }

            if (clients[i].status[0] != '\0') {
                n = snprintf(line, sizeof(line), "%s: %s", clients[i].screen_name, clients[i].status);
            } else {
                n = snprintf(line, sizeof(line), "%s", clients[i].screen_name);
            }
            if (n <= 0 || (size_t)n >= sizeof(line)) {
                free(room_response);
                return -1;
            }

            line_len = (size_t)n;
            if (room_len > 0) {
                if (room_len + 1 > (size_t)MAX_BODY_LENGTH_FIELD) {
                    free(room_response);
                    if (send_protocol_err_v1(client->fd, 4U, "Too long") != 0) {
                        return -1;
                    }
                    return 0;
                }
                room_response[room_len] = '\n';
                room_len += 1;
            }

            if (room_len + line_len > (size_t)MAX_BODY_LENGTH_FIELD) {
                free(room_response);
                if (send_protocol_err_v1(client->fd, 4U, "Too long") != 0) {
                    return -1;
                }
                return 0;
            }
            memcpy(room_response + room_len, line, line_len);
            room_len += line_len;
            room_response[room_len] = '\0';
        }

        if (send_protocol_msg_v1(client->fd, "#all", client->screen_name, room_response) != 0) {
            free(room_response);
            return -1;
        }

        free(room_response);
        return 0;
    }

    target_client = find_client_by_name(clients, client_count, target, target_len);
    if (target_client == NULL) {
        if (send_protocol_err_v1(client->fd, 2U, "Unknown recipient") != 0) {
            return -1;
        }
        return 0;
    }

    if (target_client->status[0] != '\0') {
        n = snprintf(response_body, sizeof(response_body), "%s: %s", target_client->screen_name, target_client->status);
        if (n <= 0 || (size_t)n >= sizeof(response_body)) {
            return -1;
        }
    } else {
        n = snprintf(response_body, sizeof(response_body), "No status");
        if (n <= 0 || (size_t)n >= sizeof(response_body)) {
            return -1;
        }
    }

    if (send_protocol_msg_v1(client->fd, "#all", client->screen_name, response_body) != 0) {
        return -1;
    }

    return 0;
}

/**
 * Dispatch one complete client frame to the appropriate command handler.
 *
 * @param client Requesting client.
 * @param header Parsed protocol header.
 * @param body Message body bytes, including trailing delimiter.
 * @param body_len Body byte length from the protocol header.
 * @return 0 on successful handling, -1 on unknown/invalid command or handler failure.
 */
static int dispatch_client_command(
    client_t *client,
    const client_t *clients,
    nfds_t client_count,
    const protocol_header_t *header,
    const char *body,
    size_t body_len
) {
    if (strcmp(header->code, "NAM") == 0) {
        return handle_nam(client, clients, client_count, body, body_len);
    }
    if (strcmp(header->code, "SET") == 0) {
        return handle_set(client, clients, client_count, body, body_len);
    }
    if (strcmp(header->code, "MSG") == 0) {
        return handle_msg(client, clients, client_count, body, body_len);
    }
    if (strcmp(header->code, "WHO") == 0) {
        return handle_who(client, clients, client_count, body, body_len);
    }

    (void)send_unreadable_error(client);
    return -1;
}

/**
 * Remove a consumed byte prefix from a client's input buffer.
 *
 * @param client Client record whose buffer is updated.
 * @param consumed Number of leading bytes to remove.
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
 *
 * @param client Client record whose input buffer is validated.
 * @return 0 if buffered data is valid so far, -1 on framing/dispatch violation.
 */
static int validate_and_consume_frames(client_t *client, const client_t *clients, nfds_t client_count) {
    protocol_header_t header;
    int header_parse_result;
    size_t total_frame_len;
    size_t body_len;
    const char *body;

    while (1) {
        header_parse_result = parse_protocol_header(client, &header);
        if (header_parse_result == HEADER_PARSE_INCOMPLETE) {
            return 0;
        }
        if (header_parse_result == HEADER_PARSE_INVALID) {
            (void)send_unreadable_error(client);
            return -1;
        }

        if (header.version != 1U) {
            (void)send_unreadable_error(client);
            return -1;
        }

        if (header.header_len > sizeof(client->input_buffer)) {
            (void)send_unreadable_error(client);
            return -1;
        }

        body_len = (size_t)header.body_len;
        if (body_len == 0 || body_len > sizeof(client->input_buffer) - header.header_len) {
            (void)send_unreadable_error(client);
            return -1;
        }

        total_frame_len = header.header_len + body_len;
        if (client->input_len < total_frame_len) {
            return 0;
        }

        if (client->input_buffer[total_frame_len - 1] != '|') {
            (void)send_unreadable_error(client);
            return -1;
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
 *
 * @param client Client record to update.
 * @return 0 on successful read/no-op, 1 if client should be disconnected,
 *         -1 on fatal read error.
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
 *
 * @param listen_fd Listening socket file descriptor.
 * @return 0 on clean shutdown, -1 on fatal polling error.
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
                remove_client(clients, &client_count, i - 1);
                remove_client_fd(pfds, &count, i);
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
                    remove_client(clients, &client_count, i - 1);
                    remove_client_fd(pfds, &count, i);
                    i -= 1;
                    continue;
                }

                if (validate_and_consume_frames(client, clients, client_count) != 0) {
                    remove_client(clients, &client_count, i - 1);
                    remove_client_fd(pfds, &count, i);
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
