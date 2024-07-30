import "../BackendCommon";
import "../ParametricTraversal";
import RayMaskOptimizationLUT;

namespace backend::xbrickmap {

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

public struct VoxelMap : IVoxelMap, ITraversalStepper {
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

    public static const uint2 GridSize = uint2(GRID_SIZE_XZ, GRID_SIZE_Y);

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

    public HitInfo RayCast(int3 worldOrigin, float3 rayPos, float3 rayDir, bool coarse) {
        return ParametricRayCast(this, worldOrigin, rayPos, rayDir, coarse);
    }

    public bool GetStepPos(inout int3 ipos, float3 dir, bool coarse) {
        uint3 pos = uint3(ipos);
        uint3 brickPos = pos / BRICK_SIZE;
        uint3 sectorPos = pos / (BRICK_SIZE * 4);
        uint64_t occMask = GetBrickMask(sectorPos);

        uint maskIdx = GetLinearIndex(brickPos, 4, 4);
        int scale = BRICK_SIZE;

        if ((occMask >> maskIdx & 1) != 0) {
            occMask = GetVoxelMask(pos);
            maskIdx = GetLinearIndex(pos, 4, 4);
            scale = 1;

            if ((occMask >> maskIdx & 1) != 0) return false;
        } else if (occMask == 0) {
            occMask = GetSectorMask(sectorPos);
            maskIdx = GetLinearIndex(sectorPos, 4, 4);
            scale = BRICK_SIZE * 4;
        }

        // Mask optimization
        uint dirOctant = (dir.x < 0 ? 0 : 1) + (dir.y < 0 ? 0 : 2) + (dir.z < 0 ? 0 : 4);
        occMask &= RayMaskOptimizationLUT[maskIdx + dirOctant * 64];

        int lod = GetIsotropicLOD(occMask, maskIdx) * scale;

        if (coarse && lod < 4) {
            int3 occPos = tzcnt64(occMask) >> int3(0, 4, 2) & 3;
            ipos = (ipos & ~3) | occPos;
            return false;
        }
        ipos = AlignToCellBoundaries(ipos, dir, lod);
        return true;
    }

    static int GetIsotropicLOD(uint64_t mask, uint idx) {
        if (mask == 0) {
            return 4;
        }
        uint currMask = idx < 32 ? uint(mask) : uint(mask >> 32);

        if ((currMask >> (idx & 0b1010) & 0x00330033) == 0) {
            return 2;
        }
        return 1;
    }
};

}