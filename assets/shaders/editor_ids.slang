module tau_editor_ids;
import tau_globals;

[ShaderConfig("Custom", "Opaque", true, true, false)]
struct EditorIdsShader
{
    uint unused;
};

struct IdVertexOut
{
    float4 position : SV_Position;
    nointerpolation uint draw_id;
};

[shader("vertex")]
IdVertexOut vertexMain(uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID,
                       uint base_instance: SV_StartInstanceLocation)
{
    VertexOut vertex = defaultVertexTransform(vertex_id, instance_id, base_instance);

    IdVertexOut out;
    out.position = vertex.position;
    out.draw_id = vertex.draw_id;
    return out;
}

[RenderTargetsConfig]
struct FragmentOut
{
    [RenderTarget("R32_UINT")]
    uint id : SV_Target0;
};

[shader("fragment")]
FragmentOut fragmentMain(IdVertexOut input)
{
    FragmentOut out;
    out.id = getObjectData(input.draw_id).entity;
    return out;
}
