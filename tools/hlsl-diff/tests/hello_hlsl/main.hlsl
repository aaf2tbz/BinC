// tools/hlsl-diff/tests/hello_hlsl/main.hlsl — Phase 1's first failing test.
// A trivial compute kernel: out[tid] = tid * 2.0. When the HLSL frontend
// lands, `diff.sh -E main -T cs_6_0 -g 4 -w 4 tests/hlsl-diff/tests/hello_hlsl/main.hlsl`
// must report OK with the reference (DXC -> SPIRV-Cross) compilation.
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(4, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    out_buf[tid.x] = float(tid.x) * 2.0;
}
