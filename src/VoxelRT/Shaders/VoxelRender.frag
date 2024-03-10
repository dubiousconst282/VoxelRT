in vec2 v_FragCoord;
out vec4 o_FragColor;

uniform mat4 u_InvProjMat;
uniform bool u_ShowTraversalHeatmap;

#include "VoxelMap.glsl"
#include "VoxelTraversal.glsl"

void getPrimaryRay(vec2 uv, out vec3 rayPos, out vec3 rayDir) {
    vec4 near = u_InvProjMat * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec4 far = near + u_InvProjMat[2];
    rayPos = vec3(near * (1.0 / near.w));
    rayDir = normalize(vec3(far * (1.0 / far.w)));
}

void main() {
    vec3 rayDir, rayPos;
    getPrimaryRay(v_FragCoord, rayPos, rayDir);
    
    HitInfo hit;
    rayCast(rayPos, rayDir, hit);
    //o_FragColor = vec4(hit.norm *0.5+0.5, 1.0);
    o_FragColor = vec4(mat_GetColor(hit.mat), 1.0);

    if (hit.norm.x != 0)
        o_FragColor *= (hit.norm.x < 0?0.7:0.8);
    else if (hit.norm.z != 0)
        o_FragColor *= (hit.norm.z < 0 ? 0.5 : 0.6);
    else if (hit.norm.y < 0)
        o_FragColor *= 0.5;

    if (u_ShowTraversalHeatmap) {
        o_FragColor.rgb=vec3(hit.iters)/96.0;
    }
}