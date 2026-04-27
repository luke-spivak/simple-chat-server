#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Include production code directly so these tests can exercise parser +
 * dispatch + handlers without broadening public headers yet.
 */
#define static
#define main chatd_program_main
#include "../src/chatd.c"
#undef main
#undef static

static int tests_run = 0;

static void assert_true(int condition, const char *message) {
    tests_run += 1;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void recv_exact(int fd, char *out, size_t len) {
    size_t got;
    ssize_t rc;

    got = 0;
    while (got < len) {
        rc = recv(fd, out + got, len - got, 0);
        if (rc > 0) {
            got += (size_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        fprintf(stderr, "FAIL: recv_exact failed\n");
        exit(1);
    }
}

static void read_protocol_frame(int fd, char *out_code, size_t out_code_cap, char *out_body, size_t out_body_cap) {
    char header[128];
    size_t header_len;
    int bars;
    char ch;
    char *p1;
    char *p2;
    char *p3;
    char saved;
    unsigned long body_len_ul;
    size_t body_len;

    header_len = 0;
    bars = 0;
    while (bars < 3) {
        recv_exact(fd, &ch, 1);
        if (header_len + 1 >= sizeof(header)) {
            fprintf(stderr, "FAIL: header too long\n");
            exit(1);
        }
        header[header_len++] = ch;
        if (ch == '|') {
            bars += 1;
        }
    }
    header[header_len] = '\0';

    p1 = strchr(header, '|');
    p2 = (p1 == NULL) ? NULL : strchr(p1 + 1, '|');
    p3 = (p2 == NULL) ? NULL : strchr(p2 + 1, '|');
    assert_true(p1 != NULL && p2 != NULL && p3 != NULL, "response header must contain 3 delimiters");
    assert_true((size_t)(p2 - (p1 + 1)) == 3, "response code must be 3 chars");
    assert_true(out_code_cap >= 4, "out_code buffer must hold 3-char code");

    memcpy(out_code, p1 + 1, 3);
    out_code[3] = '\0';

    saved = *p3;
    *p3 = '\0';
    body_len_ul = strtoul(p2 + 1, NULL, 10);
    *p3 = saved;
    body_len = (size_t)body_len_ul;
    assert_true(body_len > 0, "response body length must be positive");
    assert_true(body_len + 1 <= out_body_cap, "response body buffer too small");

    recv_exact(fd, out_body, body_len);
    out_body[body_len] = '\0';
}

static void expect_reply(int fd, const char *want_code, const char *want_body) {
    char code[4];
    char body[2048];

    read_protocol_frame(fd, code, sizeof(code), body, sizeof(body));
    assert_true(strcmp(code, want_code) == 0, "unexpected reply code");
    assert_true(strcmp(body, want_body) == 0, "unexpected reply body");
}

static void queue_client_frame(client_t *client, const char *frame) {
    size_t len;

    len = strlen(frame);
    assert_true(len <= sizeof(client->input_buffer), "test frame too large");
    memcpy(client->input_buffer, frame, len);
    client->input_len = len;
}

static void init_client(client_t *client, int fd, const char *name) {
    memset(client, 0, sizeof(*client));
    client->fd = fd;
    client->is_authenticated = 1;
    strcpy(client->screen_name, name);
}

static void test_set_status_broadcast(void) {
    int fds1[2];
    int fds2[2];
    client_t clients[2];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0, "socketpair #1 should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0, "socketpair #2 should succeed");
    init_client(&clients[0], fds1[0], "Bob");
    init_client(&clients[1], fds2[0], "Alice");

    queue_client_frame(&clients[0], "1|SET|6|happy|");
    rc = validate_and_consume_frames(&clients[0], clients, 2);
    assert_true(rc == 0, "SET should process successfully");
    assert_true(strcmp(clients[0].status, "happy") == 0, "SET should persist sender status");

    expect_reply(fds1[1], "MSG", "#all|#all|Bob is now \"happy\"|");
    expect_reply(fds2[1], "MSG", "#all|#all|Bob is now \"happy\"|");

    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
}

static void test_msg_room_fanout_and_spoof_prevention(void) {
    int fds1[2];
    int fds2[2];
    int fds3[2];
    client_t clients[3];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0, "socketpair #1 should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0, "socketpair #2 should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds3) == 0, "socketpair #3 should succeed");
    init_client(&clients[0], fds1[0], "Bob");
    init_client(&clients[1], fds2[0], "Alice");
    init_client(&clients[2], fds3[0], "Carol");

    /*
     * Claimed sender is Mallory, but forwarded sender must be Bob.
     */
    queue_client_frame(&clients[0], "1|MSG|24|Mallory|#all|hello room|");
    rc = validate_and_consume_frames(&clients[0], clients, 3);
    assert_true(rc == 0, "room MSG should process successfully");

    expect_reply(fds1[1], "MSG", "Bob|#all|hello room|");
    expect_reply(fds2[1], "MSG", "Bob|#all|hello room|");
    expect_reply(fds3[1], "MSG", "Bob|#all|hello room|");

    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
    close(fds3[0]);
    close(fds3[1]);
}

static void test_msg_private_delivery_and_unknown_recipient(void) {
    int fds1[2];
    int fds2[2];
    client_t clients[2];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0, "socketpair #1 should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0, "socketpair #2 should succeed");
    init_client(&clients[0], fds1[0], "Bob");
    init_client(&clients[1], fds2[0], "Alice");

    queue_client_frame(&clients[0], "1|MSG|24|Mallory|Alice|secret hi|");
    rc = validate_and_consume_frames(&clients[0], clients, 2);
    assert_true(rc == 0, "private MSG should process successfully");
    expect_reply(fds2[1], "MSG", "Bob|Alice|secret hi|");

    queue_client_frame(&clients[0], "1|MSG|20|Mallory|Zed|nowhere|");
    rc = validate_and_consume_frames(&clients[0], clients, 2);
    assert_true(rc == 0, "unknown-recipient MSG should be recoverable");
    expect_reply(fds1[1], "ERR", "2|Unknown recipient|");

    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
}

int main(void) {
    test_set_status_broadcast();
    test_msg_room_fanout_and_spoof_prevention();
    test_msg_private_delivery_and_unknown_recipient();

    printf("PASS: %d SET/MSG integration assertions\n", tests_run);
    return 0;
}
