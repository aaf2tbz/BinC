// C99 two-level token pasting: UE uses CONCAT(A, B) around macro-expanded suffixes.
#define CONCAT2(A, B) A##B
#define CONCAT(A, B) CONCAT2(A, B)
static const float CONCAT(Output,Value) = 7.25;
RWStructuredBuffer<float> Output : register(u0);
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    Output[0] = CONCAT(Output,Value);
}
