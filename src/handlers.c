#include "handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_USER_MESSAGE_LEN 80

/**
 * Send protocol error 0 ("Unreadable") to a client.
 */
static int send_unreadable_error(const client_t *client) {
    return send_protocol_err_v1(client->fd, 0U, "Unreadable");
}

/**
 * Report unreadable protocol data and signal fatal disconnect.
 */
int fatal_unreadable(const client_t *client) {
    (void)send_unreadable_error(client);
    return -1;
}

/**
 * Return whether a byte is legal in a screen name.
 *
 * Allowed characters: letters, digits, hyphen, underscore.
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
 */
static int is_status_char(char c) {
    unsigned char uc;

    uc = (unsigned char)c;
    return (uc >= 32U && uc <= 126U) ? 1 : 0;
}

/**
 * Validate status constraints from the protocol spec.
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
 * Validate NAM payload constraints and apply login.
 */
static int handle_nam(client_t *client, const client_t *clients, nfds_t client_count, const char *body, size_t body_len) {
    size_t name_len;

    if (body_len < 1) {
        return fatal_unreadable(client);
    }

    /* NAM has one field, so the body format is "<screen_name>|". */
    if (body[body_len - 1] != '|') {
        return fatal_unreadable(client);
    }

    name_len = body_len - 1;
    if (name_len < 1 || name_len > MAX_SCREEN_NAME_LEN) {
        if (send_protocol_err_v1(client->fd, 4U, "Too long") != 0) {
            return -1;
        }
        return 0;
    }
    if (!validate_screen_name(body, name_len)) {
        if (send_protocol_err_v1(client->fd, 3U, "Illegal character") != 0) {
            return -1;
        }
        return 0;
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
 */
static int handle_set(
    client_t *client, const client_t *clients, nfds_t client_count, const char *body, size_t body_len
) {
    size_t status_len;

    if (body_len < 1) {
        return fatal_unreadable(client);
    }

    /* SET has one field, so the body format is "<status>|". */
    if (body[body_len - 1] != '|') {
        return fatal_unreadable(client);
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
 * Validate MSG payload constraints and handle delivery.
 *
 * The client-provided sender field is treated as untrusted input and is
 * ignored when forwarding. Forwarded messages always use the authenticated
 * connection identity (client->screen_name).
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
        return fatal_unreadable(client);
    }
    if (body[body_len - 1] != '|') {
        return fatal_unreadable(client);
    }

    body_end = body + body_len;
    first_sep = memchr(body, '|', body_len);
    if (first_sep == NULL) {
        return fatal_unreadable(client);
    }

    /*
     * MSG body is "sender|recipient|message|"; message may include '|',
     * so only the first two delimiters are structural.
     */
    second_sep = memchr(first_sep + 1, '|', (size_t)((body_end - 1) - (first_sep + 1)));
    if (second_sep == NULL) {
        return fatal_unreadable(client);
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
        return fatal_unreadable(client);
    }
    if (body[body_len - 1] != '|') {
        return fatal_unreadable(client);
    }

    target = body;
    target_len = body_len - 1;
    if (target_len == 0) {
        return fatal_unreadable(client);
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

    if (target_len > MAX_SCREEN_NAME_LEN) {
        if (send_protocol_err_v1(client->fd, 4U, "Too long") != 0) {
            return -1;
        }
        return 0;
    }
    if (!validate_screen_name(target, target_len)) {
        if (send_protocol_err_v1(client->fd, 3U, "Illegal character") != 0) {
            return -1;
        }
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
 */
int dispatch_client_command(
    client_t *client,
    const client_t *clients,
    nfds_t client_count,
    const protocol_header_t *header,
    const char *body,
    size_t body_len
) {
    if (!client->is_authenticated && strcmp(header->code, "NAM") != 0) {
        return fatal_unreadable(client);
    }

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

    return fatal_unreadable(client);
}
