#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char **argv) {
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

    (void)port;
    return 0;
}
