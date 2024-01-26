#include "VoxelMap.h"
#include <Common/BinaryIO.h>

namespace cvox {

static glm::uvec3 GetGpuBrickSlot(uint32_t slotId) {
    const uint32_t mask = 0x49249249; // 0b001 on all consecutive bits

    // Morton de-interleaving. This is quite arbitrary but allows
    // the texture size to be cubed in respect to number of bricks.
    auto pos = glm::uvec3(0);
    pos.x = _pext_u32(slotId, mask << 0);
    pos.y = _pext_u32(slotId, mask << 1);
    pos.z = _pext_u32(slotId, mask << 2);
    return pos;
}
static glm::uvec3 GetGpuTextureSize(uint32_t numBricks) {
    uint32_t dim = ceil(cbrt(numBricks));
    return glm::uvec3(dim * VoxelMap::BrickSize);
}

void VoxelMap::SyncGpuBuffers() {
    if (GpuMetaStorage == nullptr) {
        GpuMetaStorage = std::make_unique<ogl::Buffer>(sizeof(GpuMeta), GL_MAP_WRITE_BIT);

        uint32_t occSize = OccMap.CubeSize;
        GpuOccupancyStorage = std::make_unique<ogl::Texture3D>(occSize, occSize, (occSize + 31) / 32, OccupancyMap::NumLevels, GL_R32UI);
    }
    glm::uvec3 texSize = GetGpuTextureSize(BrickStorage.capacity());

    if (GpuBrickStorage == nullptr || GpuBrickStorage->Width < texSize.x || GpuBrickStorage->Height < texSize.y || GpuBrickStorage->Depth < texSize.z) {
        GpuBrickStorage = std::make_unique<ogl::Texture3D>(texSize.x, texSize.y, texSize.z, 1, GL_R8UI);

        for (uint32_t i = 0; i < NumTotalBricks; i++) {
            if (BrickSlots[i] != 0) {
                DirtyBricks.insert(i);
            }
        }
    }
    
    auto meta = GpuMetaStorage->Map<GpuMeta>(GL_MAP_WRITE_BIT);
    std::memcpy(meta->Palette, Palette, sizeof(Palette));

    if (DirtyBricks.size() > 0) {
        uint32_t stageCount = std::min((uint32_t)DirtyBricks.size(), 512u);
        const GLenum mappingFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT;
        auto stageBuffer = ogl::Buffer(stageCount * sizeof(Brick), mappingFlags);
        Brick* stageBufferPtr = stageBuffer.Map<Brick>(mappingFlags | GL_MAP_FLUSH_EXPLICIT_BIT);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, stageBuffer.Handle);

        auto itr = DirtyBricks.begin();
        for (uint32_t i = 0; itr != DirtyBricks.end() && i < stageCount; i++) {
            uint32_t brickIdx = *itr;
            uint32_t brickSlot = BrickSlots[brickIdx];
            glm::uvec3 slotPos = GetGpuBrickSlot(brickSlot);

            glm::uvec3 brickPos = GetBrickPos(brickIdx) * BrickSize;
            OccMap.UpdateBrick(brickPos, BrickStorage[brickSlot].Data);

            stageBufferPtr[i] = BrickStorage[brickSlot];
            meta->BrickSlots[brickIdx] = slotPos.x << 0 | slotPos.y << 10 | slotPos.z << 20;

            stageBuffer.FlushMappedRange(i * sizeof(Brick), sizeof(Brick));
            GpuBrickStorage->SetPixels(GL_RED_INTEGER, GL_UNSIGNED_BYTE, (void*)(i * sizeof(Brick)), 8, 8, 
                                       0, slotPos * VoxelMap::BrickSize, glm::uvec3(8));

            DirtyBricks.erase(itr++);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        if (DirtyBricks.size() == 0) {
            for (uint32_t z = 0; z < NumVoxelsPerAxis / 2; z += 32) {
                OccMap.UpdateMips(glm::uvec3(0, 0, z), NumVoxelsPerAxis / 2);
            }

            for (int32_t i = 0; i < OccupancyMap::NumLevels; i++) {
                GpuOccupancyStorage->SetPixels(
                    GL_RED_INTEGER, GL_UNSIGNED_INT, 
                    &OccMap.Data[OccMap.MipOffsets[i]], 0, 0, i);
            }
        }
    }
    GpuMetaStorage->Unmap();
}

void OccupancyMap::UpdateBrick(glm::uvec3 basePos, const Voxel* voxels) {
    const uint32_t size = VoxelMap::BrickSize;
    static_assert(size <= 16);

    for (uint32_t y = 0; y < size; y+= 2) {
        for (uint32_t x = 0; x < size; x+= 2) {
            uint32_t mask = 0;

            for (uint32_t z = 0; z < size; z++) {
                mask |= voxels[VoxelMap::GetVoxelIndex(x + 0, y + 0, z)].IsEmpty() ? 0 : (1u << (z/2));
                mask |= voxels[VoxelMap::GetVoxelIndex(x + 1, y + 0, z)].IsEmpty() ? 0 : (1u << (z/2));
                mask |= voxels[VoxelMap::GetVoxelIndex(x + 0, y + 1, z)].IsEmpty() ? 0 : (1u << (z/2));
                mask |= voxels[VoxelMap::GetVoxelIndex(x + 1, y + 1, z)].IsEmpty() ? 0 : (1u << (z/2));
            }
            uint32_t& colMask = Data[GetColumnIndex((basePos + glm::uvec3(x, y, 0)) >> 1u, 0)];
            uint32_t colShift = (basePos.z >> 1) & 31;

            colMask = (colMask & ~(15u << colShift)) | (mask << colShift);
        }
    }
}

void OccupancyMap::UpdateMips(glm::uvec3 basePos, uint32_t planeSize) {
    for (uint32_t k = 0; k < NumLevels - 1; k++) {
        for (uint32_t y = 0; y < (planeSize >> k); y += 2) {
            for (uint32_t x = 0; x < (planeSize >> k); x += 2) {
                uint32_t mask = 0;

                // These will never be out of bounds because CubeSize is guaranteed to be a POT.
                glm::uvec3 pos = (basePos >> k) + glm::uvec3(x, y, 0);

                mask |= Data[GetColumnIndex(pos + glm::uvec3(0, 0, 0), k)];
                mask |= Data[GetColumnIndex(pos + glm::uvec3(1, 0, 0), k)];
                mask |= Data[GetColumnIndex(pos + glm::uvec3(0, 1, 0), k)];
                mask |= Data[GetColumnIndex(pos + glm::uvec3(1, 1, 0), k)];

                // Compact consecutive 2-bits: 0b11'10'01'00 -> 0b1110
                mask = _pext_u32(mask | (mask >> 1), 0x55555555u);

                // Update half column in the lower mip
                uint32_t& lowerMask = Data[GetColumnIndex(pos >> 1u, k + 1)];
                uint32_t lowerShift = (pos.z >> 1) & 16;

                lowerMask = (lowerMask & ~(65535u << lowerShift)) | (mask << lowerShift);
            }
        }
    }
}
void OccupancyMap::Update(glm::uvec3 pos, bool occupied) {
    // TODO:
    //   occupied==true: mark appropriate cells up to root
    //   occupied==false: do nothing if any sibling cell is occupied, otherwise unmark and repeat until root
}


namespace gio = glim::io;

static const uint64_t SerMagic = 0x00'00'00'01'78'6f'76'63ul;  // "cvox 0001"

void VoxelMap::Deserialize(std::string_view filename) {
    std::ifstream is(filename.data(), std::ios::binary);

    if (!is.is_open()) {
        throw std::runtime_error("File not found");
    }

    if (gio::Read<uint64_t>(is) != SerMagic) {
        throw std::runtime_error("Incompatible file");
    }

    uint32_t numBricks = gio::Read<uint32_t>(is);
    BrickStorage.resize(numBricks);

    gio::ReadCompressed(is, Palette, sizeof(Palette));
    gio::ReadCompressed(is, BrickSlots.get(), sizeof(uint32_t) * NumTotalBricks);
    gio::ReadCompressed(is, BrickStorage.data(), sizeof(Brick) * BrickStorage.size());
}
void VoxelMap::Serialize(std::string_view filename) {
    std::ofstream os(filename.data(), std::ios::binary | std::ios::trunc);

    gio::Write<uint64_t>(os, SerMagic);
    gio::Write<uint32_t>(os, BrickStorage.size());

    gio::WriteCompressed(os, Palette, sizeof(Palette));
    gio::WriteCompressed(os, BrickSlots.get(), sizeof(uint32_t) * NumTotalBricks);
    gio::WriteCompressed(os, BrickStorage.data(), sizeof(Brick) * BrickStorage.size());
}

}; // namespace cvox
