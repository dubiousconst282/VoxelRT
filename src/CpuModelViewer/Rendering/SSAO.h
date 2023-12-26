#pragma once

#include <random>

#include "Common.h"
#include "DepthPyramid.h"

namespace renderer {

// TODO: Implement possibly better and faster approach from "Scalable Ambient Obscurance" +/or maybe copy a few tricks from XeGTAO or
// something? https://www.shadertoy.com/view/3dK3zR
struct SSAO {
    static const uint32_t KernelSize = 16, FbAttachId = 8;

    float Radius = 1.3f, MaxRange = 0.35f;

    float Kernel[3][KernelSize];
    VInt _randSeed;

    SSAO() {
        std::mt19937 prng(123453);
        std::uniform_real_distribution<float> unid;

        // TODO: could probably make better use of samples with poisson disk instead
        for (uint32_t i = 0; i < KernelSize; i++) {
            glm::vec3 s = {
                unid(prng) * 2.0f - 1.0f,
                unid(prng) * 2.0f - 1.0f,
                unid(prng),
            };
            s = glm::normalize(s) * unid(prng);  // Re-distrubute to hemisphere

            // Cluster samples closer to origin
            float scale = (float)i / KernelSize;
            s *= glm::lerp(0.1f, 1.0f, scale * scale);

            for (int32_t j = 0; j < 3; j++) {
                Kernel[j][i] = s[j];
            }
        }

        for (uint32_t i = 0; i < 16; i++) {
            _randSeed[i] = (int32_t)prng();
        }
    }

