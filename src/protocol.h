#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define MAX_BODY_LENGTH_FIELD 99999

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

int parse_protocol_header(const char *buffer, size_t buffer_len, protocol_header_t *out_header);

int build_protocol_message_v1(
    const char *code,
    const char *const *fields,
    size_t field_count,
    char **out_message,
    size_t *out_len
);

int send_protocol_message_v1(int fd, const char *code, const char *const *fields, size_t field_count);
int send_protocol_msg_v1(int fd, const char *sender, const char *recipient, const char *body);
int send_protocol_err_v1(int fd, unsigned int error_code, const char *explanation);

#endif
