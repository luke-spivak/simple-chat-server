Name: Luke Spivak
NetID: ljs302

Simple Chat Server (P4)

This project implements `chatd`, a simple TCP chat server for the CS 214 P4
protocol. The server accepts clients, processes protocol frames, and supports:

- `NAM`: login with unique screen names
- `SET`: status updates
- `MSG`: room (`#all`) and direct user messaging
- `WHO`: single-user and `#all` queries
- `ERR`: protocol and semantic error reporting

## Build

Build the server:

```sh
make all
```

This produces:

- `./chatd`

## Run

Start the server on a chosen port:

```sh
./chatd <port>
```

## Test Execution

Run all automated tests:

```sh
make test
```

Current automated suites:

- `tests/test_protocol.c`
  - protocol serializer and header parser unit tests
- `tests/test_integration_nam.c`
  - integration-style login lifecycle tests (`NAM`)
- `tests/test_integration_chat.c`
  - integration-style `SET` and `MSG` routing tests
- `tests/test_integration_who_errors.c`
  - integration-style `WHO` behavior and error semantics tests

Clean build/test artifacts:

```sh
make clean
```

## Test Plan

The testing strategy combines focused unit checks for framing logic with
integration-style tests that exercise parser -> dispatch -> handler behavior.

### 1) Protocol Framing and Serialization

Goals:

- verify correct encoding format `1|CODE|LEN|...|`
- verify length accounting
- verify header parse completeness/invalid detection

Covered by:

- `test_build_protocol_message_v1_success`
- `test_build_protocol_message_v1_invalid_code`
- `test_parse_protocol_header_complete`
- `test_parse_protocol_header_incomplete`
- `test_parse_protocol_header_invalid_code`
- `test_parse_protocol_header_invalid_length_field`

### 2) Login (`NAM`) Lifecycle

Goals:

- valid login receives welcome message
- duplicate name returns `ERR 1` and remains recoverable
- invalid name characters return `ERR 3`
- overlong names return `ERR 4`
- names are reusable after disconnect cleanup

Covered by:

- `test_valid_login`
- `test_duplicate_name`
- `test_invalid_name_illegal_character`
- `test_invalid_name_too_long`
- `test_name_reuse_after_disconnect`

### 3) Status and Messaging (`SET`, `MSG`)

Goals:

- `SET` persists status and broadcasts non-empty status changes
- room messages to `#all` fan out to authenticated users
- private messages deliver to exactly one recipient
- unknown recipient returns `ERR 2`
- sender spoofing is prevented (forwarded sender = authenticated client)

Covered by:

- `test_set_status_broadcast`
- `test_msg_room_fanout_and_spoof_prevention`
- `test_msg_private_delivery_and_unknown_recipient`

### 4) User Queries and Error Semantics (`WHO`, fatal vs recoverable)

Goals:

- `WHO <user>` returns `name: status` or `No status`
- `WHO #all` returns newline-separated room listing
- recoverable errors (`ERR 1-4`) keep connection usable
- unreadable protocol errors produce `ERR 0` and fatal handling

Covered by:

- `test_who_specific_with_status`
- `test_who_specific_no_status`
- `test_who_all_listing`
- `test_recoverable_error_then_success`
- `test_fatal_unreadable_err0`