module lighting;

import tau_globals;

public struct Surface
{
    public float3 albedo;
    public float3 normal;
    public float roughness;
    public float metallic;
};

static const float PI = 3.14159265359f;
static const float MIN_ROUGHNESS = 0.045f;
static const float AMBIENT = 0.015f;

float distributionGGX(float n_dot_h, float alpha)
{
    float a2 = alpha * alpha;
    float d = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * d * d, 1e-7f);
}

float visibilitySmith(float n_dot_v, float n_dot_l, float alpha)
{
    float a2 = alpha * alpha;
    float v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0f - a2) + a2);
    float l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0f - a2) + a2);
    return 0.5f / max(v + l, 1e-7f);
}

float3 fresnelSchlick(float3 f0, float v_dot_h) { return f0 + (1.0f - f0) * pow(saturate(1.0f - v_dot_h), 5.0f); }

public float3 evaluateBRDF(Surface surface, IncidentLight light, float3 view_dir)
{
    float n_dot_l = dot(surface.normal, light.direction);
    if (n_dot_l <= 0.0f) { return float3(0.0f); }

    float n_dot_v = saturate(dot(surface.normal, view_dir)) + 1e-5f;
    float3 half_vec = normalize(light.direction + view_dir);
    float n_dot_h = saturate(dot(surface.normal, half_vec));
    float v_dot_h = saturate(dot(view_dir, half_vec));

    float roughness = clamp(surface.roughness, MIN_ROUGHNESS, 1.0f);
    float alpha = roughness * roughness;

    float3 f0 = lerp(float3(0.04f), surface.albedo, surface.metallic);
    float3 diffuse_albedo = surface.albedo * (1.0f - surface.metallic);

    float3 fresnel = fresnelSchlick(f0, v_dot_h);
    float3 specular = fresnel * distributionGGX(n_dot_h, alpha) * visibilitySmith(n_dot_v, n_dot_l, alpha);
    float3 diffuse = (1.0f - fresnel) * diffuse_albedo / PI;

    return (diffuse + specular) * light.radiance * n_dot_l;
}

// fraction of a cascade's depth range spent blending into the next one
static const float CASCADE_BLEND_FRACTION = 0.12f;

float pcf3x3(Texture2D map, SamplerComparisonState cmp, float2 uv, float depth_ref, float2 map_size, float4 clamp_rect)
{
    float2 texel_size = 1.0f / map_size;

    float2 uv_texels = uv * map_size;
    float2 base = floor(uv_texels + 0.5f);
    float s = uv_texels.x + 0.5f - base.x;
    float t = uv_texels.y + 0.5f - base.y;
    base = (base - 0.5f) * texel_size;

    float uw0 = 3.0f - 2.0f * s;
    float uw1 = 1.0f + 2.0f * s;
    float u0 = (2.0f - s) / uw0 - 1.0f;
    float u1 = s / uw1 + 1.0f;

    float vw0 = 3.0f - 2.0f * t;
    float vw1 = 1.0f + 2.0f * t;
    float v0 = (2.0f - t) / vw0 - 1.0f;
    float v1 = t / vw1 + 1.0f;

    float sum = 0.0f;
    sum +=
        uw0 * vw0 *
        map.SampleCmpLevelZero(cmp, clamp(base + float2(u0, v0) * texel_size, clamp_rect.xy, clamp_rect.zw), depth_ref);
    sum +=
        uw1 * vw0 *
        map.SampleCmpLevelZero(cmp, clamp(base + float2(u1, v0) * texel_size, clamp_rect.xy, clamp_rect.zw), depth_ref);
    sum +=
        uw0 * vw1 *
        map.SampleCmpLevelZero(cmp, clamp(base + float2(u0, v1) * texel_size, clamp_rect.xy, clamp_rect.zw), depth_ref);
    sum +=
        uw1 * vw1 *
        map.SampleCmpLevelZero(cmp, clamp(base + float2(u1, v1) * texel_size, clamp_rect.xy, clamp_rect.zw), depth_ref);

    return sum * (1.0f / 16.0f);
}

