[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int digits[] = { 480599, 139810, 464711 };
    int digit = int(id.x) % 3;
    int value = digits[digit];
}
