#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/protocol.h"

static int tests_run = 0;

static void assert_true(int condition, const char *message) {
    tests_run += 1;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void test_build_protocol_message_v1_success(void) {
    char *message;
    size_t len;
    const char *fields[3] = {"#all", "#all", "hi"};
    int rc;

    message = NULL;
    len = 0;
    rc = build_protocol_message_v1("MSG", fields, 3, &message, &len);
    assert_true(rc == 0, "build_protocol_message_v1 should succeed for valid MSG");
    assert_true(len == strlen("1|MSG|13|#all|#all|hi|"), "serialized MSG length should match expected");
    assert_true(strcmp(message, "1|MSG|13|#all|#all|hi|") == 0, "serialized MSG bytes should match expected");
    free(message);
}

static void test_build_protocol_message_v1_invalid_code(void) {
    char *message;
    size_t len;
    const char *fields[1] = {"Bob"};
    int rc;

    message = NULL;
    len = 0;
    errno = 0;
    rc = build_protocol_message_v1("MS", fields, 1, &message, &len);
    assert_true(rc == -1, "build_protocol_message_v1 should reject non-3-char code");
    assert_true(errno == EINVAL, "invalid code should set errno=EINVAL");
}

static void test_parse_protocol_header_complete(void) {
    protocol_header_t header;
    const char *frame = "1|MSG|13|#all|#all|hi|";
    int rc;

    rc = parse_protocol_header(frame, strlen(frame), &header);
    assert_true(rc == HEADER_PARSE_COMPLETE, "complete header should parse");
    assert_true(header.version == 1U, "parsed version should be 1");
    assert_true(strcmp(header.code, "MSG") == 0, "parsed code should be MSG");
    assert_true(header.body_len == 13U, "parsed body length should be 13");
    assert_true(header.header_len == strlen("1|MSG|13|"), "parsed header length should match");
}

static void test_parse_protocol_header_incomplete(void) {
    protocol_header_t header;
    const char *prefix = "1|MSG|13";
    int rc;

    rc = parse_protocol_header(prefix, strlen(prefix), &header);
    assert_true(rc == HEADER_PARSE_INCOMPLETE, "missing trailing header delimiter should be incomplete");
}

static void test_parse_protocol_header_invalid_code(void) {
    protocol_header_t header;
    const char *frame = "1|MS|4|Bob|";
    int rc;

    rc = parse_protocol_header(frame, strlen(frame), &header);
    assert_true(rc == HEADER_PARSE_INVALID, "non-3-char message code should be invalid");
}

static void test_parse_protocol_header_invalid_length_field(void) {
    protocol_header_t header;
    const char *frame = "1|MSG|A|Bob|";
    int rc;

    rc = parse_protocol_header(frame, strlen(frame), &header);
    assert_true(rc == HEADER_PARSE_INVALID, "non-numeric length field should be invalid");
}

int main(void) {
    test_build_protocol_message_v1_success();
    test_build_protocol_message_v1_invalid_code();
    test_parse_protocol_header_complete();
    test_parse_protocol_header_incomplete();
    test_parse_protocol_header_invalid_code();
    test_parse_protocol_header_invalid_length_field();

    printf("PASS: %d protocol unit tests\n", tests_run);
    return 0;
}
