.PHONY: all test clean
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I../main

all: test

test: build/test_protocolo
	./build/test_protocolo

build/test_protocolo: test_protocolo.c ../main/protocolo_pi.c ../main/protocolo_pi.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_protocolo.c ../main/protocolo_pi.c

clean:
	rm -rf build
