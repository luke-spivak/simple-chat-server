#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

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
 * @param buffer Input bytes containing zero or more protocol frames.
 * @param buffer_len Number of bytes available in buffer.
 * @param out_header Parsed header values on complete success.
 * @return HEADER_PARSE_COMPLETE if all three header fields are available and valid,
 *         HEADER_PARSE_INCOMPLETE if more bytes are needed,
 *         HEADER_PARSE_INVALID if available bytes violate header syntax.
 */
int parse_protocol_header(const char *buffer, size_t buffer_len, protocol_header_t *out_header) {
    const char *cursor;
    const char *sep;
    size_t remaining;
    size_t field_len;

    cursor = buffer;
    remaining = buffer_len;

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

    out_header->header_len = (size_t)(sep - buffer) + 1;
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
