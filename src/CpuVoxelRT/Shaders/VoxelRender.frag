in vec2 v_FragCoord;
out vec4 o_FragColor;

uniform mat4 u_InvProjMat;

#include "VoxelMap.glsl"

struct HitInfo {
    float dist;
    vec3 norm;
    vec2 uv;
    Material mat;
};

vec3 getSideDist(vec3 x, vec3 s) {
    // s < 0.0 ? x : 1.0 - x;
    x = fract(x);
    return mix(1.0 - x, x, lessThan(s, vec3(0.0)));
}

// https://medium.com/@calebleak/raymarching-voxel-rendering-58018201d9d6
// Plane ray-marching is less accurate than DDA, but apparently easier to accelerate with distance fields.
HitInfo rayMarch(vec3 origin, vec3 dir) {
    vec3 deltaDist = abs(1.0 / dir);
    vec3 sideDist;

    float totalDist = 0.0;
    uint voxelId = 0;

    for (int i = 0; i < 256; i++) {
        vec3 pos = origin + dir * totalDist;
        voxelId = vox_Lookup(ivec3(floor(pos)));

        if (voxelId == 511 || !vox_IsEmpty(voxelId)) break;

        sideDist = getSideDist(pos, dir) * deltaDist;
        float nearestSideDist = min(min(sideDist.x, sideDist.y), sideDist.z);

        totalDist += max(vox_GetStepDist(voxelId), nearestSideDist + 1.0 / 4096);
    }

    // bool sideMaskX = sideDist.x < min(sideDist.y, sideDist.z);
    // bool sideMaskY = sideDist.y < min(sideDist.x, sideDist.z);
    // bool sideMaskZ = !sideMaskX && !sideMaskY;
    bvec3 sideMask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));

    HitInfo hit;
    hit.dist = totalDist;
    hit.norm = mix(vec3(0), -sign(dir), sideMask);
    hit.mat = vox_GetMaterial(voxelId);

    return hit;
}

void getPrimaryRay(vec2 uv, out vec3 rayPos, out vec3 rayDir) {
    vec4 near = u_InvProjMat * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec4 far = near + u_InvProjMat[2];
    rayPos = vec3(near * (1.0 / near.w));
    rayDir = normalize(vec3(far * (1.0 / far.w)));
}

void main() {
    vec3 rayDir, rayPos;
    getPrimaryRay(v_FragCoord, rayPos, rayDir);
    
    HitInfo hit = rayMarch(rayPos, rayDir);
    //o_FragColor = vec4(hit.norm *0.5+0.5, 1.0);
    o_FragColor = vec4(mat_GetColor(hit.mat), 1.0);
    
    uint x = uint(v_FragCoord.x * 32);
    uint y = uint(v_FragCoord.y * 32);
    //o_FragColor = vec4(mat_GetColor(Palette[(x + y * 32)]), 1.0);
    //fragColor.rgb=vec3(hit.numIters/128.0,0,0);
    //fragColor.rgb = vec3(0.1 * noiseDeriv);
}