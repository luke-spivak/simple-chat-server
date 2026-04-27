#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Include production code directly so these tests can exercise the
 * parser/dispatch/handler pipeline without expanding public headers yet.
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
    assert_true((size_t)(p2 - (p1 + 1)) == 3, "response code must be 3 characters");
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
    char body[1024];

    read_protocol_frame(fd, code, sizeof(code), body, sizeof(body));
    assert_true(strcmp(code, want_code) == 0, "unexpected reply code");
    assert_true(strcmp(body, want_body) == 0, "unexpected reply body");
}

static void queue_client_frame(client_t *client, const char *frame) {
    size_t len;

    len = strlen(frame);
    assert_true(len <= sizeof(client->input_buffer), "test frame too large for input buffer");
    memcpy(client->input_buffer, frame, len);
    client->input_len = len;
}

static void init_client(client_t *client, int fd) {
    memset(client, 0, sizeof(*client));
    client->fd = fd;
}

static void test_valid_login(void) {
    int fds[2];
    client_t clients[1];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should succeed");
    init_client(&clients[0], fds[0]);

    queue_client_frame(&clients[0], "1|NAM|4|Bob|");
    rc = validate_and_consume_frames(&clients[0], clients, 1);
    assert_true(rc == 0, "valid NAM should be recoverable/successful");
    expect_reply(fds[1], "MSG", "#all|Bob|Welcome to the chat!|");
    assert_true(clients[0].is_authenticated == 1, "client should be authenticated after valid NAM");
    assert_true(strcmp(clients[0].screen_name, "Bob") == 0, "screen name should be stored");

    close(fds[0]);
    close(fds[1]);
}

static void test_duplicate_name(void) {
    int fds1[2];
    int fds2[2];
    client_t clients[2];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0, "socketpair #1 should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0, "socketpair #2 should succeed");
    init_client(&clients[0], fds1[0]);
    init_client(&clients[1], fds2[0]);

    clients[0].is_authenticated = 1;
    strcpy(clients[0].screen_name, "Bob");

    queue_client_frame(&clients[1], "1|NAM|4|Bob|");
    rc = validate_and_consume_frames(&clients[1], clients, 2);
    assert_true(rc == 0, "duplicate NAM should be recoverable");
    expect_reply(fds2[1], "ERR", "1|Name in use|");
    assert_true(clients[1].is_authenticated == 0, "duplicate login should not authenticate second client");

    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
}

static void test_invalid_name_illegal_character(void) {
    int fds[2];
    client_t clients[1];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should succeed");
    init_client(&clients[0], fds[0]);

    queue_client_frame(&clients[0], "1|NAM|4|Bo!|");
    rc = validate_and_consume_frames(&clients[0], clients, 1);
    assert_true(rc == 0, "illegal-char NAM should be recoverable");
    expect_reply(fds[1], "ERR", "3|Illegal character|");
    assert_true(clients[0].is_authenticated == 0, "invalid NAM should not authenticate client");

    close(fds[0]);
    close(fds[1]);
}

static void test_invalid_name_too_long(void) {
    int fds[2];
    client_t clients[1];
    char frame[128];
    char name[40];
    int n;
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should succeed");
    init_client(&clients[0], fds[0]);

    memset(name, 'a', 33);
    name[33] = '\0';
    n = snprintf(frame, sizeof(frame), "1|NAM|34|%s|", name);
    assert_true(n > 0 && (size_t)n < sizeof(frame), "too-long NAM frame should fit test buffer");

    queue_client_frame(&clients[0], frame);
    rc = validate_and_consume_frames(&clients[0], clients, 1);
    assert_true(rc == 0, "too-long NAM should be recoverable");
    expect_reply(fds[1], "ERR", "4|Too long|");
    assert_true(clients[0].is_authenticated == 0, "too-long NAM should not authenticate client");

    close(fds[0]);
    close(fds[1]);
}

static void test_name_reuse_after_disconnect(void) {
    int fds1[2];
    int fds2[2];
    client_t clients[2];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) == 0, "socketpair #1 should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) == 0, "socketpair #2 should succeed");
    init_client(&clients[0], fds1[0]);
    init_client(&clients[1], fds2[0]);

    queue_client_frame(&clients[0], "1|NAM|4|Bob|");
    rc = validate_and_consume_frames(&clients[0], clients, 2);
    assert_true(rc == 0, "initial NAM should succeed");
    expect_reply(fds1[1], "MSG", "#all|Bob|Welcome to the chat!|");

    clear_client_state(&clients[0]);

    queue_client_frame(&clients[1], "1|NAM|4|Bob|");
    rc = validate_and_consume_frames(&clients[1], clients, 2);
    assert_true(rc == 0, "name should be reusable after disconnect cleanup");
    expect_reply(fds2[1], "MSG", "#all|Bob|Welcome to the chat!|");

    close(fds1[0]);
    close(fds1[1]);
    close(fds2[0]);
    close(fds2[1]);
}

int main(void) {
    test_valid_login();
    test_duplicate_name();
    test_invalid_name_illegal_character();
    test_invalid_name_too_long();
    test_name_reuse_after_disconnect();

    printf("PASS: %d NAM integration assertions\n", tests_run);
    return 0;
}
