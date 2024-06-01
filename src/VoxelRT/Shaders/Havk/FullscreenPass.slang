struct FS_Vertex {
    float4 Position : SV_Position;
    float2 FragCoord : TEXCOORD0;
};

[shader("vertex")]
FS_Vertex VertexMain(uint vertexId: SV_VertexID) {
    // Drawing a single triangle instead of a quad will avoid helper fragment invocations
    // around the diagonal edges, assuming the hardware uses guard-band clipping.
    // - https://stackoverflow.com/a/59739538
    // - https://wallisc.github.io/rendering/2021/04/18/Fullscreen-Pass.html
    const float2 vertices[3] = { float2(-1, 3), float2(3, -1), float2(-1, -1) }; // CCW
    return {
        float4(vertices[vertexId], 0, 1),
        vertices[vertexId] * 0.5 + 0.5
    };
}