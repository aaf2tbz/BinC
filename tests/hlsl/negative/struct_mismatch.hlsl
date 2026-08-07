struct A { float x; }; struct B { float x; };
RWStructuredBuffer<float> b : register(u0);
[numthreads(1,1,1)]
void main(uint3 t : SV_DispatchThreadID){ A a; B bb; a = bb; b[0] = a.x; }
