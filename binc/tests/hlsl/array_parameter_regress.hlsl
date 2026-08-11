struct ArrayValue
{
    float X;
    float Y;
};

float ReadArray(ArrayValue Values[2])
{
    return Values[0].X + Values[1].Y;
}

float ForwardArray(ArrayValue Values[2])
{
    return ReadArray(Values);
}

[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ArrayValue Values[2] = {
        (ArrayValue)0,
        (ArrayValue)0
    };
    float Result = ForwardArray(Values);
}
