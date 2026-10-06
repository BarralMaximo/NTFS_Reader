CC       ?= gcc
CFLAGS   ?= -O2 -g
CFLAGS   += -std=c11 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -Iinclude -MMD -MP

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/%.o)
BIN := build/ntfsread

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

build:
	mkdir -p build

test: $(BIN)
	tests/run.sh

clean:
	rm -rf build

-include $(OBJ:.o=.d)

.PHONY: all test clean
