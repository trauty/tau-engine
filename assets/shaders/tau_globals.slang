module tau_globals;

#include "tau/rendering/shader_shared.h"

[[vk::binding(0, 0)]]
public SamplerState global_samplers[];
[[vk::binding(0, 0)]]
public SamplerComparisonState global_cmp_samplers[];
[[vk::binding(1, 0)]]
public Texture2D global_textures[];
[[vk::binding(2, 0)]]
public RWTexture2D<float4> global_storage_images[];

[[vk::binding(0, 1)]]
public ConstantBuffer<GlobalData> globals;

// sampler_type_e
public static const uint SAMPLER_LINEAR_REPEAT = TAU_SAMPLER_LINEAR_REPEAT;
public static const uint SAMPLER_LINEAR_CLAMP = TAU_SAMPLER_LINEAR_CLAMP;
public static const uint SAMPLER_NEAREST_REPEAT = TAU_SAMPLER_NEAREST_REPEAT;
public static const uint SAMPLER_NEAREST_CLAMP = TAU_SAMPLER_NEAREST_CLAMP;
public static const uint SAMPLER_SHADOW_CMP = TAU_SAMPLER_SHADOW_CMP;

public Texture2D getTexture(uint id) { return global_textures[NonUniformResourceIndex(id)]; }
public SamplerState getSampler(uint id) { return global_samplers[NonUniformResourceIndex(id)]; }
public SamplerComparisonState getCmpSampler(uint id) { return global_cmp_samplers[NonUniformResourceIndex(id)]; }

public struct Buffer<T>
{
    public uint64_t address;

    public __subscript(uint i)->T
    {
        get {
            return load_at(i * sizeof(T));
        }
    }

    public T load_at(uint byte_offset)
    {
        Ptr<T> p = (Ptr<T>)(address + byte_offset);
        return *p;
    }
}

public Buffer<T> getBufferAs<T>(uint64_t address)
{
    Buffer<T> b;
    b.address = address;
    return b;
}

public struct Texture
{
    public uint tex;
    public uint sampler;

    public Texture2D tex2d() { return getTexture(tex); }
    public SamplerState samplerState() { return getSampler(sampler); }
    public float4 sample(float2 uv) { return getTexture(tex).Sample(getSampler(sampler), uv); }
}

public struct Vertex
{
    public float3 position;
    public float3 normal;
    public float2 uv;
};

public struct VertexOut
{
    public float4 position : SV_Position;
    public float3 world_pos;
    public float3 normal;
    public float2 uv;
    public uint draw_id;
};

public struct ObjectData
{
    public float4x4 model_matrix;
    public float4x4 normal_matrix;
    public uint64_t vertex_buffer;
    public uint64_t index_buffer;
    public uint material_offset;
    public uint entity;
    public uint _pad1;
    public uint _pad2;
};

public struct CullData
{
    public float4 bounding_sphere; // xyz = world centre, w = radius
    public uint index_count;
    public uint pipeline_index;
    public uint flags;           // bit0 = shadow caster
    public uint draw_bin_offset; // start of this pipeline's run in the indirect buffer
};

public struct ShadowTile
{
    public float4x4 view_proj;
    public float4 uv_offset_scale; // xy = atlas uv offset, zw = atlas uv scale
    public float texel_world_size; // world units per shadow texel
    public float normal_bias;      // in shadow texels
    public float depth_bias;       // in light space ndc depth
    public float split_far;        // view space distance at which this cascade stops
};

public struct GlobalData
{
    public float4x4 view;
    public float4x4 projection;
    public float4x4 view_proj;
    public float4 camera_pos;
    public float4 frustum_planes[6];
    public float4x4 inv_view_proj;
    public uint64_t dir_light_buffer;
    public uint64_t point_light_buffer;
    public uint64_t spot_light_buffer;

    public uint64_t cluster_counts_buffer;  // one uint per cluster
    public uint64_t cluster_indices_buffer; // MAX_LIGHTS_PER_CLUSTER uints per cluster
    public uint64_t shadow_cascade_buffer;

    public float time;
    public uint dir_light_count;
    public uint point_light_count;
    public uint spot_light_count;
    public uint object_count;
    public uint active_pipeline_count;
    public uint cull_flags; // bit0 = shadow only view

    public uint debug_view;

    public uint shadow_atlas_id; // bindless texture id of the cascade atlas
    public uint shadow_cascade_count;
    public float shadow_fade_start; // view distance where shadows start fading out
    public float shadow_fade_end;   // and where are fully faded

