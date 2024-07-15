#include "VoxelMap.h"

static uint8_t SampleLOD(const Brick* brick, glm::uvec3 pos, uint32_t step) {
    switch (step) {
        default:
            assert(!"Unsupported LOD scale");
            return 0;
    }
}

void Brick::GenerateLOD(Voxel* dest, uint32_t level) const {
    if (level == 0) {
        memcpy(dest, Data, sizeof(Data));
        return;
    }
    assert(level > 0 && level < 4);

    uint32_t step = 1 << level;

    for (uint32_t y = 0; y < Brick::Size.y; y += step) {
        for (uint32_t z = 0; z < Brick::Size.z; z += step) {
            for (uint32_t x = 0; x < Brick::Size.x; x += step) {
                *dest++ = Voxel(SampleLOD(this, glm::uvec3(x, y, z), step));
            }
        }
    }
}