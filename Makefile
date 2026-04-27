CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

TARGET := chatd
SRC := src/chatd.c
OBJ := $(SRC:.c=.o)
TEST_BIN := tests/test_protocol
TEST_SRC := tests/test_protocol.c

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_BIN): $(TEST_SRC)
	$(CC) $(CFLAGS) -o $@ $<

test: all $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(TARGET) $(OBJ) $(TEST_BIN)
