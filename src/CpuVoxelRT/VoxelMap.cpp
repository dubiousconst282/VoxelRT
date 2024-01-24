#include "VoxelMap.h"
#include <Common/BinaryIO.h>

namespace cvox {

static glm::uvec3 GetGpuBrickSlot(uint32_t slotId) {
    const uint32_t mask = 0x49249249; // 0b001 on all consecutive bits

    // Morton de-interleaving. This is quite arbitrary but allows
    // the texture size to be cubed in respect with number of bricks.
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

            stageBufferPtr[i] = BrickStorage[brickSlot];
            meta->BrickSlots[brickIdx] = slotPos.x << 0 | slotPos.y << 10 | slotPos.z << 20;

            stageBuffer.FlushMappedRange(i * sizeof(Brick), sizeof(Brick));
            GpuBrickStorage->SetPixels(GL_RED_INTEGER, GL_UNSIGNED_BYTE, (void*)(i * sizeof(Brick)), 8, 8, 
                                       slotPos * VoxelMap::BrickSize, glm::uvec3(8));

            DirtyBricks.erase(itr++);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    GpuMetaStorage->Unmap();
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
