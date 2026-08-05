# ARCHITECTURE.md — how the BinC bootstrap compiler works

BinC: "works as C, acts as Metal." The bootstrap compiler is ~1050 lines of C11 in
`binc/src/` plus a generic Objective-C GPU harness. It reads `.binc` source, emits AIR
(LLVM IR text for `air64_v29-apple-macosx27.0.0`), and drives Apple's `metal`/`metallib`
tools to produce a `.metallib`. No external dependencies beyond the Xcode toolchain.

Everything below is grounded in the current code; anchors are `file:line` into the
current tree.

---

## 1. Pipeline

```
foo.binc
   │  lex()            binc/src/lexer.c     hand-written tokenizer → Token[]
   │  parse_program()  binc/src/parser.c    recursive descent → Program AST
   │  emit_air()       binc/src/codegen.c   type-directed LLVM IR text emission
   ▼
foo.ll          (AIR text: typed pointers, target triple air64_v29-apple-macosx27.0.0)
   │  metal foo.ll -c -o foo.air            (patched Clang front-end, from Xcode)
   ▼
foo.air         (LLVM bitcode, Apple-wrapped)
   │  metallib foo.air -o foo.metallib      (AIR-LLD link)
   ▼
foo.metallib    (Metal executable)
   │  binc/harness build via Makefile: harness foo.metallib foo.spec
   ▼
GPU dispatch + spec verification (actual hardware, per-example pass/fail)
```

