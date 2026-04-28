#ifndef CLIENT_H
#define CLIENT_H

#include <poll.h>
#include <stddef.h>

#define MAX_SCREEN_NAME_LEN 32
#define MAX_STATUS_LEN 64
#define CLIENT_INPUT_CAPACITY 4096

typedef struct {
    int fd;
    int is_authenticated;
    char screen_name[MAX_SCREEN_NAME_LEN + 1];
    char status[MAX_STATUS_LEN + 1];
    char input_buffer[CLIENT_INPUT_CAPACITY];
    size_t input_len;
} client_t;

int add_client(client_t **clients, nfds_t *count, nfds_t *cap, int fd);
void remove_client(client_t *clients, nfds_t *count, nfds_t idx);
void clear_client_state(client_t *client);

int is_name_in_use(
    const client_t *clients, nfds_t client_count, const client_t *self, const char *name, size_t name_len
);

const client_t *find_client_by_name(
    const client_t *clients, nfds_t client_count, const char *name, size_t name_len
);

#endif
