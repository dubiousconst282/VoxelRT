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

bool isInBounds(ivec3 pos) {
    return uint(pos.x | pos.y | pos.z) < BRICK_SIZE * NUM_BRICKS_PER_AXIS;
}
uint getVoxel(ivec3 pos) {
    uvec3 brickPos = uvec3(pos) / BRICK_SIZE;
    uint brickIdx = brickPos.y * (NUM_BRICKS_PER_AXIS * NUM_BRICKS_PER_AXIS) +
                   brickPos.z * NUM_BRICKS_PER_AXIS +
                   brickPos.x;

    uvec3 slotPos = uvec3(BrickSlots[brickIdx]) >> uvec3(0, 10, 20) & 1023u;
    return imageLoad(u_BrickStorage, ivec3(slotPos * BRICK_SIZE + uvec3(pos) % BRICK_SIZE)).r;
}
uint getOccupancy(ivec3 pos, int k) {
    uint cell = texelFetch(u_OccupancyStorage, pos >> 1, k).r;
    uint idx = (pos.x & 1) | (pos.y & 1) << 1 | (pos.z & 1) << 2;
    return cell >> idx & 1;
}

vec3 mat_GetColor(Material mat) {
    uvec3 mask = uvec3(31, 63, 31);
    return vec3(uvec3(mat.Data) >> uvec3(11, 5, 0) & mask) * (1.0 / vec3(mask));
}
float mat_GetEmissionStrength(Material mat) {
    return float(mat.Data >> 16 & 15u) / 2.0;
}