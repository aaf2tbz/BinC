# BinC bootstrap compiler (in C). Self-hosting target.
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := src/main.c src/lexer.c src/parser.c src/codegen.c
DEV     ?= /Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer

binc: $(SRC) src/binc.h
	$(CC) $(CFLAGS) -o binc $(SRC)

# GPU dispatch/verify harness (Objective-C + Metal)
harness: harness.m
	DEVELOPER_DIR=$(DEV) xcrun clang -fobjc-arc -framework Metal -framework Foundation harness.m -o harness

# Compile every example to a .metallib in build/
EXAMPLES := blend step control
examples: binc
	@mkdir -p build
	@for ex in $(EXAMPLES); do ./binc ../examples/$$ex.binc -o build/$$ex.metallib; done

# Compile + run each on the GPU, verifying correctness
verify: binc harness examples
	./harness build/blend.metallib blend
	./harness build/step.metallib step
	./harness build/control.metallib sc
	@echo "ALL EXAMPLES VERIFIED ON GPU"

clean:
	rm -f binc harness build/*.metallib

.PHONY: binc harness examples verify clean
