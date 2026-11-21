import tau_surface;

#include "tau/rendering/shader_shared.h"

[ShaderConfig("PostProcess", "Opaque", false, false, false)]
struct DeferredLightingShader
{
    [Hidden]
    uint albedo_tex;
    [Hidden]
    uint normal_tex;
    [Hidden]
    uint material_tex;
    [Hidden]
    uint depth_tex;
};

[RenderTargetsConfig]
struct DeferredLightingOutput
{
    [RenderTarget("RGBA16_FLOAT")]
    float4 color : SV_Target0;
}

struct FullscreenVertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

#define TAU_DEBUG_VIEW_CONST(name, value, label) static const uint DEBUG_##name = value;
TAU_DEBUG_VIEW_LIST(TAU_DEBUG_VIEW_CONST)
#undef TAU_DEBUG_VIEW_CONST

float3 worldFromDepth(float2 uv, float depth)
{
    float4 clip = float4(uv * 2.0f - 1.0f, depth, 1.0f);
    float4 world = mul(globals.inv_view_proj, clip);
    return world.xyz / world.w;
}

[shader("vertex")]
FullscreenVertexOut vertexMain(uint vertex_id: SV_VertexID)
{
    FullscreenVertexOut out;
    out.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    out.position = float4(out.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return out;
}

[shader("fragment")]
DeferredLightingOutput fragmentMain(FullscreenVertexOut input)
{
    DeferredLightingShader mat = getMaterial<DeferredLightingShader>(pc.custom_data);
    SamplerState samp = getSampler(SAMPLER_NEAREST_CLAMP);

    float4 albedo_sample = getTexture(mat.albedo_tex).Sample(samp, input.uv);
    float2 normal_sample = getTexture(mat.normal_tex).Sample(samp, input.uv).rg;
    float4 material_sample = getTexture(mat.material_tex).Sample(samp, input.uv);
    float depth = getTexture(mat.depth_tex).Sample(samp, input.uv).r;

    Surface surface;
    surface.albedo = albedo_sample.rgb;
    surface.normal = decodeNormalOctahedral(normal_sample);
    surface.roughness = material_sample.r;
    surface.metallic = material_sample.g;

    DeferredLightingOutput out;

    switch (globals.debug_view)
    {
    case DEBUG_ALBEDO: out.color = float4(surface.albedo, 1.0f); return out;
    case DEBUG_NORMAL: out.color = float4(surface.normal * 0.5f + 0.5f, 1.0f); return out;
    case DEBUG_ROUGHNESS: out.color = float4(surface.roughness.xxx, 1.0f); return out;
    case DEBUG_METALLIC: out.color = float4(surface.metallic.xxx, 1.0f); return out;
    case DEBUG_DEPTH: out.color = float4(pow(depth, 0.125f).xxx, 1.0f); return out; // reverse z
    default: break;
    }

    if (depth <= 0.0f) // reverse z
    {
        out.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return out;
    }

    float3 world_pos = worldFromDepth(input.uv, depth);
    if (globals.debug_view == DEBUG_CLUSTERS)
    {
        out.color = float4(clusterHeatmap(clusterLightCount(world_pos)), 1.0f);
        return out;
    }

    if (globals.debug_view == DEBUG_SHADOW_CASCADES)
    {
        out.color = float4(shadowCascadeTint(world_pos) * max(surface.albedo, 0.05f) * 4.0f, 1.0f);
        return out;
    }

    if (globals.debug_view == DEBUG_WORLD_POS)
    {
        out.color = float4(frac(world_pos), 1.0f);
        return out;
    }

    if (globals.debug_view == DEBUG_SHADOW_TERM)
    {
        out.color = float4(shadow(world_pos, surface.normal, 0).xxx, 1.0f);
        return out;
    }

    out.color = float4(applyLighting(surface, world_pos), 1.0f);
    return out;
}
