module tau_editor_outline;
import tau_globals;

[ShaderConfig("Custom", "TransparentAlpha", false, false, false)]
struct EditorOutlineShader
{ uint unused; };

struct OutlineVertexOut
{ float4 position : SV_Position; };

[shader("vertex")]
OutlineVertexOut vertexMain(uint vertex_id: SV_VertexID)
{
    float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);

    OutlineVertexOut out;
    out.position = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return out;
}

[RenderTargetsConfig]
struct FragmentOut
{
    [RenderTarget("Swapchain")]
    float4 color : SV_Target0;
};

static const int RADIUS = 2;
static const float4 OUTLINE_COLOR = float4(1.0f, 0.55f, 0.1f, 1.0f);

[shader("fragment")]
FragmentOut fragmentMain(OutlineVertexOut input)
{
    Texture2D mask = getTexture(pc.texture_id);
    uint width, height;
    mask.GetDimensions(width, height);
    int2 pixel = int2(input.position.xy);

    // outside the selection, within RADIUS of it
    bool inside = mask.Load(int3(pixel, 0)).r > 0.5f;
    bool near = false;
    for (int y = -RADIUS; y <= RADIUS && !inside && !near; y++)
    {
        for (int x = -RADIUS; x <= RADIUS; x++)
        {
            int2 p = clamp(pixel + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
            if (mask.Load(int3(p, 0)).r > 0.5f)
            {
                near = true;
                break;
            }
        }
    }

    if (inside || !near) { discard; }

    FragmentOut out;
    out.color = OUTLINE_COLOR;
    return out;
}
