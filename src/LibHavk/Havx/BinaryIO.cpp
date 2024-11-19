#include "BinaryIO.h"

#include <zstd.h>

#include <cassert>
#include <filesystem>

namespace havx::io {

std::vector<uint8_t> ReadFile(std::string_view path) {
    std::ifstream fs;
    fs.exceptions(std::ios::failbit);
    fs.open(path.data(), std::ios::binary | std::ios::ate);

    std::vector<uint8_t> buffer((size_t)fs.tellg());

    fs.seekg(0);
    fs.read((char*)buffer.data(), (std::streamsize)buffer.size());

    return buffer;
}
void WriteFile(std::string_view path, const void* data, size_t length) {
    auto fs_path = std::filesystem::path(path);
    if (fs_path.has_parent_path()) {
        std::filesystem::create_directories(fs_path.parent_path());
    }
    
    std::ofstream fs;
    fs.exceptions(std::ios::failbit);
    fs.open(fs_path, std::ios::binary | std::ios::trunc);

    fs.write((char*)data, (std::streamsize)length);
}

void WriteCompressed(std::ostream& os, const void* ptr, size_t size) {
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

void ReadCompressed(std::istream& is, void* ptr, size_t size) {
    ZSTD_DCtx* zst = ZSTD_createDCtx();
    ZSTD_outBuffer outBuf = { .dst = ptr, .size = size, .pos = 0 };

    char buffer[4096];
    ZSTD_inBuffer inputBuf = { .src = buffer, .size = 0, .pos = 0 };
    uint32_t inputAvail = Read<uint32_t>(is);

    while (inputAvail > 0) {
        if (inputBuf.pos == inputBuf.size) {
            inputBuf.pos = 0;
            inputBuf.size = std::min(inputAvail, (uint32_t)sizeof(buffer));

            if (is.read(buffer, (std::streamsize)inputBuf.size).eof()) {
                ZSTD_freeDCtx(zst);
                throw std::ios_base::failure("End of stream");
            }
            inputAvail -= inputBuf.size;
        }
        size_t ret = ZSTD_decompressStream(zst, &outBuf, &inputBuf);

        if (ZSTD_isError(ret)) {
            ZSTD_freeDCtx(zst);
            throw std::ios_base::failure("Failed to decompress stream");
        }
    }
    ZSTD_freeDCtx(zst);

    if (outBuf.pos != outBuf.size) {
        throw std::ios_base::failure("Decompressed stream is too short");
    }
}

};  // namespace havx::io