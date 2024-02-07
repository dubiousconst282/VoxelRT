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

vec3 signbit(vec3 x) {
    return vec3(greaterThan(x, vec3(0.0)));
}
HitInfo rayMarch(vec3 origin, vec3 dir) {
    int i = 0;

    vec3 invDir = 1.0 / dir;
    ivec3 stepDir = mix(ivec3(+1), ivec3(-1), lessThan(dir, vec3(0)));
    vec3 tStart = (signbit(dir) - origin) * invDir;
    vec3 currPos = origin;
    vec3 sideDist;
    float tmin = 0;

    const int maxLevel = 5;
    int k = 3;

    for (; i < 128; i++) {
        ivec3 pos = u_WorldOrigin + ivec3(floor(currPos));

        if (!isInBounds(pos)) break;

        if (getOccupancy(pos >> k, k) != 0) {
            while (--k >= 0 && getOccupancy(pos >> k, k) != 0) ;
            if (k < 0) break;
        } else {
            while (++k < maxLevel && getOccupancy(pos >> k, k) == 0) ;
            k--;
        }
        
        int cellMask = (1 << k) - 1;
        pos.x = dir.x<0 ? (pos.x|cellMask) : (pos.x&~cellMask);
        pos.y = dir.y<0 ? (pos.y|cellMask) : (pos.y&~cellMask);
        pos.z = dir.z<0 ? (pos.z|cellMask) : (pos.z&~cellMask);

        ivec3 stepSize = ivec3(cellMask);

        if (u_AnisotropicTraversal) {
            // 0 1   8 16   x+
            // 2 4  32 64   y+
            //        z+
            uint mask = 0;
            for (int j = 0; j < 7; j++) {
                ivec3 cornerPos = (ivec3(j + 1) >> ivec3(0, 1, 2)) & 1;
                mask |= getOccupancy((pos >> k) + cornerPos * stepDir, k) << j;
            }

            // See GenAnisoExpansionTable.js
            const uint[] expansionTable = {
                0x45654567, 0x01210123, 0x41614163, 0x01210123, 0x45254523, 0x01210123, 0x41214123, 0x01210123,
                0x45654563, 0x01210123, 0x41614163, 0x01210123, 0x45254523, 0x01210123, 0x41214123, 0x01210123
            };

            uint tableIdx = mask * 4;
            uint expandMask = expansionTable[tableIdx >> 5] >> (tableIdx & 31);
            stepSize += (ivec3(expandMask) >> ivec3(0, 1, 2) & 1) << k;
        }

        pos = (pos-u_WorldOrigin) + stepSize * stepDir;
        sideDist = tStart + pos * invDir;
        tmin = min(min(sideDist.x, sideDist.y), sideDist.z) + 0.001;

        currPos = origin + tmin * dir;
    }

    // bool sideMaskX = sideDist.x < min(sideDist.y, sideDist.z);
    // bool sideMaskY = sideDist.y < min(sideDist.x, sideDist.z);
    // bool sideMaskZ = !sideMaskX && !sideMaskY;
    bvec3 sideMask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));

    uint voxelId = getVoxel(u_WorldOrigin + ivec3(floor(currPos)));

    HitInfo hit;
    hit.dist = tmin;
    hit.pos = currPos;
    hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
    hit.norm = mix(vec3(0), -sign(dir), sideMask);
    hit.mat = Palette[voxelId];
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

    if (u_ShowTraversalHeatmap) {
        o_FragColor.rgb=vec3(hit.iters)/64.0;
    }
}