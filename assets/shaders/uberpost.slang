import tau_globals;

[ShaderConfig("PostProcess", "Opaque", false, false, false)]
struct UberPostShader
{
    // bindless index, useless to display
    [Hidden]
    uint scene_color_tex;

    // the rest is written from the view's profile every frame
    [Hidden]
    float exposure;
    [Hidden]
    uint tonemap_op;
    [Hidden]
    float vignette_strength;
    [Hidden]
    float vignette_radius;
    [Hidden]
    float grain_strength;
    [Hidden]
    float saturation;
    [Hidden]
    float contrast;
};

[RenderTargetsConfig]
struct UberPostOutput
{
    [RenderTarget("Swapchain")]
    float4 color : SV_Target0;
}

struct UberPostVertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float3 tonemapAces(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0f, 1.0f);
}

float3 tonemapReinhard(float3 color) { return color / (1.0f + color); }

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

[shader("vertex")]
UberPostVertexOut vertexMain(uint vertex_id: SV_VertexID)
{
    UberPostVertexOut out;

    out.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    out.position = float4(out.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return out;
}

[shader("fragment")]
UberPostOutput fragmentMain(UberPostVertexOut input)
{
    UberPostShader mat = getMaterial<UberPostShader>(pc.custom_data);

    float3 color = getTexture(mat.scene_color_tex).Sample(getSampler(SAMPLER_LINEAR_CLAMP), input.uv).rgb;

    color *= mat.exposure;

    if (mat.tonemap_op == 1) { color = tonemapAces(color); }
    else if (mat.tonemap_op == 2) { color = tonemapReinhard(color); }

    if (mat.saturation != 1.0f)
    {
        float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
        color = lerp(float3(luma), color, mat.saturation);
    }

    if (mat.contrast != 1.0f) { color = (color - 0.5f) * mat.contrast + 0.5f; }

    if (mat.vignette_strength > 0.0f)
    {
        float2 centered = input.uv - 0.5f;
        float dist = length(centered) / max(mat.vignette_radius, 0.0001f);
        color *= 1.0f - saturate(dist * dist) * mat.vignette_strength;
    }

    if (mat.grain_strength > 0.0f)
    {
        float noise = hash12(input.uv * 1024.0f + globals.time) - 0.5f;
        color += noise * mat.grain_strength;
    }

    UberPostOutput out;
    out.color = float4(saturate(color), 1.0f);
    return out;
}
