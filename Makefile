CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
LDFLAGS ?= -lm
EMCC ?= emcc
WASM_EXPORTS := "_md_create","_md_destroy","_md_frame_ptr","_md_frame_bytes","_md_process_rgba","_md_set_threshold","_md_score","_md_level","_md_changed_pixels"

.PHONY: all clean test example wasm docker-wasm

all: csa_example

csa_example: example.o csa_block.o
	$(CC) $(CFLAGS) -o $@ example.o csa_block.o $(LDFLAGS)

csa_tests: test_csa_block.o csa_block.o
	$(CC) $(CFLAGS) -o $@ test_csa_block.o csa_block.o $(LDFLAGS)

example.o: example.c csa_block.h
	$(CC) $(CFLAGS) -c example.c

test_csa_block.o: test_csa_block.c csa_block.h
	$(CC) $(CFLAGS) -c test_csa_block.c

csa_block.o: csa_block.c csa_block.h
	$(CC) $(CFLAGS) -c csa_block.c

web/motion_wasm.js: csa_block.c csa_block.h wasm_motion.c wasm_motion.h
	$(EMCC) csa_block.c wasm_motion.c -O3 \
		-s WASM=1 \
		-s MODULARIZE=1 \
		-s EXPORT_ES6=1 \
		-s EXPORT_NAME=createMotionModule \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s EXPORTED_FUNCTIONS='[$(WASM_EXPORTS)]' \
		-s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
		-o web/motion_wasm.js

example: csa_example
	./csa_example

test: csa_tests
	./csa_tests

wasm: web/motion_wasm.js

docker-wasm:
	docker run --rm -u "$$(id -u):$$(id -g)" -v "$$(pwd):/src" -w /src emscripten/emsdk:3.1.74 make wasm

clean:
	rm -f csa_example csa_tests example.o test_csa_block.o csa_block.o web/motion_wasm.js web/motion_wasm.wasm
