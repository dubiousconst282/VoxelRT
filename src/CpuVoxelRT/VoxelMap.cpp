#include "VoxelMap.h"

#include <Common/BinaryIO.h>
#include <sstream>

Brick* Sector::GetBrick(uint32_t index, bool create) {
    uint8_t& slot = BrickSlots[index];
    if (slot != 0) {
        return &Storage[slot - 1];
    }
    if (create) {
        slot = (uint8_t)Storage.size() + 1;
        return &Storage.emplace_back();
    }
    return nullptr;
}

void Sector::DeleteBricks(uint64_t mask) {
    uint32_t j = 0;

    for (uint32_t i = 0; i < 64; i++) {
        if (mask >> i & 1) {
            BrickSlots[i] = 0;
            continue;
        }

        if (j != i) {
            BrickSlots[i] = j;
            Storage[j] = Storage[i];
        }
        j++;
    }
    Storage.resize(j);
}

Brick* VoxelMap::GetBrick(glm::uvec3 pos, bool create, bool markAsDirty) {
    glm::uvec3 sectorPos = pos >> BrickIndexer::Shift;

    if (!SectorIndexer::CheckInBounds(sectorPos)) {
        return nullptr;
    }

    // TODO: global brick lookups aren't supposed to be glowing hot but it could be worth
    //       doing a single entry LRU cache (eg. lastBrickIdx + lackBrickPtr) to minimize hash lookups
    uint32_t sectorIdx = SectorIndexer::GetIndex(sectorPos);
    uint32_t brickIdx = BrickIndexer::GetIndex(pos);
    Sector* sector = nullptr;

    if (auto iter = Sectors.find(sectorIdx); iter != Sectors.end()) {
        sector = &iter->second;
    } else if (create) {
        sector = &Sectors[sectorIdx];
    } else {
        return nullptr;
    }

    if (markAsDirty) {
        DirtyLocs[sectorIdx] |= 1ull << brickIdx;
    }
    return sector->GetBrick(brickIdx, true);
}

bool VoxelMap::PopDirty(glm::uvec3& pos) {
    auto iter = DirtyLocs.begin();
    if (iter == DirtyLocs.end()) return false;

    uint32_t brickIdx = (uint32_t)std::countr_zero(iter->second);
    pos = SectorIndexer::GetPos(iter->first);
    pos = pos * BrickIndexer::Size + BrickIndexer::GetPos(brickIdx);

    iter->second &= ~(1ull << brickIdx);
    if (iter->second == 0) {
        DirtyLocs.erase(iter);
    }

    assert(GetBrick(pos) != nullptr);
    return true;
}

static glm::dvec3 GetSideDist(glm::dvec3 pos, glm::dvec3 dir) {
    pos = glm::fract(pos);
    return glm::mix(1.0 - pos, pos, glm::lessThan(dir, glm::dvec3(0.0)));
    // return dir < 0.0 ? pos : 1.0 - pos;
} 
double VoxelMap::RayCast(glm::dvec3 origin, glm::vec3 dir, uint32_t maxIters) {
    glm::dvec3 deltaDist = glm::abs(1.0f / dir);
    glm::dvec3 sideDist = GetSideDist(origin, dir) * deltaDist;
    glm::ivec3 currPos = glm::floor(origin);

    while (maxIters-- > 0) {
        if (!BrickIndexer::CheckInBounds(glm::uvec3(currPos) / Brick::Size)) break;
        if (!Get(currPos).IsEmpty()) {
            return glm::min(glm::min(sideDist.x, sideDist.y), sideDist.z);
        }

        if (sideDist.x < sideDist.y && sideDist.x < sideDist.z) {
            sideDist.x += deltaDist.x;
            currPos.x += dir.x < 0 ? -1 : +1;
        } else if (sideDist.y < sideDist.z) {
            sideDist.y += deltaDist.y;
            currPos.y += dir.y < 0 ? -1 : +1;
        } else {
            sideDist.z += deltaDist.z;
            currPos.z += dir.z < 0 ? -1 : +1;
        }
    }
    return -1.0;
}