float sampleShadowTile(Texture2D atlas, float4x4 tile_view_proj, float4 uv_offset_scale, float texel_world_size,
                       float tile_normal_bias, float tile_depth_bias, float3 world_pos, float3 normal, float n_dot_l,
                       float2 atlas_size)
{
    float sin_theta = sqrt(saturate(1.0f - n_dot_l * n_dot_l));
    float offset_scale = texel_world_size * tile_normal_bias;
    float3 biased_pos = world_pos + normal * (offset_scale * sin_theta);

    float4 lp = mul(tile_view_proj, float4(biased_pos, 1.0f));
    if (lp.w <= 0.0f) { return 1.0f; } // behind the light

    float3 ndc = lp.xyz / lp.w;
    if (ndc.z > 1.0f) { return 1.0f; } // past the light far plane

    // shadow map is stored upright
    float2 local_uv = ndc.xy * 0.5f + 0.5f;
    if (any(local_uv < 0.0f) || any(local_uv > 1.0f)) { return 1.0f; }

    float2 atlas_uv = local_uv * uv_offset_scale.zw + uv_offset_scale.xy;

    float2 half_texel = 0.5f / atlas_size;
    float4 clamp_rect = float4(uv_offset_scale.xy + half_texel, uv_offset_scale.xy + uv_offset_scale.zw - half_texel);

    SamplerComparisonState cmp = getCmpSampler(SAMPLER_SHADOW_CMP);

    return pcf3x3(atlas, cmp, atlas_uv, ndc.z - tile_depth_bias, atlas_size, clamp_rect);
}

float sampleCascade(ShadowTile cascade, float3 world_pos, float3 normal, float n_dot_l, float2 atlas_size)
{
    return sampleShadowTile(getTexture(globals.shadow_atlas_id), cascade.view_proj, cascade.uv_offset_scale,
                            cascade.texel_world_size, cascade.normal_bias, cascade.depth_bias, world_pos, normal,
                            n_dot_l, atlas_size);
}

public float shadowDirectional(float3 world_pos, float3 normal, float3 light_dir, uint light_index)
{
    // only first dir light, need to extend/redesign this later
    if (light_index != 0) { return 1.0f; }
    if (globals.shadow_atlas_id == 0xFFFFFFFFu || globals.shadow_cascade_count == 0) { return 1.0f; }
    if (globals.shadow_cascade_buffer == 0) { return 1.0f; }

    float view_depth = worldToView(world_pos).z;

    float fade = 1.0f;
    if (globals.shadow_fade_end > globals.shadow_fade_start)
    {
        fade = saturate((globals.shadow_fade_end - view_depth) / (globals.shadow_fade_end - globals.shadow_fade_start));
    }
    if (fade <= 0.0f) { return 1.0f; }

    let cascades = getBufferAs<ShadowTile>(globals.shadow_cascade_buffer);

    uint index = globals.shadow_cascade_count - 1;
    float split_near = 0.0f;
    for (uint i = 0; i < globals.shadow_cascade_count; i++)
    {
        if (view_depth < cascades[i].split_far)
        {
            index = i;
            break;
        }
        split_near = cascades[i].split_far;
    }

    Texture2D atlas = getTexture(globals.shadow_atlas_id);
    uint atlas_w, atlas_h;
    atlas.GetDimensions(atlas_w, atlas_h);
    float2 atlas_size = float2(atlas_w, atlas_h);

    float n_dot_l = saturate(dot(normal, light_dir));
    ShadowTile cascade = cascades[index];
    float lit = sampleCascade(cascade, world_pos, normal, n_dot_l, atlas_size);

    if (index + 1 < globals.shadow_cascade_count)
    {
        float band = (cascade.split_far - split_near) * CASCADE_BLEND_FRACTION;
        if (band > 0.0f)
        {
            float blend = saturate((view_depth - (cascade.split_far - band)) / band);
            if (blend > 0.0f)
            {
                float next = sampleCascade(cascades[index + 1], world_pos, normal, n_dot_l, atlas_size);
                lit = lerp(lit, next, blend);
            }
        }
    }

    return lerp(1.0f, lit, fade);
}

