#include <fstream>
#include <zstd.h>

#include "VoxelMap.h"

namespace cvox {

static const uint64_t SER_MAGIC = 0x63'76'6f'78'00'00'00'01ul; // "cvox 0001"

template<typename T>
static void Write(std::ostream& os, const T& ptr) {
    os.write((char*)&ptr, sizeof(T));
}

static void WriteCompressed(std::ostream& os, const void* ptr, size_t size) {
    ZSTD_CCtx* zst = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(zst, ZSTD_c_compressionLevel, ZSTD_CLEVEL_DEFAULT);
    ZSTD_CCtx_setParameter(zst, ZSTD_c_checksumFlag, 1);

    std::streampos startPos = os.tellp();
    Write<uint32_t>(os, 0);

    char buffer[4096];
    ZSTD_inBuffer inBuf = { .src = ptr, .size = size, .pos = 0 };

    while (true) {
        ZSTD_outBuffer outBuf = { .dst = buffer, .size = sizeof(buffer), .pos = 0 };
        size_t remaining = ZSTD_compressStream2(zst, &outBuf, &inBuf, ZSTD_e_end);

        os.write(buffer, (std::streamsize)outBuf.pos);
        if (remaining == 0) break;
    }
    ZSTD_freeCCtx(zst);

    std::streampos endPos = os.tellp();
    os.seekp(startPos);
    Write<uint32_t>(os, endPos - startPos - 4);
    os.seekp(endPos);
}

template<typename T>
static T Read(std::istream& is) {
    T val;
    
    if (is.read((char*)&val, sizeof(T)).eof()) {
        throw std::ios_base::failure("End of stream");
    }
    return val;
}

static void ReadCompressed(std::istream& is, void* ptr, size_t size) {
    ZSTD_DCtx* zst = ZSTD_createDCtx();
    ZSTD_outBuffer outBuf = { .dst = ptr, .size = size, .pos = 0 };

    char buffer[4096];
    ZSTD_inBuffer inputBuf = { .src = buffer, .size = 0, .pos = 0 };
    uint32_t inputAvail = Read<uint32_t>(is);

    while (outBuf.pos < outBuf.size) {
        if (inputAvail > 0 && inputBuf.pos == inputBuf.size) {
            inputBuf.pos = 0;
            inputBuf.size = std::min(inputAvail, (uint32_t)sizeof(buffer));

            if (is.read(buffer, (std::streamsize)inputBuf.size).eof()) {
                throw std::ios_base::failure("End of stream");
            }
            inputAvail -= inputBuf.size;
        }
        size_t ret = ZSTD_decompressStream(zst, &outBuf, &inputBuf);

        if (ZSTD_isError(ret)) {
            throw std::ios_base::failure("Failed to decompress stream");
        }
    }

    assert(inputAvail == 0);
    ZSTD_freeDCtx(zst);
}

void VoxelMap::Deserialize(std::string_view filename) {
    std::ifstream is(filename.data(), std::ios::binary);

    if (!is.is_open()) {
        throw std::runtime_error("File not found");
    }

    if (Read<uint64_t>(is) != SER_MAGIC) {
        throw std::runtime_error("Incompatible file");
    }

    uint32_t numBricks = Read<uint32_t>(is);
    BrickStorage.resize(numBricks);

    ReadCompressed(is, Palette, sizeof(Palette));
    ReadCompressed(is, BrickSlots.get(), sizeof(uint32_t) * NumTotalBricks);
    ReadCompressed(is, BrickStorage.data(), sizeof(Brick) * BrickStorage.size());
}
void VoxelMap::Serialize(std::string_view filename) {
    std::ofstream os(filename.data(), std::ios::binary | std::ios::trunc);

    Write<uint64_t>(os, SER_MAGIC);
    Write<uint32_t>(os, BrickStorage.size());

    WriteCompressed(os, Palette, sizeof(Palette));
    WriteCompressed(os, BrickSlots.get(), sizeof(uint32_t) * NumTotalBricks);
    WriteCompressed(os, BrickStorage.data(), sizeof(Brick) * BrickStorage.size());
}

}; // namespace cvox
