struct Material {
    uint Data;
};
struct Sector {
    uint BaseSlot;
    uint AllocMask_0;
    uint AllocMask_32; // TODO: replace these with uvec2 maybe
};
buffer ssbo_VoxelData {
    Material Palette[256];
    Sector Sectors[NUM_SECTORS_XZ * NUM_SECTORS_Y * NUM_SECTORS_XZ];
    uint BrickData[];
} b_VoxelData;

buffer ssbo_Occupancy {
    uint Data[];
} b_Occupancy;

uniform ivec3 u_WorldOrigin;

const uint GRID_SIZE_XZ = BRICK_SIZE * NUM_SECTORS_XZ * 4;
const uint GRID_SIZE_Y = BRICK_SIZE * NUM_SECTORS_Y * 4;

const uint BRICK_STRIDE = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE;
const uint OCC_STRIDE = (BRICK_SIZE / 4) * (BRICK_SIZE / 2) * (BRICK_SIZE / 4);

const uint NULL_OFFSET = ~0u;

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
    Sector sector = b_VoxelData.Sectors[getLinearIndex(brickPos / 4, NUM_SECTORS_XZ, NUM_SECTORS_Y)];
    
    uint slotIdx = sector.BaseSlot;
    uint allocMask = sector.AllocMask_0;
    uint brickMask = 1u << (brickIdx & 31u);

    if (brickIdx >= 32) {
        slotIdx += bitCount(allocMask);
        allocMask = sector.AllocMask_32;
    }
    if ((allocMask & brickMask) == 0) {
        return NULL_OFFSET;
    }
    slotIdx += bitCount(allocMask & (brickMask - 1u));
    
    return slotIdx;
}
uint getBrickVoxelId(uint slot, uvec3 pos) {
    uint dataOffset = slot * BRICK_STRIDE + getLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
    // Extract byte
    uint shift = (dataOffset * 8u) & 31u;
    return b_VoxelData.BrickData[dataOffset / 4u] >> shift & 255u;
}
uint getVoxel(ivec3 spos) {
    uvec3 pos = uvec3(spos);
    uint slot = getBrickDataSlot(pos / BRICK_SIZE);
    if (slot == NULL_OFFSET) return 0;

    return getBrickVoxelId(slot, pos);
}

vec3 mat_GetColor(Material mat) {
    uvec3 mask = uvec3(31, 63, 31);
    return vec3(uvec3(mat.Data) >> uvec3(11, 5, 0) & mask) * (1.0 / vec3(mask));
}
float mat_GetEmissionStrength(Material mat) {
    return float(mat.Data >> 16 & 15u) / 2.0;
}