uint cubeFaceFromDirection(float3 dir)
{
    float3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) { return dir.x > 0.0f ? 0u : 1u; }
    if (a.y >= a.z) { return dir.y > 0.0f ? 2u : 3u; }
    return dir.z > 0.0f ? 4u : 5u;
}

float samplePunctualTile(uint tile_index, float3 world_pos, float3 normal, float3 light_dir)
{
    if (tile_index >= globals.punctual_shadow_count) { return 1.0f; }

    Texture2D atlas = getTexture(globals.punctual_shadow_atlas_id);
    uint atlas_w, atlas_h;
    atlas.GetDimensions(atlas_w, atlas_h);

    ShadowTile tile = getBufferAs<ShadowTile>(globals.punctual_shadow_buffer)[tile_index];
    float n_dot_l = saturate(dot(normal, light_dir));

    return sampleShadowTile(atlas, tile.view_proj, tile.uv_offset_scale, tile.texel_world_size, tile.normal_bias,
                            tile.depth_bias, world_pos, normal, n_dot_l, float2(atlas_w, atlas_h));
}

bool punctualShadowsReady(float slot)
{
    if (slot < 0.0f) { return false; } // this light was not given a tile this frame
    if (globals.punctual_shadow_atlas_id == 0xFFFFFFFFu) { return false; }
    if (globals.punctual_shadow_buffer == 0 || globals.punctual_shadow_count == 0) { return false; }
    return true;
}

public float pointShadow(PointLight light, float3 world_pos, float3 normal)
{
    if (!punctualShadowsReady(light.shadow_params.x)) { return 1.0f; }

    float3 to_receiver = world_pos - light.position_radius.xyz;
    uint face = cubeFaceFromDirection(to_receiver);

    return samplePunctualTile(uint(light.shadow_params.x) + face, world_pos, normal, normalize(-to_receiver));
}

public float spotShadow(SpotLight light, float3 world_pos, float3 normal)
{
    if (!punctualShadowsReady(light.shadow_params.x)) { return 1.0f; }

    float3 to_receiver = world_pos - light.position_range.xyz;

    return samplePunctualTile(uint(light.shadow_params.x), world_pos, normal, normalize(-to_receiver));
}

// shadow entry point for shaders
public float shadow(float3 world_pos, float3 normal, uint light_index)
{
    if (light_index != 0 || globals.dir_light_count == 0 || globals.dir_light_buffer == 0) { return 1.0f; }

    DirectionalLight dl = getBufferAs<DirectionalLight>(globals.dir_light_buffer)[0];
    return shadowDirectional(world_pos, normal, normalize(-dl.direction_intensity.xyz), light_index);
}

// debug: which cascade covers this point, 0xFFFFFFFF when past the last one
public uint shadowCascadeIndex(float3 world_pos)
{
    if (globals.shadow_cascade_buffer == 0 || globals.shadow_cascade_count == 0) { return 0xFFFFFFFFu; }

    float view_depth = worldToView(world_pos).z;
    let cascades = getBufferAs<ShadowTile>(globals.shadow_cascade_buffer);

    for (uint i = 0; i < globals.shadow_cascade_count; i++)
    {
        if (view_depth < cascades[i].split_far) { return i; }
    }

    return 0xFFFFFFFFu;
}

