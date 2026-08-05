# BinC Language Specification

Version: 0.1 · Status: living document — every rule below is enforced by the reference
implementation (`binc/`). Grammar rules are named after the parser functions that
implement them (`parse_*` in `src/parser.c`).

BinC is a C-like GPU language that compiles to Apple Metal AIR. **It works as C and
acts as Metal**: syntax and semantics follow C where the GPU permits, and follow
Metal Shading Language (MSL) where the hardware demands. There is no runtime, no
heap, no recursion, and no dynamic dispatch.

---

## 1. Lexical structure

- Source is UTF-8 text; whitespace (space, tab, newline) separates tokens.
- Comments: `//` to end of line, and `/* ... */` (non-nesting).
- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`.
- Integer literals: decimal (`123`), hexadecimal (`0x1F`, `0XFF`), with an optional
  `u`/`U` suffix (`0xFFu`). Values are 32-bit.
- Float literals: decimal with `.` and/or exponent, with an optional `f`/`F`
  suffix (`3.14`, `1e-3`, `2.5f`).
- Keywords:

```
bool        break       case        const       constant    continue
coord       default     device      do          else        false
float       for         fragment    grid_extent half        if
int         kernel      mat         return      sampler     struct
switch      template    texture2d   thread      threadgroup true
typedef     uint        uniform     varying     vertex      vertex_id
void        while       typename
```

- Punctuation: `( ) { } [ ] [[ ]] * , ; -> . + - / % ! = == != < <= > >=
  && || & | ^ ~ << >> ? : ++ -- += -= *= /= %= &= |= ^= <<= >>=`

### 1.1 Modules

- `include "path.binc";` splices another file at the driver level. Resolution
  order: relative to the including file, then `-I <dir>` search paths, then the
  current directory.
- `once;` marks a file as included at most once per compilation.
- Include cycles are compile errors; a missing file is a located error.
- `binc/prelude.binc` is auto-included before every compilation unless
  `-no-prelude` is given.

---

## 2. Types

### 2.1 Scalar types

| Type    | Meaning                          | AIR                          |
|---------|----------------------------------|------------------------------|
| `void`  | no value (returns / no-op calls) | `void`                       |
| `bool`  | 1-bit truth                      | `i1`                         |
| `int`   | 32-bit signed two's complement   | `i32`                        |
| `uint`  | 32-bit unsigned                  | `i32` (same bits)            |
| `float` | IEEE 754 single                  | `float`                      |
| `half`  | IEEE 754 binary16                | `half` (loaded as `float`)   |

Implicit conversions: `int` → `float`, `float` → `half` on storage, and the
reverse on load (`fpext`). Arithmetic on mixed operands promotes to `float`.
Implicit narrowing emits a warning; explicit casts `(type)expr` are required for
lossy conversions.

### 2.2 Vector types

`float2`–`float4`, `int2`–`int4`, `uint2`–`uint4` (spelled `floatN`, `intN`,
`uintN` in the type position). Component access by index (`v[0]`) or swizzle
(`v.xy`, `v.rgb`, `.xyzw`/`.rgba`; up to 4 components). Swizzles are lvalues
(`v.xy = ...`). Mixed-scalar/vector constructors are allowed
(`float3(v2, s)`, arity 1..N; single-argument form splats).

Vector arithmetic, comparison (yields a mask vector), `select(a, b, mask)`, and
the vector math library (`dot`, `cross`, `length`, `distance`, `normalize`,
`reflect`, `clamp`, `mix`, `step`, `smoothstep`, `fract`, `mod`, `rsqrt`,
`atan2`, `sign`, `radians`, `degrees`) follow MSL semantics.

### 2.3 Matrix types

`mat2`, `mat3`, `mat4`: column-major float matrices, MSL-compatible layout.
Constructors: 1 scalar (diagonal), N column vectors, or N×N scalars in
column-major order. Element access `m[col][row]`. Operations: `mat*vec`,
`vec*mat`, `mat*mat`, `mat±mat`, `mat*scalar`. Matrices are locals, struct
fields, and device/threadgroup pointers; matrix-by-value parameters and returns
are not supported.

### 2.4 Structs

```c
struct Pair { float a; float b; };   // C-like, MSL-compatible layout
```

Fields may be scalars, vectors, matrices, fixed-size arrays (`float v[4]`,
`float g[2][3]`), or other structs. Struct values support assignment,
initialization (`Pair p = other;`), by-value parameters and returns in plain
functions, and field access on struct-returning calls (`make().a`).

Generic structs: `template<typename T> struct Pair { T a; T b; };` — every use
`Pair<float>` / `Pair<int2>` instantiates a distinct layout (`Pair$f32`,
`Pair$v2f32`). Type-variable-nested instantiations and pointer-type arguments
are errors.

Render stage structs use `[[...]]` field attributes (see §6).

### 2.5 Address spaces and pointers

Pointers carry an address space:

- `device` — buffer memory (AIR address space 1). Default for `T*`.
- `constant` — immutable buffer (address space 2).
- `threadgroup` — shared memory within a workgroup (address space 3).
- `thread` — per-thread memory (register-backed; `vertex_id`).
- `device`/`constant` buffers are kernel parameters; pointers may be indexed
  (`buf[i]`), dereferenced (`*p`), and used for `->` field access. Pointer
  arithmetic is restricted to integer offsets from buffer parameters.

### 2.6 Special types

- `coord1D` / `coord2D` / `coord3D` — the launch domain coordinate; a kernel
  parameter that makes the function a kernel.
- `grid_extent` — the launch domain extent.
- `atomic<float|int|uint>` — global-memory atomic; method `acc->add(v)` lowers
  to `air.atomic.global.add`.
- `texture2d<float|half|int|uint>` — opaque texture handle with `.read(c)`,
  `.write(v, c)`, `.sample(sampler, uv)`; `sampler` — opaque sampler handle.

### 2.7 Generics

`template<typename T> T f(T x) { ... }` declares a monomorphized function.
Each call site with a concrete `T` instantiates a separate copy
(`f.f32`, `f.i32`, ...), cached per type; `T` may appear in locals and `(T)`
casts. Kernels cannot be templates; the binding type is inferred from the first
`T`-typed argument; unsupported types (structs, textures, pointers) are errors.

---

## 3. Program structure

```
program        := { struct_def | const_def | function_def }
struct_def     := [ template_head ] "struct" ident "{" { field } "}" ";"
template_head  := "template" "<" "typename" ident ">"
field          := type ident { "[" ic "]" } [ "[" "[" attr "]" "]" ] { "," ident { "[" ic "]" } } ";"
const_def      := "constant" scalar_type ident "=" literal ";"
function_def   := [ template_head ] [ "vertex" | "fragment" | "kernel" ] type ident "(" [ param { "," param } ] ")" block
param          := [ "uniform" | "varying" ] type ident { "[" ic "]" }
```

- A function with a `coordN` parameter is a kernel (the `kernel` keyword is
  optional). Render stages may not be kernels.
- Only scalar numeric constants may be module-level `constant` globals.
- Functions are visible across the whole module regardless of definition order;
  recursion is rejected.
- Struct tags must be declared before use.

---

## 4. Types grammar

```
type           := [ addrspace ] base_type { "*" }
addrspace      := "device" | "constant" | "threadgroup" | "thread"
base_type      := "void" | "bool" | "float" | "half" | "int" | "uint"
               | "float" d | "int" d | "uint" d        (* d in 2..4 *)
               | "mat" d                               (* d in 2..4 *)
               | "coord" d                             (* d in 1..3 *)
               | "grid_extent"
               | "atomic" "<" ("float"|"int"|"uint") ">"
               | "texture2d" "<" ("float"|"half"|"int"|"uint") ">"
               | "sampler"
               | ident [ "<" type ">" ]                (* struct / generic struct *)
               | type_parameter                        (* inside template bodies *)
