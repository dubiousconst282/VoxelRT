#pragma once

#include <SwRast/Rasterizer.h>
#include <SwRast/Texture.h>

namespace renderer {
using namespace swr::simd;

namespace utils {

// https://chilliant.blogspot.com/2012/08/srgb-approximations-for-hlsl.html
inline VFloat3 SrgbToLinear(VFloat3 x) { return x * (x * (x * 0.305306011 + 0.682171111) + 0.012522878); }
inline VFloat3 Tonemap_Unreal(VFloat3 x) {
    // Unreal 3, Documentation: "Color Grading"
    // Adapted to be close to Tonemap_ACES, with similar range
    // Gamma 2.2 correction is baked in, don't use with sRGB conversion!
    return x / (x + 0.155) * 1.019;
}
// inline VFloat3 LinearToSrgb(VFloat3 x) {
//     VFloat3 S1 = sqrt(x);
//     VFloat3 S2 = sqrt(S1);
//     VFloat3 S3 = sqrt(S2);
//     return 0.662002687 * S1 + 0.684122060 * S2 - 0.323583601 * S3 - 0.0225411470 * x;
// }

// Encode normal using signed octahedron + arbitrary extra into 10:10:1 + 10 bits (bit at index 1 is unused)
// https://johnwhite3d.blogspot.com/2017/10/signed-octahedron-normal-encoding.html
inline VInt SignedOctEncode(VFloat3 n, VFloat w) {
    VFloat m = rcp14(abs(n.x) + abs(n.y) + abs(n.z)) * 0.5f;
    n = n * m;

    VFloat ny = n.y + 0.5;
    VFloat nx = n.x + ny;
    ny = ny - n.x;

    return round2i(nx * 1023.0f) << 22 | round2i(ny * 1023.0f) << 12 | round2i(w * 1023.0f) << 2 | shrl(re2i(n.z), 31);
}
inline VFloat4 SignedOctDecode(VInt p) {
    float scale = 1.0f / 1023.0f;
    VFloat px = conv2f((p >> 22) & 1023) * scale;
    VFloat py = conv2f((p >> 12) & 1023) * scale;
    VFloat pw = conv2f((p >> 2) & 1023) * scale;

    VFloat nx = px - py;
    VFloat ny = px + py - 1.0f;
    VFloat nz = (1.0 - abs(nx) - abs(ny)) ^ re2f(p << 31);

    return { normalize({ nx, ny, nz }), pw };

    // OutN.z = n.z * 2.0 - 1.0;  // n.z ? 1 : -1
    // OutN.z = OutN.z * (1.0 - abs(OutN.x) - abs(OutN.y));
}

}; // namespace utils
};  // namespace renderer