#ifndef HANDLERS_H
#define HANDLERS_H

#include <poll.h>
#include <stddef.h>

#include "client.h"
#include "protocol.h"

int fatal_unreadable(const client_t *client);

int dispatch_client_command(
    client_t *client,
    const client_t *clients,
    nfds_t client_count,
    const protocol_header_t *header,
    const char *body,
    size_t body_len
);

#endif
