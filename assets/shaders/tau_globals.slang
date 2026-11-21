module tau_globals;

__exported import tau_gpu;

#include "tau/rendering/shader_shared.h"

[[vk::binding(0, 0)]]
public SamplerState global_samplers[];
[[vk::binding(0, 0)]]
public SamplerComparisonState global_cmp_samplers[];
[[vk::binding(1, 0)]]
public Texture2D global_textures[];
[[vk::binding(1, 0)]]
public Texture3D global_textures_3d[];
[[vk::binding(1, 0)]]
public TextureCube global_textures_cube[];
[[vk::binding(2, 0)]]
public RWTexture2D<float4> global_storage_images[];
[[vk::binding(2, 0)]]
public RWTexture2D<uint> global_storage_images_uint[];
[[vk::binding(2, 0)]]
public RWTexture3D<float4> global_storage_images_3d[];

[[vk::binding(0, 1)]]
public ConstantBuffer<GlobalData> globals;

// sampler_type_e
public static const uint SAMPLER_LINEAR_REPEAT = TAU_SAMPLER_LINEAR_REPEAT;
public static const uint SAMPLER_LINEAR_CLAMP = TAU_SAMPLER_LINEAR_CLAMP;
public static const uint SAMPLER_NEAREST_REPEAT = TAU_SAMPLER_NEAREST_REPEAT;
public static const uint SAMPLER_NEAREST_CLAMP = TAU_SAMPLER_NEAREST_CLAMP;
public static const uint SAMPLER_SHADOW_CMP = TAU_SAMPLER_SHADOW_CMP;

public Texture2D getTexture(uint id) { return global_textures[NonUniformResourceIndex(id)]; }
public Texture3D getTexture3D(uint id) { return global_textures_3d[NonUniformResourceIndex(id)]; }
public TextureCube getTextureCube(uint id) { return global_textures_cube[NonUniformResourceIndex(id)]; }
public RWTexture2D<float4> getStorageImage(uint id) { return global_storage_images[NonUniformResourceIndex(id)]; }
public RWTexture2D<uint> getStorageImageUint(uint id)
{ return global_storage_images_uint[NonUniformResourceIndex(id)]; }
public RWTexture3D<float4> getStorageImage3D(uint id) { return global_storage_images_3d[NonUniformResourceIndex(id)]; }
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

    float4 world_pos = mul(obj.model_matrix, float4(vertex.position, 1.0f));

    VertexOut out;
    out.position = mul(globals.view_proj, world_pos);
    out.world_pos = world_pos.xyz;

    float3x3 normal_mat = float3x3(obj.normal_matrix[0].xyz, obj.normal_matrix[1].xyz, obj.normal_matrix[2].xyz);
    out.normal = normalize(mul(normal_mat, vertex.normal));
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

public extension PointLight : ILight { public IncidentLight evaluate(float3 world_pos) { IncidentLight out_light;
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

public extension DirectionalLight : ILight { public IncidentLight evaluate(float3 world_pos) { IncidentLight out_light;

out_light.direction = normalize(-direction_intensity.xyz);
out_light.radiance = color.xyz * direction_intensity.w;

return out_light;
}
};

public extension SpotLight : ILight { public IncidentLight evaluate(float3 world_pos) { IncidentLight out_light;
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

public float3 worldToView(float3 world_pos) { return mul(globals.view, float4(world_pos, 1.0f)).xyz; }

public uint clusterFromViewPos(float3 view_pos)
{
    if (view_pos.z <= 0.0f) { return 0; }

    float2 ndc = float2(globals.projection[0][0] * view_pos.x, globals.projection[1][1] * view_pos.y) / view_pos.z;
    float2 uv = saturate(ndc * 0.5f + 0.5f);

    uint tile_x = min(uint(uv.x * float(globals.cluster_tiles_x)), globals.cluster_tiles_x - 1);
    uint tile_y = min(uint(uv.y * float(globals.cluster_tiles_y)), globals.cluster_tiles_y - 1);

    return clusterFlatIndex(tile_x, tile_y, clusterSliceFromDepth(view_pos.z));
}
