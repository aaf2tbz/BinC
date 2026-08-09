// C/HLSL permit whitespace between a function-like macro name and `(`.
#define SCALE2(value) ((value) * 2.0)
RWStructuredBuffer<float> out_buf : register(u0);

[numthreads(16, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    out_buf[tid.x] = SCALE2  (float(tid.x));
}
