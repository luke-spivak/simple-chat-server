#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Include production code directly so we can test parser/dispatch/handler
 * behavior end-to-end without changing public headers.
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
            fprintf(stderr, "FAIL: response header too long\n");
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
    assert_true((size_t)(p2 - (p1 + 1)) == 3, "response code length must be 3");
    assert_true(out_code_cap >= 4, "out_code buffer must hold code");

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
    char body[4096];

    read_protocol_frame(fd, code, sizeof(code), body, sizeof(body));
    assert_true(strcmp(code, want_code) == 0, "unexpected response code");
    assert_true(strcmp(body, want_body) == 0, "unexpected response body");
}

static void queue_client_frame(client_t *client, const char *frame) {
    size_t len;

    len = strlen(frame);
    assert_true(len <= sizeof(client->input_buffer), "test frame too large");
    memcpy(client->input_buffer, frame, len);
    client->input_len = len;
}

static void init_authenticated_client(client_t *client, int fd, const char *name, const char *status) {
    memset(client, 0, sizeof(*client));
    client->fd = fd;
    client->is_authenticated = 1;
    strcpy(client->screen_name, name);
    if (status != NULL) {
        strcpy(client->status, status);
    }
}

static void test_who_specific_with_status(void) {
    int fds_req[2];
    int fds_target[2];
    client_t clients[2];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_req) == 0, "socketpair requester should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_target) == 0, "socketpair target should succeed");
    init_authenticated_client(&clients[0], fds_req[0], "Alice", "");
    init_authenticated_client(&clients[1], fds_target[0], "Bob", "busy");

    queue_client_frame(&clients[0], "1|WHO|4|Bob|");
    rc = validate_and_consume_frames(&clients[0], clients, 2);
    assert_true(rc == 0, "WHO user with status should be recoverable/success");
    expect_reply(fds_req[1], "MSG", "#all|Alice|Bob: busy|");

    close(fds_req[0]);
    close(fds_req[1]);
    close(fds_target[0]);
    close(fds_target[1]);
}

static void test_who_specific_no_status(void) {
    int fds_req[2];
    int fds_target[2];
    client_t clients[2];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_req) == 0, "socketpair requester should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_target) == 0, "socketpair target should succeed");
    init_authenticated_client(&clients[0], fds_req[0], "Alice", "");
    init_authenticated_client(&clients[1], fds_target[0], "Bob", "");

    queue_client_frame(&clients[0], "1|WHO|4|Bob|");
    rc = validate_and_consume_frames(&clients[0], clients, 2);
    assert_true(rc == 0, "WHO user with no status should be recoverable/success");
    expect_reply(fds_req[1], "MSG", "#all|Alice|No status|");

    close(fds_req[0]);
    close(fds_req[1]);
    close(fds_target[0]);
    close(fds_target[1]);
}

static void test_who_all_listing(void) {
    int fds_a[2];
    int fds_b[2];
    int fds_c[2];
    client_t clients[3];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_a) == 0, "socketpair A should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_b) == 0, "socketpair B should succeed");
    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds_c) == 0, "socketpair C should succeed");
    init_authenticated_client(&clients[0], fds_a[0], "Alice", "");
    init_authenticated_client(&clients[1], fds_b[0], "Bob", "busy");
    init_authenticated_client(&clients[2], fds_c[0], "Carol", "");

    queue_client_frame(&clients[0], "1|WHO|5|#all|");
    rc = validate_and_consume_frames(&clients[0], clients, 3);
    assert_true(rc == 0, "WHO #all should be recoverable/success");
    expect_reply(fds_a[1], "MSG", "#all|Alice|Alice\nBob: busy\nCarol|");

    close(fds_a[0]);
    close(fds_a[1]);
    close(fds_b[0]);
    close(fds_b[1]);
    close(fds_c[0]);
    close(fds_c[1]);
}

static void test_recoverable_error_then_success(void) {
    int fds[2];
    client_t clients[1];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should succeed");
    init_authenticated_client(&clients[0], fds[0], "Alice", "");

    queue_client_frame(&clients[0], "1|WHO|4|Zed|");
    rc = validate_and_consume_frames(&clients[0], clients, 1);
    assert_true(rc == 0, "unknown WHO user should be recoverable");
    expect_reply(fds[1], "ERR", "2|Unknown recipient|");

    queue_client_frame(&clients[0], "1|WHO|6|Alice|");
    rc = validate_and_consume_frames(&clients[0], clients, 1);
    assert_true(rc == 0, "client should continue after recoverable error");
    expect_reply(fds[1], "MSG", "#all|Alice|No status|");

    close(fds[0]);
    close(fds[1]);
}

static void test_fatal_unreadable_err0(void) {
    int fds[2];
    client_t clients[1];
    int rc;

    assert_true(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair should succeed");
    init_authenticated_client(&clients[0], fds[0], "Alice", "");

    queue_client_frame(&clients[0], "1|BAD|4|Bob|");
    rc = validate_and_consume_frames(&clients[0], clients, 1);
    assert_true(rc == -1, "unknown message type should be fatal");
    expect_reply(fds[1], "ERR", "0|Unreadable|");

    close(fds[0]);
    close(fds[1]);
}

int main(void) {
    test_who_specific_with_status();
    test_who_specific_no_status();
    test_who_all_listing();
    test_recoverable_error_then_success();
    test_fatal_unreadable_err0();

    printf("PASS: %d WHO/error integration assertions\n", tests_run);
    return 0;
}
