// UE Common.ush passes sampled resources by value through helper functions.
float ResourceHelper(Texture2D Tex) {
    return 7.25;
}
Texture2D Source : register(t0);
RWStructuredBuffer<float> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Output[0] = ResourceHelper(Source);
}