    void Generate(swr::Framebuffer& fb, const DepthPyramid& depthMap, glm::mat4& projViewMat) {
        glm::mat4 invProj = glm::inverse(projViewMat);
        // Bias matrix so that input UVs can be in range [0..1] rather than [-1..1]
        invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
        invProj = glm::scale(invProj, glm::vec3(2.0f / fb.Width, 2.0f / fb.Height, 1.0f));

        uint32_t stride = fb.Width / 2;
        uint8_t* aoBuffer = fb.GetAttachmentBuffer<uint8_t>(FbAttachId);

        XorShiftStep(_randSeed);  // update RNG on each frame for TAA

        fb.IterateTiles(
            [&](uint32_t x, uint32_t y) {
                VInt rng = _randSeed * (int32_t)(x * 12345 + y * 9875);

                VInt iu = (int32_t)x + swr::FragPixelOffsetsX * 2;
                VInt iv = (int32_t)y + swr::FragPixelOffsetsY * 2;
                VFloat z = depthMap.SampleDepth(iu, iv, 0);

                if (!any(z < 1.0f)) return;  // skip over tiles that don't have geometry

                VFloat4 pos = PerspectiveDiv(TransformVector(invProj, { conv2f(iu), conv2f(iv), z, 1.0f }));

                // TODO: better normal reconstruction - https://atyuwen.github.io/posts/normal-reconstruction/
                // VFloat3 posDx = { dFdx(pos.x), dFdx(pos.y), dFdx(pos.z) };
                // VFloat3 posDy = { dFdy(pos.x), dFdy(pos.y), dFdy(pos.z) };
                // VFloat3 N = normalize(cross(posDx, posDy));

                // Using textured normals is better than reconstructing from blocky derivatives, particularly around edges.
                VInt G2r = VInt::gather<4>(fb.GetAttachmentBuffer<uint32_t>(0), fb.GetPixelOffset(iu, iv));
                VFloat3 N = VFloat3(utils::SignedOctDecode(G2r));

                XorShiftStep(rng);
                VFloat3 rotation =
                    normalize({ conv2f(rng & 255) * (1.0f / 127) - 1.0f, conv2f(rng >> 8 & 255) * (1.0f / 127) - 1.0f, 0.0f });

                VFloat NdotR = dot(rotation, N);
                VFloat3 T = normalize({
                    rotation.x - N.x * NdotR,
                    rotation.y - N.y * NdotR,
                    rotation.z - N.z * NdotR,
                });
                VFloat3 B = cross(N, T);

                auto occlusion = _mm_set1_epi8(0);

                for (uint32_t i = 0; i < KernelSize; i++) {
                    VFloat kx = Kernel[0][i], ky = Kernel[1][i], kz = Kernel[2][i];

                    VFloat sx = (T.x * kx + B.x * ky + N.x * kz) * Radius + pos.x;
                    VFloat sy = (T.y * kx + B.y * ky + N.y * kz) * Radius + pos.y;
                    VFloat sz = (T.z * kx + B.z * ky + N.z * kz) * Radius + pos.z;

                    VFloat4 samplePos = PerspectiveDiv(TransformVector(projViewMat, { sx, sy, sz, 1.0f }));
                    VFloat sampleDepth = LinearizeDepth(depthMap.SampleDepth(samplePos.x * 0.5f + 0.5f, samplePos.y * 0.5f + 0.5f, 0));
                    // FIXME: range check kinda breaks when the camera gets close to geom
                    //        depth linearization might be wrong (cam close ups)
                    VFloat sampleDist = abs(LinearizeDepth(z) - sampleDepth);

                    VMask rangeMask = sampleDist < MaxRange;
                    VMask occluMask = sampleDepth <= LinearizeDepth(samplePos.z) - 0.03f;
                    occlusion = _mm_sub_epi8(occlusion, _mm_movm_epi8(occluMask & rangeMask));

                    // float rangeCheck = abs(origin.z - sampleDepth) < uRadius ? 1.0 : 0.0;
                    // occlusion += (sampleDepth <= sample.z ? 1.0 : 0.0) * rangeCheck;
                }
                occlusion = _mm_slli_epi16(occlusion, 9 - std::bit_width(KernelSize));
                occlusion = _mm_sub_epi8(_mm_set1_epi8((char)255), occlusion);

                // pow(occlusion, 3)
                __m256i o1 = _mm256_cvtepu8_epi16(occlusion);
                __m256i o2 = _mm256_srli_epi16(_mm256_mullo_epi16(o1, o1), 8);
                __m256i o3 = _mm256_srli_epi16(_mm256_mullo_epi16(o1, o2), 8);
                occlusion = _mm256_cvtepi16_epi8(o3);

                for (uint32_t sy = 0; sy < 4; sy++) {
                    std::memcpy(&aoBuffer[(x / 2) + (y / 2 + sy) * stride], (uint32_t*)&occlusion + sy, 4);
                }
            },
            2);

        // ApplyBlur(fb);
    }

private:
    static void ApplyBlur(swr::Framebuffer& fb) {
        uint8_t* aoBuffer = fb.GetAttachmentBuffer<uint8_t>(FbAttachId);
        uint32_t stride = fb.Width / 2;
        // Box blur
        // No clamping of course, as long as it doesn't crash it's fine :)
        // Buffer size is actually W*H, but we use half of that - it's really okay.
        uint32_t altBufferOffset = (fb.Height / 2) * stride;

        for (uint32_t y = 0; y < fb.Height / 2; y++) {
            for (uint32_t x = 0; x < fb.Width / 2; x += 32) {
                uint8_t* src = &aoBuffer[x + y * stride];
                BlurX32(src + altBufferOffset, src, 1);
            }
        }

        for (uint32_t y = 0; y < fb.Height / 2; y++) {
            for (uint32_t x = 0; x < fb.Width / 2; x += 32) {
                uint8_t* src = &aoBuffer[x + y * stride];
                BlurX32(src, src + altBufferOffset, (int32_t)stride);
            }
        }
    }
    static void BlurX32(uint8_t* dst, uint8_t* src, int32_t lineStride) {
        const int BlurRadius = 1, BlurSamples = BlurRadius * 2 + 1;
        __m512i accum = _mm512_set1_epi16(0);

        for (int32_t so = -BlurRadius; so <= BlurRadius; so++) {
            accum = _mm512_add_epi16(accum, _mm512_cvtepu8_epi16(_mm256_loadu_epi8(&src[so * lineStride])));
        }
        __m256i c = _mm512_cvtepi16_epi8(_mm512_mulhrs_epi16(accum, _mm512_set1_epi16(32767 / BlurSamples)));
        _mm256_storeu_epi8(dst, c);
    }

    static VFloat LinearizeDepth(VFloat d) {
        // TODO: avoid hardcoding this, get from Camera&
        const float zNear = 0.01f, zFar = 1000.0f;
        return (zNear * zFar) / (zFar + d * (zNear - zFar));
    }

    void XorShiftStep(VInt& x) {
        x = x ^ x << 13;
        x = x ^ x >> 17;
        x = x ^ x << 5;
    }
};

};  // namespace renderer