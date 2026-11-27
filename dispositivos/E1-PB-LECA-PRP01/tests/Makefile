.PHONY: all test clean
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -I../main

all: test

test: build/test_protocolo build/test_config
	./build/test_protocolo
	./build/test_config

build/test_protocolo: test_protocolo.c ../main/protocolo_pi.c ../main/protocolo_pi.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_protocolo.c ../main/protocolo_pi.c

build/test_config: test_config.c ../main/config_validacion.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ test_config.c -lm

clean:
	rm -rf build