    public float cluster_z_near;
    public float cluster_z_far;

    public uint cluster_tiles_x;
    public uint cluster_tiles_y;

    public uint64_t punctual_shadow_buffer;
    public uint punctual_shadow_atlas_id;
    public uint punctual_shadow_count;
};

public struct PushConstants
{
    public uint64_t object_buffer;
    public uint64_t material_buffer;
    public uint custom_data;
    public uint texture_id;
};

[[vk::push_constant]]
public ConstantBuffer<PushConstants> pc;

public ObjectData getObjectData(uint object_id) { return getBufferAs<ObjectData>(pc.object_buffer)[object_id]; }

public Vertex getVertex(uint64_t vertex_buffer, uint vertex_id)
{ return getBufferAs<Vertex>(vertex_buffer)[vertex_id]; }

public Vertex getIndexedVertex(uint64_t vertex_buffer, uint64_t index_buffer, uint vertex_id)
{
    uint actual_vertex_index = getBufferAs<uint>(index_buffer)[vertex_id];
    return getBufferAs<Vertex>(vertex_buffer)[actual_vertex_index];
}

public T getMaterial<T>(uint byte_offset) { return getBufferAs<T>(pc.material_buffer).load_at(byte_offset); }

public VertexOut defaultVertexTransform(uint vertex_id, uint instance_id, uint base_instance)
{
    uint actual_instance_id = instance_id + base_instance;

    ObjectData obj = getObjectData(actual_instance_id);

    Vertex vertex = getIndexedVertex(obj.vertex_buffer, obj.index_buffer, vertex_id);

    float4 world_pos = mul(float4(vertex.position, 1.0f), obj.model_matrix);

    VertexOut out;
    out.position = mul(world_pos, globals.view_proj);
    out.world_pos = world_pos.xyz;

    float3x3 normal_mat = float3x3(obj.normal_matrix[0].xyz, obj.normal_matrix[1].xyz, obj.normal_matrix[2].xyz);
    out.normal = normalize(mul(vertex.normal, normal_mat));
    out.uv = vertex.uv;
    out.draw_id = actual_instance_id;

    return out;
}

[__AttributeUsage(_AttributeTargets.Struct)]
public struct ShaderConfigAttribute
{
    // domain: decides what this shader is:
    //   Surface: drawn in the engine's mesh passes, has ObjectData, blend and shadow apply
    //   PostProcess: fullscreen, reads the frame, no ObjectData and no shadow
    //   Custom: the engine does not route it, custom routing
    public string domain;
    public string blend_mode; // Opaque, Cutout, TransparentAdd, TransparentMult, TransparentAlpha
    public bool depth_write;
    public bool depth_test;
    public bool casts_shadow;
};

