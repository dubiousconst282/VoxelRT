#include <fstream>

namespace glim::io {

template<typename T>
static void Write(std::ostream& os, const T& ptr) {
    os.write((char*)&ptr, sizeof(T));
}

template<typename T>
static T Read(std::istream& is) {
    T val;

    if (is.read((char*)&val, sizeof(T)).eof()) {
        throw std::ios_base::failure("End of stream");
    }
    return val;
}

// ZSTD compressed blob, prefixed with u32 length.
void WriteCompressed(std::ostream& os, const void* ptr, size_t size);
void ReadCompressed(std::istream& is, void* ptr, size_t size);

inline std::string ReadStr(std::istream& is) {
    std::string str(Read<uint16_t>(is), '\0');

    if (is.read(str.data(), (std::streamsize)str.size()).eof()) {
        throw std::ios_base::failure("End of stream");
    }
    return str;
}
inline void WriteStr(std::ostream& os, std::string_view str) {
    if (str.size() >= UINT16_MAX) {
        throw std::ios_base::failure("String too long");
    }
    Write<uint16_t>(os, str.size());
    os.write(str.data(), (std::streamsize)str.size());
}

inline size_t BytesAvail(std::istream& is) {
    std::streampos curr = is.tellg();
    is.seekg(0, std::ios::end);
    std::streamsize end = is.tellg();
    is.seekg(curr);
    return (size_t)(end - curr);
}

};  // namespace glim::io