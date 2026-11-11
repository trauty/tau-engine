module tau_editor_selection;
import tau_globals;

// no depth test: the whole silhouette, even behind other objects
[ShaderConfig("Custom", "Opaque", false, false, false)]
struct EditorSelectionShader
{ uint unused; };

struct SelectionVertexOut
{
    float4 position : SV_Position;
    nointerpolation uint draw_id;
};

[shader("vertex")]
SelectionVertexOut vertexMain(uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID,
                              uint base_instance: SV_StartInstanceLocation)
{
    VertexOut vertex = defaultVertexTransform(vertex_id, instance_id, base_instance);

    SelectionVertexOut out;
    out.position = vertex.position;
    out.draw_id = vertex.draw_id;
    return out;
}

[RenderTargetsConfig]
struct FragmentOut
{
    [RenderTarget("R8_UNORM")]
    float selected : SV_Target0;
};

[shader("fragment")]
FragmentOut fragmentMain(SelectionVertexOut input)
{
    uint entity = getObjectData(input.draw_id).entity;

    // selected entities: material_buffer, count in custom_data
    bool selected = false;
    for (uint i = 0; i < pc.custom_data; i++)
    {
        if (getBufferAs<uint>(pc.material_buffer)[i] == entity)
        {
            selected = true;
            break;
        }
    }
    if (!selected) { discard; }

    FragmentOut out;
    out.selected = 1.0f;
    return out;
}
