module tau_overlay;

__exported import tau_globals;

public struct OverlayItem
{
    public float4x4 model;
    public float4 color;
    public uint64_t vertices;
    public uint64_t indices;
    public uint mode; // 0 mesh, 1 lines
    public float line_width;
    public float2 viewport_size;
};

public struct OverlayLine
{
    public float4 a;
    public float4 b;
    public float4 color;
};

public struct OverlayVertexOut
{
    public float4 position : SV_Position;
    public float4 color;
};

static const float NEAR_W = 1e-4;

public OverlayVertexOut overlayVertex(uint vertex_id, uint instance_id, uint base_instance)
{
    OverlayItem item = getBufferAs<OverlayItem>(pc.object_buffer)[instance_id + base_instance];
    OverlayVertexOut out;

    if (item.mode == 0)
    {
        Vertex vertex = getIndexedVertex(item.vertices, item.indices, vertex_id);
        out.position = mul(mul(float4(vertex.position, 1.0f), item.model), globals.view_proj);
        out.color = item.color;
        return out;
    }

    // 6 vertices per segment, a quad around it in screen space
    OverlayLine line = getBufferAs<OverlayLine>(item.vertices)[vertex_id / 6];
    uint corner = vertex_id % 6;

    float4 clip_a = mul(mul(float4(line.a.xyz, 1.0f), item.model), globals.view_proj);
    float4 clip_b = mul(mul(float4(line.b.xyz, 1.0f), item.model), globals.view_proj);

    out.color = line.color * item.color;
    if (clip_a.w < NEAR_W && clip_b.w < NEAR_W)
    {
        out.position = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return out;
    }

    // one end behind the camera: slide it along the segment
    if (clip_a.w < NEAR_W) { clip_a = lerp(clip_a, clip_b, (NEAR_W - clip_a.w) / (clip_b.w - clip_a.w)); }
    if (clip_b.w < NEAR_W) { clip_b = lerp(clip_b, clip_a, (NEAR_W - clip_b.w) / (clip_a.w - clip_b.w)); }

    float2 half_size = item.viewport_size * 0.5f;
    float2 dir = clip_b.xy / clip_b.w * half_size - clip_a.xy / clip_a.w * half_size;
    float len = length(dir);
    dir = len > 1e-6f ? dir / len : float2(1.0f, 0.0f);
    float2 offset = float2(-dir.y, dir.x) * (item.line_width * 0.5f) / half_size;

    bool at_b = corner == 2 || corner == 4 || corner == 5;
    float side = (corner == 1 || corner == 2 || corner == 4) ? -1.0f : 1.0f;

    float4 clip = at_b ? clip_b : clip_a;
    clip.xy += offset * side * clip.w;
    out.position = clip;
    return out;
}
