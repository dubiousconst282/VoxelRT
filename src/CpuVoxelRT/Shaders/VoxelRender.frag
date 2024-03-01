in vec2 v_FragCoord;
out vec4 o_FragColor;

uniform mat4 u_InvProjMat;
uniform bool u_ShowTraversalHeatmap;
uniform bool u_AnisotropicTraversal;

buffer ssbo_Metrics {
    uint TotalIters;
};

#include "VoxelMap.glsl"

struct HitInfo {
    float dist;
    vec3 pos;
    vec3 norm;
    vec2 uv;
    Material mat;
    int iters;
};

vec3 clipRayToAABB(vec3 origin, vec3 dir, vec3 bbMin, vec3 bbMax) {
    vec3 invDir = 1.0 / dir;
    vec3 t1 = (bbMin - origin) * invDir;
    vec3 t2 = (bbMax - origin) * invDir;
    vec3 temp = t1;
    t1 = min(temp, t2);
    t2 = max(temp, t2);

    float tmin = max(t1.x, max(t1.y, t1.z));
    float tmax = min(t2.x, min(t2.y, t2.z));

    return tmin > 0 && tmin < tmax ? origin + dir * tmin : origin;
}

HitInfo rayMarch(vec3 origin, vec3 dir) {
    int i = 0;

    vec3 invDir = 1.0 / dir;
    ivec3 stepDir = mix(ivec3(+1), ivec3(-1), lessThan(dir, vec3(0)));
    vec3 tStart = (max(sign(dir), 0.0) - origin) * invDir;
    vec3 currPos = origin;
    vec3 sideDist;
    float tmin = 0;
    
    currPos = clipRayToAABB(currPos, dir, -u_WorldOrigin+1, vec3(GRID_SIZE_XZ, GRID_SIZE_Y,GRID_SIZE_XZ)-u_WorldOrigin-1);

    for (; i < 256; i++) {
        ivec3 pos = u_WorldOrigin + ivec3(floor(currPos));

        if (!isInBounds(pos)) break;

        int k = getLod(pos) - 1;
        if (k == 0 && getVoxel(pos) != 0) break;

        pos.x = dir.x<0 ? (pos.x&~k) : (pos.x|k);
        pos.y = dir.y<0 ? (pos.y&~k) : (pos.y|k);
        pos.z = dir.z<0 ? (pos.z&~k) : (pos.z|k);

        pos = (pos-u_WorldOrigin);
        sideDist = tStart + pos * invDir;
        tmin = min(min(sideDist.x, sideDist.y), sideDist.z);

        // Bias tmin by the smallest amount that is representable by a float (BitIncrement),
        // to avoid ray from getting stuck. Constant found by trial and error.
        tmin = uintBitsToFloat(floatBitsToUint(tmin) + 4);

        currPos = origin + tmin * dir;
    }

    bvec3 sideMask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));

    uint voxelId = getVoxel(u_WorldOrigin + ivec3(floor(currPos)));

    HitInfo hit;
    hit.dist = tmin;
    hit.pos = currPos;
    hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
    hit.norm = mix(vec3(0), -sign(dir), sideMask);
    hit.mat = u_VoxelData.Palette[voxelId];
    hit.iters=i;
    
    atomicAdd(TotalIters, uint(i));

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

    if (hit.norm.x != 0)
        o_FragColor *= (hit.norm.x < 0?0.7:0.8);
    else if (hit.norm.z != 0)
        o_FragColor *= (hit.norm.z < 0 ? 0.6 : 0.7);
    else if (hit.norm.y < 0)
        o_FragColor *= 0.5;

    if (u_ShowTraversalHeatmap) {
        o_FragColor.rgb=vec3(hit.iters)/64.0;
    }
}