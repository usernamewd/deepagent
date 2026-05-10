CC := gcc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Iinclude -Ivendor
LDFLAGS := -lcurl -lm

TARGET := build/deepagent
SRC := $(wildcard src/*.c) vendor/cJSON.c
OBJ := $(patsubst src/%.c,build/%.o,$(filter src/%.c,$(SRC))) build/cJSON.o

.PHONY: all clean install vendor-setup

all: $(TARGET)

$(TARGET): vendor/cJSON.h vendor/cJSON.c $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/cJSON.o: vendor/cJSON.c vendor/cJSON.h | build
	$(CC) $(CFLAGS) -c vendor/cJSON.c -o $@

vendor/cJSON.h vendor/cJSON.c: scripts/setup.sh
	@if [ ! -s vendor/cJSON.h ] || [ ! -s vendor/cJSON.c ] || ! grep -q "CJSON_PUBLIC" vendor/cJSON.h; then \
		sh scripts/setup.sh; \
	fi

vendor-setup:
	sh scripts/setup.sh

install: all
	install -m 0755 $(TARGET) /usr/local/bin/deepagent

clean:
	rm -rf build
