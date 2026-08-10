# BinC bootstrap compiler (in C). Self-hosting target.
CC      ?= clang
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
SRC     := src/main.c src/lexer.c src/parser.c src/codegen.c src/interp.c src/hlsl_parser.c src/hlsl_lower.c
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

# Codegen recovery regression: an aborted stage function must report its
# diagnostic without crashing during the later AIR metadata pass.
test-ue-recovery: binc
	@set +e; ./binc --stage-all -E MainPS -T ps_5_0 tests/hlsl/stage_recovery_regress.hlsl -o /tmp/stage-recovery.metallib >/tmp/stage-recovery.log 2>&1; rc=$$?; set -e; \
	if [ $$rc -eq 0 ] || [ $$rc -eq 134 ] || [ $$rc -eq 139 ] || grep -Eqi 'segmentation fault|bus error|abort trap' /tmp/stage-recovery.log; then \
		cat /tmp/stage-recovery.log; exit 1; \
	fi; grep -q 'internal: struct return UnknownStageOutput not found' /tmp/stage-recovery.log; \
	echo "UE CODEGEN RECOVERY PASS"

# UE-frontend regression: the preprocessor constructs that fixed real bugs
# must keep compiling to a metallib (tab directives, #ifndef, #define/#undef
# scoping, function-like macro chains, precise, typedef, _Static_assert, ::)
test-ue: binc test-ue-recovery harness
	@set -e; \
	for t in tests/hlsl/pp_regress.hlsl tests/hlsl/ue_standalone_directive.hlsl tests/hlsl/vector_logical_ops.hlsl tests/hlsl/texture_shape_samples.hlsl tests/hlsl/texture_gather_methods.hlsl tests/hlsl/non_square_matrix.hlsl tests/hlsl/resource_field_sample.hlsl tests/hlsl/implicit_uniform_overload.hlsl tests/hlsl/reversebits_regress.hlsl tests/hlsl/texture_load_atomic_regress.hlsl tests/hlsl/static_struct_method.hlsl tests/hlsl/nested_enum_regress.hlsl tests/hlsl/enum_regress.hlsl tests/hlsl/inout_resource_regress.hlsl tests/hlsl/norm_path_regress.hlsl tests/hlsl/macro_token_paste.hlsl tests/hlsl/texture_byvalue_helper.hlsl tests/hlsl/const_vector_field.hlsl ../third_party/UnrealEngine/Engine/Shaders/Private/NullPixelShader.usf ../third_party/UnrealEngine/Engine/Shaders/Private/LateZDummyPS.usf; do \
		case "$$t" in \
		*.hlsl) entry=main; prof=cs_5_0; defs="-D SM6_PROFILE";; \
		*NullPixelShader.usf) entry=Main; prof=ps_5_0; defs="-D COMPILER_DXC -D PLATFORM_WINDOWS -D SM6_PROFILE -D COMPILER_SUPPORTS_ATTRIBUTES -D A8_SAMPLE_MASK=.r";; \
		*LateZDummyPS.usf) entry=MainPS; prof=ps_5_0; defs="-D COMPILER_DXC -D PLATFORM_WINDOWS -D SM6_PROFILE -D COMPILER_SUPPORTS_ATTRIBUTES -D A8_SAMPLE_MASK=.r";; \
		esac; \
		echo "test-ue: $$t"; \
		./binc -E $$entry -T $$prof -I ../third_party/UnrealEngine $$defs "$$t" -o build/ue-test.metallib; \
	done; \
	./binc -E main -T cs_5_0 tests/hlsl/const_vector_field.hlsl -o /tmp/const-vector-field.metallib; \
	./harness /tmp/const-vector-field.metallib tests/hlsl/const_vector_field.spec; \
	./binc -I ../tools/hlsl-diff/ue-stubs -I ../third_party/UnrealEngine -E main -T cs_5_0 tests/hlsl/view_state_preexposure.hlsl -o /tmp/view-state-preexposure.metallib; \
	./harness /tmp/view-state-preexposure.metallib tests/hlsl/view_state_preexposure.spec; \
	./binc -E main -T cs_5_0 tests/hlsl/logical_not_integer.hlsl -o /tmp/logical-not-integer.metallib; \
	./harness /tmp/logical-not-integer.metallib tests/hlsl/logical_not_integer.spec; \
	./binc -E main -T cs_5_0 tests/hlsl/global_array_index.hlsl -o /tmp/global-array-index.metallib; \
	./harness /tmp/global-array-index.metallib tests/hlsl/global_array_index.spec; \
	./binc -E main -T cs_5_0 tests/hlsl/vector_index_lvalue.hlsl -o /tmp/vector-index-lvalue.metallib; \
	./harness /tmp/vector-index-lvalue.metallib tests/hlsl/vector_index_lvalue.spec; \
	echo "UE FRONTEND REGRESSIONS PASS"

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

# ---- HLSL-to-Metal Phase 0 (vendored DirectX corpus; see .hermes/plans/) ----
# third_party/ is gitignored; targets skip when the corpus is absent.

DDS_BIN  := ../tools/dds/test_dds

$(DDS_BIN): ../tools/dds/test_dds.c ../tools/dds/dds.h
	cc -std=c11 -Wall -Wextra -O2 ../tools/dds/test_dds.c -o $@

