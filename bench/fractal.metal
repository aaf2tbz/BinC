// bench/fractal.metal — hand-written MSL twin of fractal.binc.
// Same algorithm, same buffer layout: a fair AIR-emission sanity check.
#include <metal_stdlib>
using namespace metal;

kernel void fractal(device float* out [[buffer(0)]],
                    uint2 p [[thread_position_in_grid]]) {
    float2 c = (float2((float)p.x, (float)p.y) - float2(128.0, 128.0)) * 0.008;
    float2 z = float2(0.0, 0.0);
    int iter = 0;
    float m = 0.0;
    while (iter < 64) {
        z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c;
        m = dot(z, z);
        if (m > 4.0) break;
        iter++;
    }
    out[p.y * 256 + p.x] = (float)iter / 64.0;
}