```

---

## 5. Statements

```
statement      := "{" { statement } "}"
               | declaration
               | expression ";"
               | "return" [ expression ] ";"
               | "if" "(" expression ")" statement [ "else" statement ]
               | "while" "(" expression ")" statement
               | "do" statement "while" "(" expression ")" ";"
               | "for" "(" [ init ] ";" [ expression ] ";" [ expression ] ")" statement
               | "switch" "(" expression ")" "{" { switch_case } [ "default" ":" { statement } ] "}"
               | "break" ";"
               | "continue" ";"
declaration    := [ "const" ] type ident { "[" ic "]" } [ "=" expression ] ";"
init           := declaration | expression
```

- `switch` cases fall through; the condition must be an integer scalar.
- `const` locals are compile-time immutable (writes are errors).
- Local fixed-size arrays (`float local[8]`, `float g[4][4]`) are allocated per
  thread; array elements are not values (no whole-array assignment).
- `break`/`continue` must be inside a loop or `switch`.

---

## 6. Expressions

```
expression     := assignment
assignment     := unary ( "=" | "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ) assignment
               | ternary
ternary        := binary [ "?" expression ":" expression ]     (* lower precedence *)
binary         := unary { ( "+" | "-" | "*" | "/" | "%" | "&" | "|" | "^" | "<<" | ">>" ) unary }
               | unary { ( "<" | "<=" | ">" | ">=" | "==" | "!=" ) unary }
               | unary { ( "&&" | "||" ) unary }
unary          := "!" unary | "-" unary | "~" unary | "*" unary
               | "(" type ")" unary                            (* cast *)
               | postfix
postfix        := primary { "(" [ expression { "," expression } ] ")"
                           | "[" expression "]"
                           | "." ident
                           | "->" ident
                           | "++" | "--" }
primary        := int_literal | float_literal | "true" | "false" | ident
               | "(" expression ")"
```

- The ternary condition must be a scalar `bool`; the branches must agree in type
  (ints coerce to float).
- Comparison on scalars yields `bool`; on vectors yields a mask vector of
  `bool` lanes (usable with `select`).
- Logical `&&`/`||` are short-circuit, scalar `bool` only.
- `++`/`--` are postfix-only, on scalar numeric lvalues, yielding the old value.
- Assignments to `const` locals, arrays, or structs with compound operators are
  errors.
- Calls: user functions (no recursion), texture/atomic methods, vector/matrix
  constructors, and builtins (§7). Argument counts and types are checked; struct
  arguments must match the parameter's struct tag.

---

## 7. Built-ins

Scalar math (vector overloads where noted): `sqrt`, `rsqrt`, `sin`, `cos`,
`tan`, `asin`, `acos`, `atan`, `atan2`, `pow`, `exp`, `log`, `exp2`, `log2`,
`abs` (and `fabs`), `fmin`, `fmax`, `imin`, `imax`, `floor`, `ceil`, `fract`,
`mod`, `sign`, `radians`, `degrees`, `mix`, `clamp`, `step`, `smoothstep`,
`dot`, `cross`, `length`, `distance`, `normalize`, `reflect`, `select`,
`sync`.

The prelude (`prelude.binc`) additionally defines `PI`, `TAU`, `E`, `min`,
`max`, `lerp`, `saturate`, `pack_rgba`/`unpack_rgba` (0–255 uint packing), and
`hash12`.

---

## 8. Render stages

```c
struct VOut { float4 pos [[position]]; float3 uv [[user(locn0)]]; };
vertex VOut vs(device float4* verts, uint vid [[vertex_id]], coord1D i) { ... }
fragment FOut fs(VIn in) { ... }          // stage-in struct by value
```

- Stage-out structs return the position (`[[position]]`), interpolants
  (`[[user(locnN)]]`, `[[color(N)]]`), and `[[depth(any)]]`; stage-in structs
  receive interpolated values; `[[flat]]` disables perspective interpolation.
- Stage functions take buffer parameters plus `[[vertex_id]]` and `coordN`.
- Struct-by-value parameters are legal only in fragment functions (stage-in);
  struct returns only in vertex/fragment functions (stage-out).

---

## 9. Semantics

- **Numeric behavior**: `float` operations use `fast` floating-point flags
  (the MSL default); `int` arithmetic is wrapping two's complement; division by
  zero is not guarded. Integer shifts mask the count to 5 bits.
- **Uniformity**: a function parameter may be `uniform` (default, same for all
  threads) or `varying`. Built-in uniformity checks reject calling a
  barrier-requiring construct from data-dependent control flow, and
  `device`-buffer accesses from uniform (non-coordinate) code. `sync()` in
  divergent control flow is a compile error.
- **Memory model**: `device`/`constant`/`threadgroup` accesses are
  un-ordered; atomics provide ordering per object. A kernel containing a
  barrier is marked convergent (`#1`); others are `nosync`-eligible.
- **Termination**: kernels return `void`; a falling-off-the-end function
  returns zero/`undef` per its return type.

---

## 10. Code generation contract (AIR)

- Target triple and `!air.version` are derived from the installed SDK
  (`xcrun --show-sdk-version`): `air64_v<SDK+2>-apple-macosx<SDK>.0.0`,
  version `2.<SDK-18>`.
- Kernel scalar parameters become `addrspace(2)` dereferenceable constants;
  buffer parameters are `addrspace(1)` pointers; threadgroup arrays are module
  globals in `addrspace(3)`.
- Texture/sampler parameters are opaque struct pointers in address spaces
  1/2; methods lower to `air.read_texture_2d`, `air.write_texture_2d`,
  `air.sample_texture_2d`, `air.get_read_sampler`.
- Stage entry points carry `air.vertex`/`air.fragment` metadata with per-field
  `air.vertex_output`, `air.fragment_input`, `air.render_target`, `air.depth`
  nodes; stage-out structs lower to AIR literal types with a chained
  `insertvalue` sequence.
- All emitted AIR must survive `xcrun metal` + `metallib` on the installed
  toolchain (both local beta and CI stable Xcode — the SDK-derived contract
  is what makes this portable).
