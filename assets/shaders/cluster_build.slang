import tau_globals;

[[vk::binding(0, 2)]]
RWStructuredBuffer<uint> cluster_light_counts;
[[vk::binding(1, 2)]]
RWStructuredBuffer<uint> cluster_light_indices;

struct ClusterBounds
{
    float3 min_corner;
    float3 max_corner;
};

ClusterBounds clusterBounds(uint tile_x, uint tile_y, uint slice)
{
    float near_z = clusterSliceDepth(slice);
    float far_z = clusterSliceDepth(slice + 1);

    float2 ndc_min =
        float2(float(tile_x) / float(globals.cluster_tiles_x), float(tile_y) / float(globals.cluster_tiles_y));
    float2 ndc_max =
        float2(float(tile_x + 1) / float(globals.cluster_tiles_x), float(tile_y + 1) / float(globals.cluster_tiles_y));
    ndc_min = ndc_min * 2.0f - 1.0f;
    ndc_max = ndc_max * 2.0f - 1.0f;

    float2 x_slope = float2(ndc_min.x, ndc_max.x) / globals.projection[0][0];
    float2 y_slope = float2(ndc_min.y, ndc_max.y) / globals.projection[1][1];

    float4 xs = float4(x_slope.x * near_z, x_slope.x * far_z, x_slope.y * near_z, x_slope.y * far_z);
    float4 ys = float4(y_slope.x * near_z, y_slope.x * far_z, y_slope.y * near_z, y_slope.y * far_z);

    ClusterBounds bounds;
    bounds.min_corner = float3(min(min(xs.x, xs.y), min(xs.z, xs.w)), min(min(ys.x, ys.y), min(ys.z, ys.w)), near_z);
    bounds.max_corner = float3(max(max(xs.x, xs.y), max(xs.z, xs.w)), max(max(ys.x, ys.y), max(ys.z, ys.w)), far_z);
    return bounds;
}

bool sphereTouchesCluster(float3 center, float radius, ClusterBounds bounds)
{
    float3 closest = clamp(center, bounds.min_corner, bounds.max_corner);
    float3 offset = center - closest;
    return dot(offset, offset) <= radius * radius;
}

bool coneTouchesSphere(float3 tip, float3 axis, float range, float cos_outer, float3 center, float radius)
{
    float3 to_center = center - tip;
    float along = dot(to_center, axis);

    if (along > range + radius) { return false; }
    if (along < -radius) { return false; }

    float dist_sq = dot(to_center, to_center);
    float perp = sqrt(max(dist_sq - along * along, 0.0f));

    float sin_outer = sqrt(saturate(1.0f - cos_outer * cos_outer));
    float cone_dist = cos_outer * perp - sin_outer * along;

    return cone_dist <= radius;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 thread_id: SV_DispatchThreadID)
{
    uint cluster = thread_id.x;
    if (cluster >= clusterCount()) { return; }

    uint tiles_x = globals.cluster_tiles_x;
    uint tiles_y = globals.cluster_tiles_y;

    uint tile_x = cluster % tiles_x;
    uint tile_y = (cluster / tiles_x) % tiles_y;
    uint slice = cluster / (tiles_x * tiles_y);

    ClusterBounds bounds = clusterBounds(tile_x, tile_y, slice);

    uint base = cluster * MAX_LIGHTS_PER_CLUSTER;
    uint count = 0;

    for (uint p = 0; p < globals.point_light_count && count < MAX_LIGHTS_PER_CLUSTER; p++)
    {
        PointLight light = getBufferAs<PointLight>(globals.point_light_buffer)[p];
        if (sphereTouchesCluster(worldToView(light.position_radius.xyz), light.position_radius.w, bounds))
        {
            cluster_light_indices[base + count] = (CLUSTER_LIGHT_POINT << CLUSTER_LIGHT_TYPE_SHIFT) | p;
            count++;
        }
    }

    float3 bounds_center = (bounds.min_corner + bounds.max_corner) * 0.5f;
    float bounds_radius = length(bounds.max_corner - bounds_center);

    for (uint s = 0; s < globals.spot_light_count && count < MAX_LIGHTS_PER_CLUSTER; s++)
    {
        SpotLight light = getBufferAs<SpotLight>(globals.spot_light_buffer)[s];

        float3 tip = worldToView(light.position_range.xyz);
        if (!sphereTouchesCluster(tip, light.position_range.w, bounds)) { continue; }

        float3 axis = normalize(mul(globals.view, float4(light.direction_intensity.xyz, 0.0f)).xyz);

        if (!coneTouchesSphere(tip, axis, light.position_range.w, light.outer_cos.x, bounds_center, bounds_radius))
        {
            continue;
        }

        cluster_light_indices[base + count] = (CLUSTER_LIGHT_SPOT << CLUSTER_LIGHT_TYPE_SHIFT) | s;
        count++;
    }

    cluster_light_counts[cluster] = count;
}