# every vendored .dds must parse (header + size); the degenerate allowlist
# (tools/vendor/degenerate_dds.txt) documents files with lying/mangled headers
test-dds: $(DDS_BIN)
	@if [ ! -d ../third_party ]; then echo "SKIP: third_party corpus not present"; exit 0; fi; \
	find ../third_party -name "*.dds" | sort > /tmp/dds_list.txt; \
	: > /tmp/dds_sweep.txt; while read f; do $(DDS_BIN) "$$f" >> /tmp/dds_sweep.txt 2>&1 || true; done < /tmp/dds_list.txt; \
	grep -q ": parse error" /tmp/dds_sweep.txt && rc=1 || rc=0; \
	if [ $$rc -ne 0 ]; then \
	  grep -v "^#\|  #" ../tools/vendor/degenerate_dds.txt | grep -v "^$$" > /tmp/dds_degen.txt; \
	  ok=1; for f in $$(grep ": parse error" /tmp/dds_sweep.txt | sed 's|: parse error.*||; s|^\.\./third_party/||'); do \
	    if ! grep -qxF "$$f" /tmp/dds_degen.txt; then echo "UNEXPECTED FAIL: $$f"; ok=0; fi; done; \
	  if [ $$ok -eq 1 ]; then echo "DDS sweep OK ($$(grep -c ': parse error' /tmp/dds_sweep.txt) degenerate, allowlisted)"; \
	  else echo "DDS sweep: unexpected failures"; exit 1; fi; \
	else echo "DDS sweep OK"; fi

# regenerate tools/vendor/corpus.json (shader + texture index with profiles)
corpus-scan:
	@python3 ../tools/vendor/scan_corpus.py

# DXC acceptance over the manifest (reference toolchain sanity; hard BinC gate lands in Phase 1)
corpus-parse:
	@bash ../tools/hlsl-diff/corpus-parse.sh

# differential smoke: ours (BinC HLSL frontend) vs reference (DXC -> SPIRV-Cross)
diff-smoke:
	@bash ../tools/hlsl-diff/diff.sh -g 4 -w 4 $(abspath ../tools/hlsl-diff/tests/hello_hlsl/main.hlsl)

# Phase-1 conformance: how much of DXC's lit corpus the HLSL frontend parses
test-lit:
	@bash ../tools/hlsl-diff/lit-conformance.sh

# Phase-2+ differential suite: every kernel in tools/hlsl-diff/tests/compute/,
# ours vs reference (DXC -> SPIRV-Cross) on GPU
DIFF_TESTS := $(wildcard ../tools/hlsl-diff/tests/compute/*.hlsl)
diff-suite:
	@for t in $(DIFF_TESTS); do \
		case "$$t" in *mrt*) continue;; esac; \
		echo "== $$(basename $$t)"; \
		bash ../tools/hlsl-diff/diff.sh -g 16 -w 16 $(abspath $$t) || exit 1; \
	done
	@echo "diff-suite: compute kernels OK"

# Phase-3 render differential: corpus VS/PS pairs + the MRT test,
# ours vs reference (DXC -> SPIRV-Cross) rendered pixel-for-pixel
RENDER_PAIRS := \
	../third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12HelloWorld/src/HelloTriangle/shaders.hlsl \
	../third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12HelloWorld/src/HelloFrameBuffering/shaders.hlsl \
	../third_party/DirectX-Graphics-Samples/Samples/Desktop/D3D12HelloWorld/src/HelloBundles/shaders.hlsl
# Phase-3/4 render differential: the full render-suite (3 corpus VS/PS pairs,
# cbuffer + tight-packing cbuffer, texture sample, MRT), ours vs the
# DXC -> SPIRV-Cross reference, rendered pixel-for-pixel
render-diff:
	@bash ../tools/hlsl-diff/render-suite.sh 2>&1 | tail -1

# Phase-8 differential sweep: deterministic suites (compute + render + nbody)
# plus the corpus compute probe -> coverage table; any crash/hang fails the gate
diff-sweep:
	@bash ../tools/hlsl-diff/diff-sweep.sh 2>&1 | tail -8

# Phase-5: the D3D12 nBodyGravity gravity CS (groupshared + barriers +
# four thread semantics + inout helper), ours vs DXC->SPIRV-Cross on GPU
diff-nbody:
	@bash ../tools/hlsl-diff/nbody-diff.sh 2>&1 | tail -1

# Phase-7 D3D9 goldens: .fx shaders rendered against pinned, hand-verified
# pixels (no clean differential path — fxc is Windows-only)
GOLDEN_TESTS := basichlsl normalmap motionblur
golden-d3d9:
	@for g in $(GOLDEN_TESTS); do \
		echo "== $$g"; \
		bash ../tools/hlsl-diff/golden-d3d9.sh $$g || exit 1; \
	done

# Phase-2 UE base-pass + compute goldens (reduced fixtures; the real
# BasePassPixelShader.usf needs the vendored UE tree + SCW-generated stubs)
golden-basepass:
	@echo "== basepass"
	bash ../tools/hlsl-diff/goldens/basepass/golden.sh || exit 1
	@echo "== uecompute"
	bash ../tools/hlsl-diff/goldens/uecompute/golden.sh || exit 1

# GPU-free CI pipeline: build, negative tests, fuzz, interpreter, compile all examples
ci: binc test-negative test-fuzz test-interp examples
	@echo "CI OK"

clean:
	rm -f binc harness pong_host binc_runtime.o build/*.metallib build/*.h *.ll *.air

.PHONY: binc harness runtime examples pong verify test-negative test-fuzz ci clean golden-d3d9 golden-basepass
