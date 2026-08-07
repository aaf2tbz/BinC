// Geometry shaders: parse + stage classification work, but Metal mesh-stage
// lowering is not yet wired — the frontend must fail with a located, honest
// diagnostic instead of crashing or emitting a bogus metallib.
// (GS is D3D10-era gs_4_0; Phase 7 covers D3D9 sm3 .fx — see plan doc.)
struct VSOut
{
    float4 pos : POSITION;
};

VSOut vs_main( float4 vPos : POSITION )
{
    VSOut o;
    o.pos = vPos;
    return o;
}

[maxvertexcount(3)]
void gs_unsupported( triangle VSOut input[3], inout TriangleStream<VSOut> stream )
{
    stream.Append( input[0] );
    stream.RestartStrip();
}

float4 ps_main( float4 pos : POSITION ) : COLOR0
{
    return pos;
}

technique T
{
    pass P0
    {
        VertexShader = compile vs_3_0 vs_main();
        GeometryShader = compile gs_3_0 gs_unsupported();
        PixelShader = compile ps_3_0 ps_main();
    }
}
