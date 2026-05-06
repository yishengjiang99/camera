CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
LDFLAGS ?= -lm
EMCC ?= emcc
WASM_EXPORTS := "_md_create","_md_destroy","_md_frame_ptr","_md_frame_bytes","_md_process_rgba","_md_set_threshold","_md_score","_md_level","_md_changed_pixels","_md_motion_left","_md_motion_top","_md_motion_right","_md_motion_bottom"
BUILD_DIR := build
SRC_DIR := src
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
SITE_DIR := $(BUILD_DIR)/web
SITE_FILES := $(SITE_DIR)/index.html $(SITE_DIR)/app.js $(SITE_DIR)/style.css
WASM_JS := $(SITE_DIR)/motion_wasm.js

.PHONY: all clean test example wasm site docker-wasm docker-site

all: $(BIN_DIR)/csa_example

$(OBJ_DIR) $(BIN_DIR) $(SITE_DIR):
	mkdir -p $@

$(BIN_DIR)/csa_example: $(OBJ_DIR)/example.o $(OBJ_DIR)/csa_block.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/example.o $(OBJ_DIR)/csa_block.o $(LDFLAGS)

$(BIN_DIR)/csa_tests: $(OBJ_DIR)/test_csa_block.o $(OBJ_DIR)/csa_block.o | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(OBJ_DIR)/test_csa_block.o $(OBJ_DIR)/csa_block.o $(LDFLAGS)

$(OBJ_DIR)/example.o: $(SRC_DIR)/example.c $(SRC_DIR)/csa_block.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $(SRC_DIR)/example.c -o $@

$(OBJ_DIR)/test_csa_block.o: $(SRC_DIR)/test_csa_block.c $(SRC_DIR)/csa_block.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $(SRC_DIR)/test_csa_block.c -o $@

$(OBJ_DIR)/csa_block.o: $(SRC_DIR)/csa_block.c $(SRC_DIR)/csa_block.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $(SRC_DIR)/csa_block.c -o $@

$(SITE_DIR)/index.html: web/index.html | $(SITE_DIR)
	cp web/index.html $@

$(SITE_DIR)/app.js: web/app.js | $(SITE_DIR)
	cp web/app.js $@

$(SITE_DIR)/style.css: web/style.css | $(SITE_DIR)
	cp web/style.css $@

$(WASM_JS): $(SRC_DIR)/csa_block.c $(SRC_DIR)/csa_block.h $(SRC_DIR)/wasm_motion.c $(SRC_DIR)/wasm_motion.h | $(SITE_DIR)
	$(EMCC) $(SRC_DIR)/csa_block.c $(SRC_DIR)/wasm_motion.c -O3 \
		-s WASM=1 \
		-s MODULARIZE=1 \
		-s EXPORT_ES6=1 \
		-s EXPORT_NAME=createMotionModule \
		-s ALLOW_MEMORY_GROWTH=1 \
		-s EXPORTED_FUNCTIONS='[$(WASM_EXPORTS)]' \
		-s EXPORTED_RUNTIME_METHODS='["HEAPU8"]' \
		-o $(WASM_JS)

example: $(BIN_DIR)/csa_example
	./$(BIN_DIR)/csa_example

test: $(BIN_DIR)/csa_tests
	./$(BIN_DIR)/csa_tests

wasm: $(WASM_JS)

site: $(SITE_FILES) wasm

docker-wasm:
	docker run --rm -u "$$(id -u):$$(id -g)" -v "$$(pwd):/src" -w /src emscripten/emsdk:3.1.74 make wasm

docker-site:
	docker run --rm -u "$$(id -u):$$(id -g)" -v "$$(pwd):/src" -w /src emscripten/emsdk:3.1.74 make site

clean:
	rm -rf $(BUILD_DIR)
