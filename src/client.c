#include "client.h"

#include <stdlib.h>
#include <string.h>

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
int add_client(client_t **clients, nfds_t *count, nfds_t *cap, int fd) {
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
void remove_client(client_t *clients, nfds_t *count, nfds_t idx) {
    nfds_t i;

    for (i = idx; i + 1 < *count; i++) {
        clients[i] = clients[i + 1];
    }
    *count -= 1;
}

/**
 * Clear per-client session state before removing the client entry.
 *
 * @param client Client record to clear.
 */
void clear_client_state(client_t *client) {
    client->is_authenticated = 0;
    client->screen_name[0] = '\0';
    client->status[0] = '\0';
    client->input_len = 0;
}

/**
 * Return whether a name is already assigned to another connected client.
 *
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param self Client requesting the name.
 * @param name Candidate name bytes.
 * @param name_len Candidate name length.
 * @return 1 if the name is in use by another client, 0 otherwise.
 */
int is_name_in_use(
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

/**
 * Find an authenticated client by exact screen name.
 *
 * @param clients Client registry.
 * @param client_count Number of active clients.
 * @param name Screen name bytes.
 * @param name_len Screen name length in bytes.
 * @return Matching client pointer, or NULL if no active user has that name.
 */
const client_t *find_client_by_name(
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