`metal` and `metallib` are the only external tools, and they are mandatory: the `air64`
target exists only inside Apple's patched Clang shipped with the `metal` driver — stock
clang cannot emit it, and stock-clang bitcode is rejected by `metallib` ("Invalid
bitcode"). The old `metal-ir-converter` path is gone in this toolchain. See
`reference/VERIFIED_FACTS.md` items 1–7 for the measured evidence.

The driver (`binc/src/main.c`) shells out with two `system()` calls (`main.c:50-51`
and `main.c:55-56`), so a build is: `./binc foo.binc -o foo.metallib`.

---

## 2. File map

| Path | What it is |
|---|---|
| `binc/src/binc.h` | Shared definitions: type system, AST, token kinds, the 4 exported entry points. 85 lines. |
| `binc/src/lexer.c` | Tokenizer + `die()` (the one error API). 80 lines. |
| `binc/src/parser.c` | Recursive-descent parser → `Program` AST. 222 lines. |
| `binc/src/codegen.c` | AIR emission — the bulk of the compiler. 597 lines. |
| `binc/src/main.c` | Driver: read file → lex → parse → emit → `metal` → `metallib`. 62 lines. |
| `binc/harness.m` | Generic dispatch/verify harness: loads any `.metallib`, binds buffers per a `.spec`, runs one threadgroup on the GPU, compares results. 142 lines. |
| `binc/Makefile` | `binc` (compiler), `harness`, `examples` (compile all `../examples/*.binc`), `verify` (run all specs on GPU), `clean`. |
| `examples/` | One `.binc` + one `.spec` per test; 15 pairs currently. These are the test suite. |
| `reference/` | Hand-probed AIR oracle files (`air_*.ll` compiled from MSL by the real toolchain) + `VERIFIED_FACTS.md` — the ground-truth contract this compiler targets. |
| `proof/` | Early hand-authored proof IR (`blend_vision_proof.ll`) and captured binc outputs. |

---

## 3. Source inventory

### binc.h — the shared vocabulary

Types and enums:

- `AddrSpace` (`binc.h:9`) — `AS_THREAD=0, AS_DEVICE=1, AS_CONSTANT=2, AS_THREADGROUP=3`.
  Matches the AIR address-space map exactly (verified: `reference/VERIFIED_FACTS.md` item L).
- `Uniformity` (`binc.h:10`) — `UN_UNIFORM`/`UN_VARYING` param annotation; parsed and stored
  but currently only informational.
- `TypeKind` (`binc.h:12`) — `T_VOID, T_FLOAT, T_HALF, T_INT32, T_UINT32, T_BOOL, T_STRUCT`.
- `Type` (`binc.h:14-20`) — `{ kind, struct_name, is_ptr, as, vecn }`. `vecn` is 0/1 for
  scalars, 2/3/4 for vectors; a bare `T*` defaults to `AS_DEVICE` (set by the parser).
- `ValKind` (`binc.h:23`) — `VK_F32, VK_I32, VK_U32, VK_I1, VK_PTR`: the runtime value
  classification that drives type-directed codegen. Signedness is tracked here
  (`VK_U32` vs `VK_I32`), not in the LLVM type (both are `i32` — same bits).

AST:

- `ExprKind` (`binc.h:27-30`) — `E_FCONST, E_ICONST, E_BOOL, E_IDENT, E_DEREF, E_FIELD,
  E_INDEX, E_BIN, E_CMP, E_LOG, E_NOT, E_NEG, E_ASSIGN, E_CALL`.
- Operator enums (`binc.h:32-35`) — `BinOp`, `CmpOp`, `LogOp`, `AssignOp` (incl. compound
  assigns `+=` … `%=`).
- `struct Expr` (`binc.h:37-44`) — one node type for everything: literal values, `name`
  (ident/callee), `field` (E_FIELD), `operand` (unary/deref/field/index base),
  `lhs`/`rhs` (binary; `rhs` is also the E_INDEX subscript), `args`/`nargs` (E_CALL).
- `StmtKind`/`struct Stmt` (`binc.h:46-57`) — `S_EXPR, S_DECL, S_RETURN, S_IF, S_FOR,
  S_WHILE, S_BLOCK, S_BREAK, S_CONTINUE`. Bodies are `Block` (a `Stmt` array);
  `S_FOR` splits out `for_init/for_cond/for_incr`.
- `Param, Function, Field, StructDef, Program` (`binc.h:59-63`) — a `Function` is
  `{ name, params, body, is_kernel, ret }`; `Program` is parallel arrays of struct
  defs and functions. Everything is `malloc`/`realloc`, nothing is freed (short-lived process).

Lexer tables:

- `TokKind` (`binc.h:66-77`), `Token` (`binc.h:78`), `TokStream` (`binc.h:79`).
  Vector type keywords carry their width in `Token.ival` (see lexer below).

Exported entry points (`binc.h:81-84`): `lex`, `die`, `parse_program`, `emit_air`.

### lexer.c — tokenizer

- `die()` (`lexer.c:8-12`) — the compiler's only error path: print `binc: error (line N): …`,
  `exit(1)`. Every phase uses it; there is no error recovery.
- `lex()` (`lexer.c:17-80`) — single pass over the source; skips whitespace and `//`
  comments; one- or two-char punctuation tokens; numbers (`[0-9.]+` with optional `f`
  suffix — '.' or suffix ⇒ `TK_FCONST`, else `TK_ICONST` via `atol`); identifiers matched
  against the keyword tables.
- Keyword handling (`lexer.c:59-70`): two macros. `KW` emits a plain keyword token.
  `VW` handles the vector type names — `float2/3/4`, `int2/3/4`, `uint2/3/4` — by emitting
  the *base scalar* keyword (`TK_KW_FLOAT` etc.) with the width stashed in `Token.ival`
  (`lexer.c:60-63`). That payload is what `parse_type` later reads into `Type.vecn`.
  (`half2..4` deliberately do not exist.)

### parser.c — recursive descent

Token plumbing (`parser.c:6-9`): `peek/advance/accept/expect`.

Types:

- `parse_type` (`parser.c:11-23`) — optional address-space keyword, then base type.
  For scalar keywords it captures the vector width from `Token.ival` into `Type.vecn`
  (`parser.c:15-17`). An identifier becomes a struct type (`T_STRUCT` + tag). A trailing
  `*` sets `is_ptr` and defaults the address space to device.
- `starts_scalar_type` (`parser.c:24-25`) — declaration-statement lookahead.
- `stags`/`is_stag` (`parser.c:30-32`) — registry of known struct tags, filled by
  `parse_struct` (`parser.c:214`); used to disambiguate `Dog d;` (struct local) from an
  expression statement.

Expressions, lowest to highest precedence (`parser.c:34-122`):

- `parse_primary` — literals, `true/false`, identifiers, parenthesized exprs. Also the
  vector-constructor hook (`parser.c:40-44`): a vector type keyword in expression position
  becomes a synthetic `E_IDENT` named e.g. `"float4"`, which `parse_postfix` then turns
  into an `E_CALL` — constructors reuse the whole call machinery.
- `parse_postfix` (`parser.c:49-72`) — loops over four postfix forms:
  `f(args)` → `E_CALL` (callee must be a bare `E_IDENT`; no function pointers),
  `p[i]` → `E_INDEX`, `s.f` → `E_FIELD`, and `p->f` → desugared to
  `E_FIELD(E_DEREF(p), f)` so `->` keeps its "implicit index" meaning (`p->f == p[id].f`).
- `parse_unary` (`- ! *`), then `parse_mul` → `parse_add` → `parse_rel` → `parse_eq` →
  `parse_and` → `parse_or` → `parse_assign` (right-associative, `= += -= *= /= %=`).
  C operator precedence throughout.

Statements (`parser.c:124-181`):

- `return [expr];` (`parser.c:136-138`) — expression optional.
- `break;` / `continue;` (`parser.c:139-140`).
- `if/else`, `while`, `for` (`parser.c:141-167`) — bodies are braced blocks or single
  statements; the for-init may be a scalar declaration or an expression.
- Struct local (`parser.c:169-174`): `Dog d;` — identifier that is a known struct tag
  followed by another identifier.
- Scalar/vector local (`parser.c:175-179`), expression statement (fallthrough).

Top level:

- `parse_function` (`parser.c:183-201`) — optional `kernel` keyword, return type
  (non-void kernel ⇒ die; struct return ⇒ die), params with optional `uniform`/`varying`,
  struct-by-value param ⇒ die. Kernels and plain functions share the grammar; the
  distinction is semantic (see codegen).
- `parse_struct` (`parser.c:202-215`) — `struct Tag { ty f1, f2; … };`, fields may be any
  scalar/vector type, no pointer fields.
- `parse_program` (`parser.c:216-222`) — loop over structs and functions to EOF.

### codegen.c — AIR emission

Utilities:

- `SB` string builder (`codegen.c:10-15`) — each function body is built in memory and
  dumped at the end.
- `find_struct` (`codegen.c:17-18`).
- Type mapping helpers (`codegen.c:19-29`): `scalar_ll` (TypeKind → LLVM type text),
  `scalar_vk` (→ expression-level ValKind; note `T_HALF → VK_F32`: half always promotes
  to float in expressions), `size_of`/`align_of` (half=2, bool=1, else 4), `type_name`
  (MSL spellings for metadata).
- Vector/layout helpers (`codegen.c:30-43`): `type_size`/`type_align` implement the MSL
  vector layout rules (vec2 = 2×elem, vec3/vec4 = 4×elem for both size and alignment);
  `ll_of` renders `"<4 x float>"`-style type text; `tn_of` renders `"float4"`-style MSL
  names; `struct_layout` computes real struct size (aligned field offsets + tail padding)
  and alignment — used for `air.arg_type_size` metadata.
- `Loc` (`codegen.c:45`) — a local variable: `{ name, slot, kind, sname, vecn }`.
- `CG` (`codegen.c:46-55`) — per-function codegen state: output buffers (`pre` for
  allocas, `body` for instructions), the owning `Program`/`Function`, temp/label counters,
  `%_id`-as-i64 string `idx`, per-param `read`/`written` flags and `scalar_load` cache,
  the local table, `term` (current block terminated), the `brk_l`/`cont_l` loop-label
  stack, `uses_sync` (barrier present → needs convergent attributes), and `rvw` (vector
  width of the most recent `gen_rval` result — see below).
- `coerce` (`codegen.c:61-72`) — scalar implicit conversions: int↔uint is a no-op
  (same `i32` bits; signedness is a BinC-level fact), `sitofp/uitofp/fptosi/fptoui`,
  `zext` from `i1`.
- Name resolution (`codegen.c:74-84`): `resolve` → `R_LOCAL | R_PTR | R_SCALAR | R_NONE`;
  `root_param` walks `E_DEREF/E_FIELD/E_INDEX` chains to the underlying buffer param
  (used for the divergence heuristic and read/write tracking).

Lvalues — the core of the implicit-grid model:

- `LInfo` (`codegen.c:87`) — describes the *pointee* of an lvalue: `{ tk, sname, as, pi,
  is_local, vecn }`. Every load/store is driven by this, so element types are never
  assumed (the Phase-0 hardcoded-float bug is gone).
- `pty_str` (`codegen.c:88-91`) — renders a typed pointer string, with or without
  `addrspace(N)` depending on whether the pointee is a local alloca or a buffer.
- `element_ptr_idx` (`codegen.c:94-101`) — the `p[ix]` GEP for buffer param `pi`:
  `getelementptr inbounds <elt>, <elt> addrspace(N)* %_p, i64 <ix>`.
- `eval_ptr` (`codegen.c:107-119`) — a pointer *expression* → (param, i64 index value).
  Bare `p` ⇒ the implicit thread index `c->idx` (kernels only — dies in plain functions);
  `p ± int` ⇒ `sext` the offset and `add/sub i64` on the index (recursively, so
  `*(in-1)`, `*(out+1)`, `(p+1)[i]` all work). This is pointer arithmetic.
- `fill_param_li` (`codegen.c:120-122`).
- `gen_lval` (`codegen.c:371-413`) — expression → (address, LInfo):
  - `E_IDENT` → local alloca slot (params are not lvalues).
  - `E_DEREF` → `eval_ptr` + `element_ptr_idx`.
  - `E_INDEX` → explicit subscript: `sext` the index to i64 (added onto the base index
    when the base is itself pointer arithmetic), then `element_ptr_idx`.
  - `E_FIELD` on a struct → field index lookup in the `StructDef` (unknown field dies),
    GEP `i64 0, i32 <field>`; the resulting LInfo takes the field's declared type.
  - `E_FIELD` on a vector → component access (`codegen.c:400-409`): `bitcast` the vector
    address to an element pointer and GEP the component — this makes `.x/.y/.z/.w`
    (and `.r/.g/.b/.a`) work as both lvalues and rvalues, in locals and in buffers.
- `emit_load_t` (`codegen.c:125-133`) — typed load through an LInfo (whole struct dies);
  half loads immediately `fpext` to float; sets `CG.rvw` for vector loads.
- `store_val` (`codegen.c:135-139`) — coerce to the storage type; `fptrunc` on half stores.
- `splat`/`vconv` (`codegen.c:141-153`) — broadcast a scalar into `<n x elt>` via an
  `insertelement` chain; element-wise vector int↔float conversion.
- `to_storage` (`codegen.c:155-163`) — one funnel for "rvalue → storage type": splat or
  convert vectors, reject vector-into-scalar, else `store_val`. Used by assignments,
  declarations, returns, and call arguments.
- `vec_name`/`comp_idx` (`codegen.c:165-177`) — constructor-name recognition
  (`float2..uint4`) and component-name → index (`xyzw`/`rgba` aliases).

Builtins:

- `builtins[]` (`codegen.c:183-198`) — name → AIR intrinsic spelling, arity, return and
  argument types. Float math maps to `air.fast_*` (`sqrt fabs floor ceil sin cos exp log
  fmin fmax pow`), int min/max to `air.min.s.i32`/`air.max.s.i32` (BinC names `imin`/`imax`),
  and `sync()` to the threadgroup barrier `air.wg.barrier`. Spellings were probed from the
  metal front-end (`metal -emit-llvm -S` on an MSL file); the module enables fast math
  (`air.compile.fast_math_enable`), hence the `fast_` variants.
- `builtin_used[]` (`codegen.c:199`) — bitmap so `declare` lines are emitted only for
  builtins actually called.
- `cmp_name`/`as_op` (`codegen.c:201-214`) — predicate/op spelling tables, three regimes:
  float (`oeq/olt/…`, `fadd/…`), signed int (`slt/sdiv/srem`), unsigned int
  (`ult/ule/ugt/uge`, `udiv/urem`).

Expressions — `gen_rval` (`codegen.c:216-370`):

- Returns the LLVM SSA name (or literal) of the value, sets `*k` to its `ValKind`, and
  maintains `CG.rvw` — the vector width of the result (0 = scalar), reset on entry
  (`codegen.c:217`) and read by callers immediately after each recursive call.
- `E_IDENT` (`codegen.c:222-237`): local → typed load; kernel scalar param → one cached
  load from its `addrspace(2)` constant buffer; non-kernel scalar param → the plain SSA
  argument `%_name` (by-value). Unknown names die.
- `E_DEREF/E_FIELD/E_INDEX` (`codegen.c:238-242`): `gen_lval` + `emit_load_t`; marks the
  root buffer param read.
- `E_NEG/E_NOT`, `E_BIN` (`codegen.c:243-268`): mixed-kind arithmetic — float dominates;
  unsigned if either side is `VK_U32`. If either operand is a vector (`rvw > 0`), scalars
  are splatted and mismatched element kinds converted, then a single `<n x ty>` op.
- `E_CMP` (`codegen.c:269-276`) — vector comparison dies; else float/signed/unsigned
  predicate from `cmp_name`.
- `E_LOG` — `i1` and/or; vector operands die.
- `E_ASSIGN` (`codegen.c:280-308`) — evaluate rhs, `gen_lval` the target, compound ops
  reload through the LInfo and use `as_op` (vector-aware), then store via `to_storage`/
  `store_val` with the pointee's real type. Whole-struct assignment dies.
- `E_CALL` (`codegen.c:309-367`), in order:
  1. Vector constructor (`vec_name` matched): 1 arg = splat, N args = per-element
     `insertelement` chain (`codegen.c:312-322`).
  2. User function (resolved over the whole `Program`, so definition order is free):
     kernels are not callable, direct recursion dies, arg count checked; scalar/vector
     args coerced via `to_storage`; pointer args must be one of the caller's own buffer
     params with matching type/address-space, passed straight through; result kind/width
     from the callee's `ret` (half returns `fpext`ed) (`codegen.c:324-352`).
  3. Builtin from the table; marks `builtin_used`; `sync()` emits
     `call void @air.wg.barrier(i32 2, i32 5, i32 1)` and sets `uses_sync`
     (`codegen.c:353-365`).
  4. Otherwise `undefined function` dies.

Statements — `gen_stmt`/`gen_block` (`codegen.c:430-498`):

- `S_DECL` (`codegen.c:434-450`): struct local → `%struct.T` alloca at the struct's real
  alignment; scalar/vector → typed alloca in `pre` (entry block), optional init store.
- `S_RETURN` (`codegen.c:451-459`): coerces through `to_storage`, emits `ret <ty> <v>`;
  value-less return in a non-void function (and vice versa) dies.
- `S_BREAK/S_CONTINUE` (`codegen.c:460-463`): branch to the top of the loop-label stack;
  outside a loop they die.
- `S_IF/S_WHILE/S_FOR` (`codegen.c:465-495`): fresh numeric labels (`bb0:`, `bb1:`, …),
  `term` tracks block termination so no dead branches are emitted; loops push
  {exit, increment/condition} onto the break/continue stack. Data-dependent conditions
  (see `is_varying` below) print a divergence note to stderr — a warning, not an error.
- `gen_cond` (`codegen.c:415-422`) — any scalar expression → `i1` (`fcmp one` / `icmp ne`
  against zero); vectors die.
- `is_varying` (`codegen.c:424-428`) — the divergence heuristic: an expression is varying
  if it reads through a buffer param (any `E_DEREF/E_FIELD/E_INDEX` with a param root).

Module emission — `emit_air` (`codegen.c:509-597`):

1. Header: fixed datalayout string + `air64_v29-apple-macosx27.0.0` triple
   (`codegen.c:512-513`) — byte-identical to what clang emits for this toolchain.
2. `%struct.T = type { … }` declarations with real field types (`codegen.c:514-518`).
3. Per function (`codegen.c:521-554`): build the signature into a buffer but **print it
   only after the body is generated** — because a body containing `sync()` must get the
   convergent attribute group `#1` instead of the `nosync` group `#0`
   (`codegen.c:549-550`). Kernels: `define void @name(..., i32 noundef %_id)
   local_unnamed_addr`, pointer params as typed `addrspace(N)*`, scalar params lowered to
   `T addrspace(2)* … dereferenceable(size)` constant buffers, and the prologue
   `%t0 = zext i32 %_id to i64`. Non-kernels: `define internal <ret> @name(...)` with
   by-value scalar params and no `%_id`. A fallthrough `ret` (zero value of the return
   type) is appended when the body doesn't terminate.
4. `declare` lines for used builtins only (`codegen.c:556-560`).
5. Attribute groups (`codegen.c:561-564`): `#0` default fn attrs (incl. `nosync`,
   `norecurse` — accurate because recursion is rejected), `#1` the convergent variant for
   barrier-using functions, `#2` readnone (math intrinsics), `#3` convergent (barrier).
6. Metadata (`codegen.c:565-596`): fully numbered by hand. `!0–!12` are the fixed module
   flags/compile options/version nodes; kernel nodes start at `!13`. **Only `is_kernel`
   functions appear in `!air.kernel`** and get argument metadata; plain functions are
   invisible to the Metal runtime. Per kernel: an empty node, an arg-list node, the
   `!{fn-ptr-type, !empty, !arglist}` kernel node, one `air.buffer` node per parameter
   (with `air.location_index`, access mode derived from the actual read/write analysis —
   `air.read`/`air.write`/`air.read_write`, `air.address_space`, real
   `air.arg_type_size`/`air.arg_type_align_size` from `type_size`/`struct_layout`, MSL
   `air.arg_type_name`), and finally the `air.thread_position_in_grid` node for the
   hidden `%_id`. `fn_ptr_str` (`codegen.c:500-507`) renders the kernel's function-pointer
   type for the kernel node.

### main.c — driver

`main` (`main.c:22-61`): parse args (`binc <file.binc> [-o out.metallib]`), read the file,
`lex` → `parse_program` → `emit_air` into `<base>.ll` in the cwd, then
`metal <base>.ll -c -o <base>.air` and `metallib <base>.air -o <lib>` via `system()`
(`main.c:49-58`), checking exit codes. The `.ll`/`.air` intermediates stay in `binc/` —
they are the primary debugging artifacts (`make clean` removes them).

### harness.m — dispatch & verify

Spec format (documented at `harness.m:5-14`):

```
kernel <name>                 entry point in the metallib
grid <N>                      dispatch N threads (single threadgroup)
buf <idx> <v0> <v1> ...       buffer at <idx>, initialized with 32-bit words
bufh <idx> <bb bb ...>        buffer at <idx>, initialized with raw hex bytes
out <idx> <nwords>            zero-initialized output buffer at <idx>
expect <idx> <v0> <v1> ...    compare words (int tokens exact, float tokens w/ tolerance)
expecth <idx> <bb bb ...>     byte-exact comparison
```

- `parse_word` (`harness.m:26-33`): a token containing `.`/`e`/`E` is parsed as float and
  stored as its 32-bit bits; anything else as int32 bits. (Large uint bit patterns ride
  through as int32 bits — `4000000000` becomes the correct `0xEE6B2800` word.)
- Directive parsing (`harness.m:53-70`), buffer creation (`harness.m:83-98`): `buf` fills
  words, `bufh` fills bytes (`strtoul(…,16)`), `out` zero-fills.
- Dispatch (`harness.m:100-106`): one threadgroup of `grid` threads,
  `waitUntilCompleted`.
- Comparison (`harness.m:109-140`): `expecth` compares bytes exactly; `expect` compares
  int tokens exactly and float tokens within `1e-4·(|expected|+1)`. Prints per-element
  OK/X lines and a per-kernel ✅/❌ verdict; exit status is the aggregate.

---

## 4. The BinC → AIR mapping (semantic contract)

Address spaces:

| BinC | AIR | Use |
|---|---|---|
| `device` (default for `T*`) | `addrspace(1)` | global buffers |
| `constant` | `addrspace(2)` | constant buffers; also where scalar params land |
| `threadgroup` | `addrspace(3)` | shared memory (parsed; codegen use comes later) |
| `thread` | `addrspace(0)` | locals/allocas (no qualifier in IR) |

Source constructs:

| BinC | Emitted AIR |
|---|---|
| `kernel void f(...)` | `define void @f(<params>, i32 noundef %_id) local_unnamed_addr #0` + `!air.kernel` + per-arg metadata + `air.thread_position_in_grid` node |
| plain `float g(...)` | `define internal float @g(<by-value params>) #0` — no `%_id`, no metadata |
| scalar kernel param `float dt` | `float addrspace(2)* nocapture noundef readonly align 4 dereferenceable(4) %_dt` (constant buffer; metadata `air.buffer`, addrspace 2, `air.buffer_size`) |
| scalar plain-fn param | bare by-value `float noundef %_x` |
| `%_id` prologue | `%t0 = zext i32 %_id to i64` |
| `*p` | `gep <elt>, <elt> addrspace(N)* %_p, i64 %t0` (implicit thread index) |
| `p[i]` | same GEP with `sext i32 %i to i64` as the index |
| `*(p + k)` / `*(p - k)` | index arithmetic: `add/sub i64 %t0, sext(k)`, then the GEP |
| `s.f` / `p[i].f` / `p->f` | second GEP `i64 0, i32 <field index>` into `%struct.T`; field type from the `StructDef` |
| `v.y = x` (vector component) | `bitcast <n x float>* → float*`, GEP by component index, scalar store |
| `float4(a,b,c,d)` | `insertelement <4 x float>` chain from `undef`; 1 arg = splat |
| `half` load/store | `fpext half → float` on load, `fptrunc` on store (all arithmetic in float) |
| `uint` ops | `udiv/urem`, `ult/ule/ugt/uge`, `uitofp/fptoui` (int keeps `sdiv/srem/slt/…`) |
| `vec * scalar` | splat scalar, then `fmul fast <n x float>` (etc.) |
| `break` / `continue` | branch to loop exit / increment (for) or condition (while) label |
| `sync()` | `call void @air.wg.barrier(i32 2, i32 5, i32 1)`; containing function gets attributes `#1` (convergent, no `nosync`) |
| `sqrt/fabs/floor/ceil/sin/cos/exp/log(x)` | `call float @air.fast_<op>.f32(float)` (declared on demand, attrs `#2`) |
| `fmin/fmax/pow(x,y)` | `air.fast_fmin.f32` / `air.fast_fmax.f32` / `air.fast_pow.f32` |
| `imin/imax(a,b)` | `air.min.s.i32` / `air.max.s.i32` |

Layout rules (probed against clang's own AIR output):

| Type | Size | Align |
|---|---|---|
| `bool` (`i1`) | 1 | 1 |
| `half` | 2 | 2 |
| `float`/`int`/`uint` | 4 | 4 |
| `float2`/`int2`/`uint2` | 8 | 8 |
| `float3`/`int3`/`uint3` | 16 | 16 |
| `float4`/`int4`/`uint4` | 16 | 16 |
| struct | aligned field offsets, tail-padded | max field align |

Attribute groups (`codegen.c:561-564`):

| Group | Meaning | On |
|---|---|---|
| `#0` | `argmemonly mustprogress nofree norecurse nosync nounwind willreturn` | ordinary functions/kernels |
| `#1` | `#0` minus `nosync`, plus `convergent` | functions whose body calls `sync()` |
| `#2` | `mustprogress nofree nosync nounwind readnone willreturn` | math intrinsic declares |
| `#3` | `convergent mustprogress nounwind willreturn` | `air.wg.barrier` declare |

---

## 5. How a kernel becomes a grid — `step.binc` end to end

Source (`examples/step.binc`):

```c
struct Particle { float x, y, z; float vx, vy, vz; };
kernel void step(device Particle* p, float dt) {
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    p->z += p->vz * dt;
}
```

What the compiler does with it:

1. **Parse** — `p->x` becomes `E_FIELD(E_DEREF(p), "x")`; `+=` becomes `E_ASSIGN(A_ADDEQ)`.
2. **Signature** — `step` is a kernel: `p` is a `%struct.Particle addrspace(1)*`, the
   scalar `dt` auto-lowers to a constant buffer `float addrspace(2)* … dereferenceable(4)`,
   and the hidden thread index is appended:
   ```llvm
   define void @step(%struct.Particle addrspace(1)* nocapture noundef %_p,
                     float addrspace(2)* nocapture noundef readonly align 4 dereferenceable(4) %_dt,
                     i32 noundef %_id) local_unnamed_addr #0 {
   ```
3. **Body** — `%_id` is widened once; `p->vx` lowers to the two-GEP chain
   (element by id, then field 3), `dt` is loaded once from addrspace(2) and cached:
   ```llvm
   %t0 = zext i32 %_id to i64
   %t1 = getelementptr inbounds %struct.Particle, %struct.Particle addrspace(1)* %_p, i64 %t0
   %t2 = getelementptr inbounds %struct.Particle, %struct.Particle addrspace(1)* %t1, i64 0, i32 3
   %t3 = load float, float addrspace(1)* %t2, align 4
   %t4 = load float, float addrspace(2)* %_dt, align 4
   %t5 = fmul fast float %t3, %t4
   …load field 0, fadd, store back…
   ```
   (The full file is `binc/step.ll` after any build.)
4. **Metadata** — the kernel is registered with its argument layout:
   ```llvm
   !air.kernel = !{!13}
   !13 = !{void (%struct.Particle addrspace(1)*, float addrspace(2)*, i32)* @step, !14, !15}
   !15 = !{!16, !17, !18}
   !16 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write",
           !"air.address_space", i32 1, !"air.arg_type_size", i32 24, …, !"air.arg_name", !"p"}
   !17 = !{… !"air.buffer_size", …, !"air.address_space", i32 2, …, !"air.arg_name", !"dt"}
   !18 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"id"}
   ```
   The `air.read_write` on `p` comes from the compiler's own read/write analysis of the body.
5. **Driver** — `metal step.ll -c -o step.air && metallib step.air -o step.metallib`.
6. **Dispatch** — the harness binds the buffers from `step.spec`, launches one
   threadgroup of 2 threads; thread `id` reads/writes `p[id]` — the C-looking per-element
   body *is* the grid.

## 6. Verification loop

`make verify` (`binc/Makefile:21-23`):

1. Builds the compiler (`binc`) and the harness (`harness`, compiled with the beta
   toolchain via `DEVELOPER_DIR=…/Xcode-beta.app`).
2. `examples`: compiles every `../examples/*.binc` to `build/<name>.metallib`
   (intermediates `<name>.ll`/`<name>.air` land in `binc/`).
3. Runs `./harness build/<name>.metallib ../examples/<name>.spec` for each; any mismatch
  fails the build.
4. Prints `ALL EXAMPLES VERIFIED ON GPU`.

Current suite (15): `blend` (basic element-wise), `step`/`dogpark` (struct buffers,
`->`, compound assign), `control` (locals, for, if/else, scalar params), `stencil`
(pointer arithmetic), `brk` (break/continue), `calls` (user calls, forward refs, pointer
pass-through), `fieldchain` (`p[i].f` chains, early return, non-kernel emission), `math`
(builtins), `sync` (barrier), `uints` (unsigned ops over >2³¹ values), `halfs` (real f16
buffers + half scalar param, packed 2-per-word), `mixed` (mixed-type struct verified
byte-exact via `bufh`/`expecth`), `slocal` (struct locals), `vecs` (float4 buffers,
constructors, components, int2/float3 locals).

### Known quirks (documented, not bugs that affect the suite)

- `air.struct_type_info` metadata (present in clang's output for struct buffers) is not
  emitted; the runtime doesn't require it.
- Mutual recursion is not detected (only direct self-calls); a `void` call used as a
  value yields a dummy `0`; half vectors and multi-component swizzles don't exist.
