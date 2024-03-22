const uint GRID_SIZE_XZ = BRICK_SIZE * NUM_SECTORS_XZ * 4;
const uint GRID_SIZE_Y = BRICK_SIZE * NUM_SECTORS_Y * 4;

const uint BRICK_STRIDE = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
const uint OCC_STRIDE = (BRICK_SIZE / 4) * (BRICK_SIZE / 4) * (BRICK_SIZE / 4);

const uint NUM_SECTORS = NUM_SECTORS_XZ * NUM_SECTORS_XZ * NUM_SECTORS_Y;

const uint NULL_OFFSET = ~0u;

struct Material {
    uint Data;
};
buffer ssbo_VoxelData {
    Material Palette[256];
    uint BaseSlots[NUM_SECTORS];
    uvec2 AllocMasks[NUM_SECTORS];
    uvec2 SectorOccupancy[NUM_SECTORS / 64];  // Occupancy masks at sector level
    uint BrickData[];
} b_VoxelData;

buffer ssbo_VoxelOccupancy {
    uvec2 Data[];
} b_VoxelOccupancy;

uniform ivec3 u_WorldOrigin;

bool isInBounds(ivec3 pos) {
    return uint(pos.x | pos.z) < GRID_SIZE_XZ && uint(pos.y) < GRID_SIZE_Y;
}

uint getLinearIndex(uvec3 pos, uint sizeXZ, uint sizeY) {
    return (pos.x & (sizeXZ - 1)) +
           (pos.z & (sizeXZ - 1)) * sizeXZ +
           (pos.y & (sizeY - 1)) * (sizeXZ * sizeXZ);
}
uvec3 getPosFromLinearIndex(uint idx, uint sizeXZ, uint sizeY) {
    uvec3 pos;
    pos.x = idx % sizeXZ;
    pos.z = (idx / sizeXZ) % sizeXZ;
    pos.y = (idx / sizeXZ) / sizeXZ;
    return pos;
}

uint getBrickDataSlot(uvec3 brickPos) {
    uint brickIdx = getLinearIndex(brickPos, 4, 4);
    uint sectorIdx = getLinearIndex(brickPos / 4, NUM_SECTORS_XZ, NUM_SECTORS_Y);
    
    uvec2 allocMask = b_VoxelData.AllocMasks[sectorIdx];
    uint currMask = allocMask.x;
    uint slotIdx = b_VoxelData.BaseSlots[sectorIdx];
    uint brickMask = 1u << (brickIdx & 31u);

    if (brickIdx >= 32) {
        slotIdx += bitCount(currMask);
        currMask = allocMask.y;
    }
    if ((currMask & brickMask) == 0) {
        return NULL_OFFSET;
    }
    slotIdx += bitCount(currMask & (brickMask - 1u));
    
    return slotIdx;
}
uint getVoxelId(uint brickSlot, uvec3 pos) {
    uint dataOffset = brickSlot * BRICK_STRIDE + getLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
    // Extract byte
    uint shift = (dataOffset * 8u) & 31u;
    return b_VoxelData.BrickData[dataOffset / 4u] >> shift & 255u;
}
Material getVoxelMaterial(ivec3 spos) {
    uvec3 pos = uvec3(spos);
    uint slot = getBrickDataSlot(pos / BRICK_SIZE);
    uint voxelId = slot == NULL_OFFSET ? 0 : getVoxelId(slot, pos);
    return b_VoxelData.Palette[voxelId];
}

vec3 getMaterialColor(Material mat) {
    uvec3 mask = uvec3(31, 63, 31);
    return vec3(uvec3(mat.Data) >> uvec3(11, 5, 0) & mask) * (1.0 / vec3(mask));
}
float getMaterialEmission(Material mat) {
    return float(mat.Data >> 16 & 15u) / 2.0;
}