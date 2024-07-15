public struct VoxelStorage {
    public Material Palette[256];
    public SectorInfo Sectors[VoxelMap.NUM_SECTORS];
    public uint64_t AllocMasks[VoxelMap.NUM_SECTORS];
    public uint64_t SectorMasks[VoxelMap.NUM_SECTORS / 64];
    public uint8_t BrickVoxelData[];
}

public struct SectorInfo {
    uint32_t PackedData;

    public property uint BaseAddress {
        get { return PackedData & ~15u; }
    }
    public property uint LevelOfDetail {
        get { return PackedData & 3; }
    }

    public property uint BrickStride {
        get { return VoxelMap.BRICK_STRIDE >> (LevelOfDetail * 3); }
    }

    [__ref]
    void SendRequestIfUnloaded() {
        if ((PackedData >> 2 & 1) != 0) return;

        InterlockedOr(PackedData, 1 << 2);

        // elect should prevent dupes within wave and serve as scalar hint.
        if (WaveMaskIsFirstLane(BaseAddress == 0)) {
            // InterlockedAdd()
        }
    }
}

public struct VoxelMap {
    // NOTE: keep in sync with GpuRenderer.cpp
    public static const uint
        BRICK_SIZE = 8,
        NUM_SECTORS_XZ = 4096 / (BRICK_SIZE * 4),
        NUM_SECTORS_Y = 2048 / (BRICK_SIZE * 4);

    public static const uint
        GRID_SIZE_XZ = BRICK_SIZE * NUM_SECTORS_XZ * 4,
        GRID_SIZE_Y = BRICK_SIZE * NUM_SECTORS_Y * 4,

        BRICK_STRIDE = BRICK_SIZE * BRICK_SIZE * BRICK_SIZE,
        OCC_STRIDE = (BRICK_SIZE / 4) * (BRICK_SIZE / 4) * (BRICK_SIZE / 4),

        NUM_SECTORS = NUM_SECTORS_XZ * NUM_SECTORS_XZ * NUM_SECTORS_Y;

    public VoxelStorage* Storage;
    public uint64_t* VoxelOccupancy;

    public static bool CheckBounds(int3 pos) {
        return uint(pos.x | pos.z) < GRID_SIZE_XZ && uint(pos.y) < GRID_SIZE_Y;
    }
    public uint GetBrickVoxelId(uint address, uint3 pos) {
        uint offset = GetLinearIndex(pos, BRICK_SIZE, BRICK_SIZE);
        return Storage->BrickVoxelData[address + offset];
    }

    public uint GetBrickAddress(uint3 brickPos) {
        uint sectorIdx = GetLinearIndex(brickPos / 4, NUM_SECTORS_XZ, NUM_SECTORS_Y);
        uint brickIdx = GetLinearIndex(brickPos, 4, 4);
        uint64_t allocMask = Storage->AllocMasks[sectorIdx];
        
        if ((allocMask >> brickIdx & 1) != 0) {
            SectorInfo sector = Storage->Sectors[sectorIdx];
            return sector.BaseAddress + popcnt_var64(allocMask, brickIdx) * sector.BrickStride;
        }
        return 0;
    }

    public uint64_t GetVoxelMask(uint3 pos) {
        uint address = GetBrickAddress(pos / BRICK_SIZE);
        uint cellOffset = GetLinearIndex(pos / 4, BRICK_SIZE / 4, BRICK_SIZE / 4);
        return VoxelOccupancy[(address / 64) + cellOffset];
    }
    public uint64_t GetBrickMask(uint3 sectorPos) {
        uint sectorIdx = GetLinearIndex(sectorPos, NUM_SECTORS_XZ, NUM_SECTORS_Y);
        return Storage->AllocMasks[sectorIdx];
    }
    public uint64_t GetSectorMask(uint3 sectorPos) {
        return Storage->SectorMasks[GetLinearIndex(sectorPos / 4, NUM_SECTORS_XZ / 4, NUM_SECTORS_Y / 4)];
    }

    public uint GetVoxelId(uint3 pos) {
        uint address = GetBrickAddress(pos / BRICK_SIZE);
        if (address == 0) {
            return 0;
        }
        return GetBrickVoxelId(address, pos);
    }

    public Material GetMaterialFromId(uint id) {
        return Storage->Palette[id];
    }
};

// Count number of set bits in variable range [0..width]
public uint popcnt_var64(uint64_t mask, uint width) {
    // return popcnt64(mask & ((1ull << mask) - 1));
    uint himask = uint(mask);
    uint count = 0;

    if (width >= 32) {
        count = countbits(himask);
        himask = uint(mask >> 32);
    }
    uint m = 1u << (width & 31u);
    count += countbits(himask & (m - 1u));

    return count;
}
public uint popcnt64(uint64_t mask) {
    uint a = countbits(uint(mask >> 0));
    uint b = countbits(uint(mask >> 32));
    return a + b;
}
public uint tzcnt64(uint64_t mask) {
    uint a = uint(mask >> 0);
    uint b = uint(mask >> 32);
    uint c = firstbitlow(a != 0 ? a : b);
    return a != 0 ? c : c + 32;
}

// Returns linear index for 3D grid in XZY order. Sizes **must** be power of two.
public uint GetLinearIndex(uint3 pos, uint sizeXZ, uint sizeY) {
    return (pos.x & (sizeXZ - 1)) +
           (pos.z & (sizeXZ - 1)) * sizeXZ +
           (pos.y & (sizeY - 1)) * (sizeXZ * sizeXZ);
}
public uint3 GetPosFromLinearIndex(uint idx, uint sizeXZ, uint sizeY) {
    uint3 pos;
    pos.x = idx % sizeXZ;
    pos.z = (idx / sizeXZ) % sizeXZ;
    pos.y = (idx / sizeXZ) / sizeXZ;
    return pos;
}

public struct Material {
    uint2 Data;

    public property float3 Color {
        get {
            uint3 mask = uint3(31, 63, 31);
            float3 color = float3(Data.xxx >> uint3(11, 5, 0) & mask) / float3(mask);
            return color * color;  // srgb gamma hack
        }
    }
    public property float Emission {
        get {
            return f16tof32(Data.x >> 16);
        }
    }
    public property float MetalFuzziness {
        get {
            return (Data.y & 255) / 255.0;
        }
    }
};
