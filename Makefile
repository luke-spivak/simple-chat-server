CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

TARGET := chatd
SRC := src/chatd.c
OBJ := $(SRC:.c=.o)
TEST_BIN := tests/test_protocol
TEST_SRC := tests/test_protocol.c
INTEG_TEST_BIN := tests/test_integration_nam
INTEG_TEST_SRC := tests/test_integration_nam.c
CHAT_INTEG_TEST_BIN := tests/test_integration_chat
CHAT_INTEG_TEST_SRC := tests/test_integration_chat.c

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(INTEG_TEST_BIN): $(INTEG_TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(CHAT_INTEG_TEST_BIN): $(CHAT_INTEG_TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $<

test: all $(TEST_BIN) $(INTEG_TEST_BIN) $(CHAT_INTEG_TEST_BIN)
	./$(TEST_BIN)
	./$(INTEG_TEST_BIN)
	./$(CHAT_INTEG_TEST_BIN)

clean:
	rm -f $(TARGET) $(OBJ) $(TEST_BIN) $(INTEG_TEST_BIN) $(CHAT_INTEG_TEST_BIN)
