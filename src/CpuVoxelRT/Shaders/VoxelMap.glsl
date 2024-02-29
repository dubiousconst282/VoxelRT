struct Material {
    uint Data;
};
struct Sector {
    uint BaseSlot;
    uint AllocMask_0;
    uint AllocMask_32;
};
readonly buffer ssbo_VoxelData {
    Material Palette[256];
    Sector Sectors[NUM_SECTORS_XZ * NUM_SECTORS_Y * NUM_SECTORS_XZ];
    uint BrickData[];
} u_VoxelData;

uniform ivec3 u_WorldOrigin;

const uint GRID_SIZE_XZ = BRICK_SIZE * NUM_SECTORS_XZ * 4;
const uint GRID_SIZE_Y = BRICK_SIZE * NUM_SECTORS_Y * 4;

bool isInBounds(ivec3 pos) {
    return uint(pos.x | pos.z) < GRID_SIZE_XZ && uint(pos.y) < GRID_SIZE_Y;
}

uint getLinearIndex(uvec3 pos, uint sizeXZ, uint sizeY) {
    return (pos.x & (sizeXZ - 1)) +
           (pos.z & (sizeXZ - 1)) * sizeXZ +
           (pos.y & (sizeY - 1)) * (sizeXZ * sizeXZ);
}
int getLod(ivec3 pos) {
    uint brickIdx = getLinearIndex(pos / BRICK_SIZE, 4, 4);
    Sector sector = u_VoxelData.Sectors[getLinearIndex(pos / (BRICK_SIZE * 4), NUM_SECTORS_XZ, NUM_SECTORS_Y)];

    if ((sector.AllocMask_0 | sector.AllocMask_32) == 0) {
        return BRICK_SIZE * 4;
    }
    
    uint currMask = brickIdx >= 32 ? sector.AllocMask_32 : sector.AllocMask_0;
    uint predMask = 1u << (brickIdx & 31u);

    if ((currMask & (0x00330033u << (brickIdx & 0xA))) == 0) {
        return BRICK_SIZE * 2;
    }
    if ((currMask & predMask) == 0) {
        return BRICK_SIZE;
    }
    return 1;
}
uint getVoxel(ivec3 pos) {
    uint brickIdx = getLinearIndex(pos / BRICK_SIZE, 4, 4);
    Sector sector = u_VoxelData.Sectors[getLinearIndex(pos / (BRICK_SIZE * 4), NUM_SECTORS_XZ, NUM_SECTORS_Y)];
    
    uint slotIdx = sector.BaseSlot;
    uint currMask = sector.AllocMask_0;
    uint predMask = 1u << (brickIdx & 31u);

    if (brickIdx >= 32) {
        slotIdx += bitCount(currMask);
        currMask = sector.AllocMask_32;
    }
    if ((currMask & predMask) == 0) {
        return 0;
    }
    slotIdx += bitCount(currMask & (predMask - 1u));

    slotIdx *= BRICK_SIZE*BRICK_SIZE*BRICK_SIZE;
    slotIdx += getLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
    
    return u_VoxelData.BrickData[slotIdx / 4u] >> (slotIdx * 8u) & 255u;
}

vec3 mat_GetColor(Material mat) {
    uvec3 mask = uvec3(31, 63, 31);
    return vec3(uvec3(mat.Data) >> uvec3(11, 5, 0) & mask) * (1.0 / vec3(mask));
}
float mat_GetEmissionStrength(Material mat) {
    return float(mat.Data >> 16 & 15u) / 2.0;
}