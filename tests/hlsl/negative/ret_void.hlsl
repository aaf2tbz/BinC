RWStructuredBuffer<float> b : register(u0);
[numthreads(1,1,1)]
float main(uint3 t : SV_DispatchThreadID){ return 1.0; }
