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

runtime: binc_runtime.m binc_runtime.h
	DEVELOPER_DIR=$(DEV) xcrun clang -fobjc-arc -c binc_runtime.m -o binc_runtime.o

# Compile every example to a .metallib in build/
EXAMPLES := $(basename $(notdir $(wildcard ../examples/*.binc)))
examples: binc
	@mkdir -p build
	@for ex in $(EXAMPLES); do ./binc ../examples/$$ex.binc -o build/$$ex.metallib; done

# Compile + run each on the GPU, verifying against its .spec
verify: binc harness examples
	@for ex in $(EXAMPLES); do ./harness build/$$ex.metallib ../examples/$$ex.spec || exit 1; done
	@echo "ALL EXAMPLES VERIFIED ON GPU"

clean:
	rm -f binc harness binc_runtime.o build/*.metallib build/*.h *.ll *.air

.PHONY: binc harness examples verify clean
