import tau_surface;

// unlit, so doesnt need shading
[ShaderConfig("Surface", "Opaque", true, true, true)]
struct UnlitShader
{
    [Color]
    float4 base_color;

    [Texture]
    uint albedo_tex;
    [Sampler]
    uint albedo_sampler;

    property Texture albedo
    {
        get {
            return { albedo_tex, albedo_sampler };
        }
    }
};

[shader("vertex")]
VertexOut vertexMain(uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID,
                     uint base_instance: SV_StartInstanceLocation)
{ return defaultVertexTransform(vertex_id, instance_id, base_instance); }

[shader("fragment")]
SurfaceOutput fragmentMain(VertexOut input)
{
    ObjectData obj = getObjectData(input.draw_id);
    UnlitShader mat = getMaterial<UnlitShader>(obj.material_offset);

    SurfaceOutput out;
    out.color = mat.albedo.sample(input.uv) * mat.base_color;
    return out;
}