[__AttributeUsage(_AttributeTargets.Struct)]
public struct RenderTargetsConfigAttribute
{
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct RenderTargetAttribute
{
    public string format_alias;
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct HiddenAttribute
{
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct TextureAttribute
{
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct SamplerAttribute
{
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct ColorAttribute
{
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct EnumAttribute
{
    public string labels;
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct RangeAttribute
{
    public float min;
    public float max;
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct TooltipAttribute
{
    public string text;
};

public struct IncidentLight
{
    public float3 direction;
    public float3 radiance;
};

public float distanceAttenuation(float distance_sq, float range)
{
    float factor = distance_sq / max(range * range, 1e-4f);
    float smooth_factor = saturate(1.0f - factor * factor);
    return (smooth_factor * smooth_factor) / max(distance_sq, 1e-4f);
}

public interface ILight
{
    IncidentLight evaluate(float3 world_pos);
};

public struct PointLight : ILight
{
    public float4 position_radius; // xyz = pos, w = radius
    public float4 color_intensity; // xyz = color, w = intensity
    public float4 shadow_params;   // x = the punctual shadow tile this light owns, negative when it casts none

    public IncidentLight evaluate(float3 world_pos)
    {
        IncidentLight out_light;
        out_light.radiance = float(0.0f);
        out_light.direction = float3(0.0f, 1.0f, 0.0f);

        float3 light_vec = position_radius.xyz - world_pos;
        float distance_sq = dot(light_vec, light_vec);
        float distance = sqrt(max(distance_sq, 1e-8f));

        float attenuation = distanceAttenuation(distance_sq, position_radius.w);
        if (attenuation <= 0.0f) { return out_light; }

        out_light.direction = light_vec / distance;
        out_light.radiance = color_intensity.xyz * color_intensity.w * attenuation;

        return out_light;
    }
};

public struct DirectionalLight : ILight
{
    public float4 direction_intensity; // xyz = dir, w = intensity
    public float4 color;               // xyz = color, w = padding

    public IncidentLight evaluate(float3 world_pos)
    {
        IncidentLight out_light;

        out_light.direction = normalize(-direction_intensity.xyz);
        out_light.radiance = color.xyz * direction_intensity.w;

        return out_light;
    }
};

public struct SpotLight : ILight
{
    public float4 position_range;      // xyz = pos, w = range
    public float4 direction_intensity; // xyz = dir, w = intensity
    public float4 color_inner_cos;     // xyz = color, w = inner_cosine
    public float4 outer_cos;           // x = outer_cosine, yzw = padding
    public float4 shadow_params;       // x = the punctual shadow tile this light owns, negative when it casts none

    public IncidentLight evaluate(float3 world_pos)
    {
        IncidentLight out_light;
        out_light.radiance = float(0.0f);
        out_light.direction = float3(0.0f, 1.0f, 0.0f);

        float3 light_vec = position_range.xyz - world_pos;
        float distance_sq = dot(light_vec, light_vec);
        float distance = sqrt(max(distance_sq, 1e-8f));

        float dist_attenuation = distanceAttenuation(distance_sq, position_range.w);

        if (dist_attenuation <= 0.0f) { return out_light; }

        float3 light_dir = light_vec / distance;

        float theta = dot(light_dir, normalize(-direction_intensity.xyz));
        float inner_cos = color_inner_cos.w;
        float outer_cos = outer_cos.x;

        float epsilon = inner_cos - outer_cos;
        float cone_attenuation = clamp((theta - outer_cos) / epsilon, 0.0f, 1.0f);

        if (cone_attenuation <= 0.0f) { return out_light; }

        out_light.direction = light_dir;
        out_light.radiance = color_inner_cos.xyz * direction_intensity.w * dist_attenuation * cone_attenuation;

        return out_light;
    }
};

// clustering
public static const uint CLUSTER_SLICES_Z = TAU_CLUSTER_SLICES_Z;

public uint clusterCount() { return globals.cluster_tiles_x * globals.cluster_tiles_y * CLUSTER_SLICES_Z; }

public static const uint MAX_LIGHTS_PER_CLUSTER = TAU_MAX_LIGHTS_PER_CLUSTER;

public static const uint CLUSTER_LIGHT_TYPE_SHIFT = 24;
public static const uint CLUSTER_LIGHT_INDEX_MASK = 0x00ffffffu;
public static const uint CLUSTER_LIGHT_POINT = 0;
public static const uint CLUSTER_LIGHT_SPOT = 1;

public float clusterSliceDepth(uint slice)
{
    float ratio = globals.cluster_z_far / max(globals.cluster_z_near, 1e-4f);
    return globals.cluster_z_near * pow(ratio, float(slice) / float(CLUSTER_SLICES_Z));
}

public uint clusterSliceFromDepth(float view_z)
{
    float ratio = globals.cluster_z_far / max(globals.cluster_z_near, 1e-4f);
    float slice = log(max(view_z, globals.cluster_z_near) / globals.cluster_z_near) / log(max(ratio, 1.0001f));
    return min(uint(max(slice, 0.0f) * float(CLUSTER_SLICES_Z)), CLUSTER_SLICES_Z - 1);
}

public uint clusterFlatIndex(uint tile_x, uint tile_y, uint slice)
{ return (slice * globals.cluster_tiles_y + tile_y) * globals.cluster_tiles_x + tile_x; }

public float3 worldToView(float3 world_pos) { return mul(float4(world_pos, 1.0f), globals.view).xyz; }

public uint clusterFromViewPos(float3 view_pos)
{
    if (view_pos.z <= 0.0f) { return 0; }

    float2 ndc = float2(globals.projection[0][0] * view_pos.x, globals.projection[1][1] * view_pos.y) / view_pos.z;
    float2 uv = saturate(ndc * 0.5f + 0.5f);

    uint tile_x = min(uint(uv.x * float(globals.cluster_tiles_x)), globals.cluster_tiles_x - 1);
    uint tile_y = min(uint(uv.y * float(globals.cluster_tiles_y)), globals.cluster_tiles_y - 1);

    return clusterFlatIndex(tile_x, tile_y, clusterSliceFromDepth(view_pos.z));
}
