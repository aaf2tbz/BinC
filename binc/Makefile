# BinC bootstrap compiler (in C). Self-hosting target.
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := src/main.c src/lexer.c src/parser.c src/codegen.c src/interp.c
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
	@set -e; for ex in $(EXAMPLES); do ./binc ../examples/$$ex.binc -o build/$$ex.metallib; done

# Build the playable BinC Pong host after the compiler has generated its binding header.
pong: runtime examples
	DEVELOPER_DIR=$(DEV) xcrun clang -fobjc-arc -framework Metal -framework Foundation -framework Cocoa -framework QuartzCore -framework AVFoundation ../examples/pong_host.m binc_runtime.o -I. -o pong_host

# Compile + run each on the GPU, verifying against its .spec
verify: binc harness examples
	@for ex in $(EXAMPLES); do ./harness build/$$ex.metallib ../examples/$$ex.spec || exit 1; done
	@echo "ALL EXAMPLES VERIFIED ON GPU"

# Compile-error suite: every tests/negative/*.binc must fail with expected messages
test-negative: binc
	chmod +x check_negative.sh
	./check_negative.sh

# Mutation fuzz smoke: the compiler must never crash or hang on malformed input
test-fuzz: binc
	python3 fuzz.py

# CPU reference interpreter: run each tests/interp/*.binc and compare buffer 0
test-interp: binc
	@fail=0; for t in ../tests/interp/*.binc; do \
		out="$$(./binc -i "$$t")"; want="$$(cat "$${t%.binc}.expected")"; \
		if [ "$$out" = "$$want" ]; then echo "PASS: $$t"; else echo "FAIL: $$t"; echo "  got: $$out"; echo "  want: $$want"; fail=1; fi; done; \
	if [ $$fail -eq 0 ]; then echo "ALL INTERP TESTS PASS"; else exit 1; fi

# tree-sitter grammar: every example + the prelude must parse with no ERROR nodes
test-grammar:
	@bash ../tools/tree-sitter-binc/test/parse_all.sh

# GPU-free CI pipeline: build, negative tests, fuzz, interpreter, compile all examples
ci: binc test-negative test-fuzz test-interp examples
	@echo "CI OK"

clean:
	rm -f binc harness pong_host binc_runtime.o build/*.metallib build/*.h *.ll *.air

.PHONY: binc harness runtime examples pong verify test-negative test-fuzz ci clean
