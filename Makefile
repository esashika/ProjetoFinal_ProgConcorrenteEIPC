CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -pthread
TARGET = build/pizzaria
SRC = src/pizzaria.c

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf build
