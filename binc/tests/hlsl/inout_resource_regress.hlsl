// Regression for inout lowering moving the shared statement array before
// resource-reference scanning walks the helper body.
RWStructuredBuffer<float> Output : register(u0);

void Adjust(inout float x) {
    x = x + 1.0;
}

[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float x = 2.0;
    Adjust(x);
    Output[0] = x;
}