public float3 shadowCascadeTint(float3 world_pos)
{
    uint index = shadowCascadeIndex(world_pos);
    switch (index)
    {
    case 0: return float3(0.9f, 0.25f, 0.25f);
    case 1: return float3(0.25f, 0.9f, 0.25f);
    case 2: return float3(0.25f, 0.45f, 0.95f);
    case 3: return float3(0.9f, 0.85f, 0.25f);
    default: return float3(0.15f, 0.15f, 0.15f); // beyond the last cascade
    }
}

public uint clusterLightCount(float3 world_pos)
{
    if (globals.cluster_counts_buffer == 0) { return 0; }
    return getBufferAs<uint>(globals.cluster_counts_buffer)[clusterFromViewPos(worldToView(world_pos))];
}

public float3 clusterHeatmap(uint count)
{
    if (count == 0) { return float3(0.05f, 0.05f, 0.08f); }

    float t = saturate(float(count) / float(MAX_LIGHTS_PER_CLUSTER));
    float3 cold = float3(0.1f, 0.3f, 1.0f);
    float3 warm = float3(0.2f, 1.0f, 0.2f);
    float3 hot = float3(1.0f, 0.1f, 0.05f);

    return (t < 0.5f) ? lerp(cold, warm, t * 2.0f) : lerp(warm, hot, (t - 0.5f) * 2.0f);
}

float3 punctualLighting(Surface surface, float3 world_pos, float3 view_dir)
{
    float3 sum = float3(0.0f);

    if (globals.cluster_counts_buffer == 0)
    {
        for (uint p = 0; p < globals.point_light_count; p++)
        {
            PointLight pl = getBufferAs<PointLight>(globals.point_light_buffer)[p];
            sum += evaluateBRDF(surface, pl.evaluate(world_pos), view_dir) * pointShadow(pl, world_pos, surface.normal);
        }

        for (uint s = 0; s < globals.spot_light_count; s++)
        {
            SpotLight sl = getBufferAs<SpotLight>(globals.spot_light_buffer)[s];
            sum += evaluateBRDF(surface, sl.evaluate(world_pos), view_dir) * spotShadow(sl, world_pos, surface.normal);
        }

        return sum;
    }

    uint cluster = clusterFromViewPos(worldToView(world_pos));
    uint count = getBufferAs<uint>(globals.cluster_counts_buffer)[cluster];
    uint base = cluster * MAX_LIGHTS_PER_CLUSTER;

    for (uint i = 0; i < count; i++)
    {
        uint packed = getBufferAs<uint>(globals.cluster_indices_buffer)[base + i];
        uint index = packed & CLUSTER_LIGHT_INDEX_MASK;

        if ((packed >> CLUSTER_LIGHT_TYPE_SHIFT) == CLUSTER_LIGHT_POINT)
        {
            PointLight pl = getBufferAs<PointLight>(globals.point_light_buffer)[index];
            sum += evaluateBRDF(surface, pl.evaluate(world_pos), view_dir) * pointShadow(pl, world_pos, surface.normal);
        }
        else
        {
            SpotLight sl = getBufferAs<SpotLight>(globals.spot_light_buffer)[index];
            sum += evaluateBRDF(surface, sl.evaluate(world_pos), view_dir) * spotShadow(sl, world_pos, surface.normal);
        }
    }

    return sum;
}

public float3 applyLighting(Surface surface, float3 world_pos)
{
    float3 view_dir = normalize(globals.camera_pos.xyz - world_pos);
    float3 final_color = surface.albedo * AMBIENT;

    // dir lights are not clustered
    for (uint d = 0; d < globals.dir_light_count; d++)
    {
        DirectionalLight dl = getBufferAs<DirectionalLight>(globals.dir_light_buffer)[d];
        IncidentLight incident = dl.evaluate(world_pos);
        final_color += evaluateBRDF(surface, incident, view_dir) *
                       shadowDirectional(world_pos, surface.normal, incident.direction, d);
    }

    return final_color + punctualLighting(surface, world_pos, view_dir);
}
