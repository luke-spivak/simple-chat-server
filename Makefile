CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic

TARGET := chatd
SRC := src/chatd.c
OBJ := $(SRC:.c=.o)

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	@echo "No tests yet."

clean:
	rm -f $(TARGET) $(OBJ)