bool Brick::IsEmpty() const {
    auto ptr = (uint8_t*)Data;
    auto end = ptr + sizeof(Data);

    while (ptr < end) {
#ifdef __AVX512F__
        auto a = _mm512_loadu_epi8(&ptr[0]);
        auto b = _mm512_loadu_epi8(&ptr[64]);
        if (_mm512_cmpneq_epi8_mask(_mm512_or_si512(a, b), _mm512_set1_epi8(0)) != 0) {
            return false;
        }
        ptr += 128;
#elif __AVX2__
        auto a = _mm256_loadu_si256((__m256i*)&ptr[0]);
        auto b = _mm256_loadu_si256((__m256i*)&ptr[32]);
        if (~_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_or_si256(a, b), _mm256_set1_epi8(0))) != 0) {
            return false;
        }
        ptr += 64;
#else
        uint64_t a = *(uint64_t*)&ptr[0];
        uint64_t b = *(uint64_t*)&ptr[8];
        uint64_t c = *(uint64_t*)&ptr[16];
        uint64_t d = *(uint64_t*)&ptr[24];
        if ((a | b | c | d) != 0) {
            return false;
        }
        ptr += 32;
#endif
    }
    return true;
}

namespace gio = glim::io;

// TODO: This serialization format is as horrible as iostreams. switch to/design something better
static const uint64_t SerMagic = 0x00'00'00'03'78'6f'76'63ul;  // "cvox 0003"
static const uint32_t MaxPackSize = 1024 * 1024 * 16;

void VoxelMap::Deserialize(std::string_view filename) {
    std::ifstream is(filename.data(), std::ios::binary);

    if (!is.is_open()) {
        throw std::runtime_error("File not found");
    }

    if (gio::Read<uint64_t>(is) != SerMagic) {
        throw std::runtime_error("Incompatible file");
    }
    uint32_t numSectors = gio::Read<uint32_t>(is);
    Sectors.reserve(numSectors);

    gio::ReadCompressed(is, Palette, sizeof(Palette));

    std::istringstream cst;

    for (uint32_t i = 0; i < numSectors; i++) {
        if (gio::BytesAvail(cst) == 0) {
            std::string buf(gio::Read<uint32_t>(is), '\0');
            gio::ReadCompressed(is, buf.data(), buf.size());
            cst.str(std::move(buf));
        }
        uint32_t idx = gio::Read<uint32_t>(cst);
        uint64_t mask = gio::Read<uint64_t>(cst);
        Sector& sector = Sectors[idx];

        sector.Storage.reserve((size_t)std::popcount(mask));

        for (; mask != 0; mask &= mask - 1) {
            uint32_t j = (uint32_t)std::countr_zero(mask);
            *sector.GetBrick(j, true) = gio::Read<Brick>(cst);
        }
    }
}
void VoxelMap::Serialize(std::string_view filename) {
    std::ofstream os(filename.data(), std::ios::binary | std::ios::trunc);

    gio::Write<uint64_t>(os, SerMagic);
    gio::Write<uint32_t>(os, Sectors.size());
    gio::WriteCompressed(os, Palette, sizeof(Palette));

    std::ostringstream cst;
    const auto FlushPack = [&](bool final = false) {
        if (cst.tellp() >= MaxPackSize || final) {
            auto buf = cst.view();
            gio::Write<uint32_t>(os, buf.size());
            gio::WriteCompressed(os, buf.data(), buf.size());
            cst.str("");
        }
    };

    for (auto& [idx, sector] : Sectors) {
        uint64_t mask = sector.GetAllocationMask();
        gio::Write<uint32_t>(cst, idx);
        gio::Write<uint64_t>(cst, mask);
        
        for (; mask != 0; mask &= mask - 1) {
            int32_t j = std::countr_zero(mask);
            Brick* brick = &sector.Storage[sector.BrickSlots[j]];

            gio::Write(cst, *brick);
        }
        FlushPack();
    }
    FlushPack(true);
}