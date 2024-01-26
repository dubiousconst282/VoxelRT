in vec2 v_FragCoord;
out vec4 o_FragColor;

uniform mat4 u_InvProjMat;

#include "VoxelMap.glsl"

struct HitInfo {
    float dist;
    vec3 norm;
    vec2 uv;
    Material mat;
    int iters;
};

vec3 getSideDist(vec3 x, vec3 s) {
    // s < 0.0 ? x : 1.0 - x;
    x = fract(x);
    return mix(1.0 - x, x, lessThan(s, vec3(0.0)));
}

HitInfo rayMarch(vec3 origin, vec3 dir) {
    vec3 deltaDist = abs(1.0 / dir);
    vec3 sideDist;

    float totalDist = 0.0;
    uint voxelId = 0;
    int i = 0;

    for (; i < 128; i++) {
        vec3 pos = origin + dir * totalDist;
        ivec3 gridPos = ivec3(floor(pos));
        voxelId = vox_Lookup(gridPos);

        if (voxelId == 511 || !vox_IsEmpty(voxelId)) break;

        float cellSize = float(vox_GetMipCellSize(gridPos));
        sideDist = getSideDist(pos/cellSize, dir) * (deltaDist * cellSize);
        float nearestSideDist = min(min(sideDist.x, sideDist.y), sideDist.z);

        totalDist += nearestSideDist + 1.0 / 4096;
    }

    // bool sideMaskX = sideDist.x < min(sideDist.y, sideDist.z);
    // bool sideMaskY = sideDist.y < min(sideDist.x, sideDist.z);
    // bool sideMaskZ = !sideMaskX && !sideMaskY;
    bvec3 sideMask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));

    HitInfo hit;
    hit.dist = totalDist;
    hit.norm = mix(vec3(0), -sign(dir), sideMask);
    hit.mat = vox_GetMaterial(voxelId);
    hit.iters=i;

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

    /*uint sz=vox_GetMipCellSize(ivec3(rayPos+rayDir*(hit.dist)));
    if(sz==1)o_FragColor.r+=0.2;
    if(sz==2)o_FragColor.r+=0.5;
    if(sz==4)o_FragColor.g+=0.2;
    if(sz==8)o_FragColor.g+=0.5;
    if(sz>=16)o_FragColor.b+=0.5;*/
    //o_FragColor.rgb=vec3(hit.iters)/50.0;
    
    uint x = uint(v_FragCoord.x * 32);
    uint y = uint(v_FragCoord.y * 32);
    //o_FragColor = vec4(mat_GetColor(Palette[(x + y * 32)]), 1.0);
    //fragColor.rgb=vec3(hit.numIters/128.0,0,0);
    //fragColor.rgb = vec3(0.1 * noiseDeriv);
}