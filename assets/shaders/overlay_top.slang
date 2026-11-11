module tau_overlay_top;
import tau_overlay;

[ShaderConfig("Custom", "TransparentAlpha", true, true, false)]
struct OverlayTopShader
{
    uint unused;
};

[shader("vertex")]
OverlayVertexOut vertexMain(uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID,
                            uint base_instance: SV_StartInstanceLocation)
{ return overlayVertex(vertex_id, instance_id, base_instance); }

[RenderTargetsConfig]
struct FragmentOut
{
    [RenderTarget("Swapchain")]
    float4 color : SV_Target0;
};

[shader("fragment")]
FragmentOut fragmentMain(OverlayVertexOut input)
{
    FragmentOut out;
    out.color = input.color;
    return out;
}
