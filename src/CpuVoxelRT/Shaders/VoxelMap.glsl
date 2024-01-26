#define NUM_BRICKS_PER_AXIS 128
#define BRICK_SIZE 8

struct Material {
    uint Data;
};
readonly buffer ssbo_VoxelMapData {
    Material Palette[256];
    uint BrickSlots[NUM_BRICKS_PER_AXIS * NUM_BRICKS_PER_AXIS * NUM_BRICKS_PER_AXIS];
};
layout(r8ui) uniform readonly uimage3D u_BrickStorage;
uniform usampler3D u_OccupancyStorage; // occupancy mips of N x N x (N/32) voxels

uint vox_Lookup(ivec3 pos) {
    if (uint(pos.x | pos.y | pos.z) >= BRICK_SIZE * NUM_BRICKS_PER_AXIS) return 511;

    //int k = 1;
    //uvec4 tx=texelFetch(u_OccupancyStorage, ivec3(pos.xy >> k, pos.z >> (5 + k)),k-1);
    //return ((tx.r >> (pos.z>>k) & 31u) & 1) * ((pos.x+pos.y+pos.z)%128+32);
    
    uvec3 brickPos = uvec3(pos) / BRICK_SIZE;
    uint brickIdx = brickPos.y * (NUM_BRICKS_PER_AXIS * NUM_BRICKS_PER_AXIS) +
                   brickPos.z * NUM_BRICKS_PER_AXIS +
                   brickPos.x;

    uvec3 slotPos = uvec3(BrickSlots[brickIdx]) >> uvec3(0, 10, 20) & 1023u;
    return imageLoad(u_BrickStorage, ivec3(slotPos * BRICK_SIZE + uvec3(pos) % BRICK_SIZE)).r;
}

uint vox_GetMipCellSize(ivec3 pos) {
    int k = 0;
    for (; k < 5; k++) {
        ivec3 mipPos = pos >> (k+1);
        uint column = texelFetch(u_OccupancyStorage, ivec3(mipPos.xy, mipPos.z >> 5), k).r;
        uint opaque = (column >> (mipPos.z & 31u)) & 1u;
        if (opaque != 0) break;
    }
    return 1 << k;
}


bool vox_IsEmpty(uint data) {
    return data == 0;
}
Material vox_GetMaterial(uint data) {
    return Palette[data];
}

vec3 mat_GetColor(Material mat) {
    uvec3 mask = uvec3(31, 63, 31);
    return vec3(uvec3(mat.Data) >> uvec3(11, 5, 0) & mask) * (1.0 / vec3(mask));
}
float mat_GetEmissionStrength(Material mat) {
    return float(mat.Data >> 16 & 15u) / 2.0;
}