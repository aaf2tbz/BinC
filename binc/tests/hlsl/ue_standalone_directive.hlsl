// UE standalone compile marker is intentionally left undefined in isolated audits.
COMPILER_ALLOW_CS_DERIVATIVES

RWStructuredBuffer<float> out_buf : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    out_buf[tid.x] = 1.0;
}
