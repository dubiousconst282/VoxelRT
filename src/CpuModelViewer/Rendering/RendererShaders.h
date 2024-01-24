#pragma once

#include "Common.h"
#include "SSAO.h"

namespace renderer {

enum class DebugLayer { None, BaseColor, Normals, MetallicRoughness, Occlusion, EmissiveMask, Overdraw };

// Deferred PBR shader
// https://google.github.io/filament/Filament.html
// https://bruop.github.io/ibl/
struct DefaultShader {
    static const uint32_t NumCustomAttribs = 8, NumFbAttachments = 9;

    static constexpr swr::SamplerDesc SurfaceSampler = {
        .Wrap = swr::WrapMode::Repeat,
        .MagFilter = swr::FilterMode::Linear,
        .MinFilter = swr::FilterMode::Nearest,  // Downsample with nearest for better perf, quality loss is quite subtle.
        .EnableMips = true,
    };
    static constexpr swr::SamplerDesc EnvSampler = {
        .Wrap = swr::WrapMode::ClampToEdge,
        .MagFilter = swr::FilterMode::Linear,
        .MinFilter = swr::FilterMode::Linear,
        .EnableMips = true,
    };

    // Owned
    std::unique_ptr<swr::HdrTexture2D> SkyboxTex;
    std::unique_ptr<swr::HdrTexture2D> IrradianceMap, FilteredEnvMap;
    swr::Texture2D<swr::pixfmt::RG16f> BRDFEnvLut = GenerateBRDFEnvLut();

    // TAA
    uint32_t FrameNo;
    glm::vec2 Jitter, PrevJitter;
    glm::mat4 PrevProjMat;
    std::unique_ptr<swr::Framebuffer> PrevFrame;
    swr::AlignedBuffer<uint32_t> ResolvedTAABuffer;

    // Uniform: Forward
    glm::mat4 ProjMat, ModelMat;
    const swr::RgbaTexture2D* MaterialTex;  // See `glim::Material` for what's on this texture.

    // Uniform: Compose pass
    const swr::Framebuffer* ShadowBuffer;
    glm::mat4 ShadowProjMat, ViewMat;
    glm::vec3 LightPos, ViewPos;

    float IntensityIBL = 0.3f;
    float Exposure = 1.0f;
    bool EnableTAA = true;
    bool BlurSkybox = false;

    void ShadeVertices(const swr::VertexReader& data, swr::ShadedVertexPacket& vars) const {
        VFloat3 pos = data.ReadAttribs<VFloat3>(&glim::Vertex::x);
        vars.Position = TransformVector(ProjMat, { pos, 1.0f });

        vars.SetAttribs(0, data.ReadAttribs<VFloat2>(&glim::Vertex::u));

        VFloat3 norm = data.ReadAttribs<VFloat3>(&glim::Vertex::nx);
        vars.SetAttribs(2, TransformNormal(ModelMat, norm));

        VFloat3 tang = data.ReadAttribs<VFloat3>(&glim::Vertex::tx);
        vars.SetAttribs(5, TransformNormal(ModelMat, tang));
    }

    void ShadePixels(swr::Framebuffer& fb, swr::VaryingBuffer& vars) const {
        vars.ApplyPerspectiveCorrection();

        VFloat u = vars.GetSmooth(0);
        VFloat v = vars.GetSmooth(1);

        VInt baseColor = MaterialTex->Sample<SurfaceSampler>(u, v, 0);
        VFloat3 N = normalize(vars.GetSmooth<VFloat3>(2));

        VFloat metalness = 0.0f;
        VFloat roughness = 0.5f;

        if (MaterialTex->NumLayers >= 2) [[likely]] {
            VFloat4 SN = swr::pixfmt::RGBA8u::Unpack(MaterialTex->Sample<SurfaceSampler>(u, v, 1));
            VFloat3 T = vars.GetSmooth<VFloat3>(5);

            // Gram-schmidt process (produces higher-quality normal mapping on large meshes)
            // Re-orthogonalize T with respect to N
            T = normalize(T - dot(T, N) * N);
            VFloat3 B = cross(N, T);

            // Flip bitangent depending on whether UVs are mirrored - https://stackoverflow.com/a/44901073
            // TODO: maybe cheaper to compute tangents using screen derivatives all the way. (maybe too blocky?)
            VFloat det = (dFdy(u) * dFdx(v) - dFdx(u) * dFdy(v)) & -0.0f;
            B = { B.x ^ det, B.y ^ det, B.z ^ det };  // det < 0 ? -B : B

            // Reconstruct Z from 2-channel normap map
            // https://aras-p.info/texts/CompactNormalStorage.html
            // https://www.researchgate.net/publication/259000109_Real-Time_Normal_Map_DXT_Compression
            // This isn't exact due to interpolation and mipmapping, but it's quite subtle after texturing anyway.
            VFloat Sx = SN.x * 2.0f - 1.0f;
            VFloat Sy = SN.y * 2.0f - 1.0f;
            VFloat Sz = sqrt14(1.0f - (Sx * Sx + Sy * Sy));

            N.x = T.x * Sx + B.x * Sy + N.x * Sz;
            N.y = T.y * Sx + B.y * Sy + N.y * Sz;
            N.z = T.z * Sx + B.z * Sy + N.z * Sz;

            metalness = SN.z;
            roughness = SN.w;
        }
        // G-Buffer channels
        //               LSB 0  ...  31 MSB
        //   #1 [24: BaseColor] [8: Metalness]
        //   #2 [1: NormalSign] [1: HasEmissive] [10: Roughness] [20: EncNormal]
        //   #3 [8: Unused] [24: EmissiveColor]
        VInt alpha = shrl(baseColor, 24);
        vars.TileMask &= alpha >= 128;  // alpha test

        bool hasEmissive = MaterialTex->NumLayers >= 3 && any(alpha == 255);

        if (hasEmissive) [[unlikely]] {
            VInt emissiveColor = MaterialTex->Sample<SurfaceSampler>(u, v, 2);
            _mm512_mask_store_epi32(fb.GetAttachmentBuffer<uint32_t>(4, vars.TileOffset), vars.TileMask, emissiveColor);
        }

        VInt G1 = (baseColor & 0xFFFFFF) | round2i(metalness * 255.0f) << 24;
        fb.WriteTile(vars.TileOffset, vars.TileMask, G1, vars.Depth);

        VInt G2 = utils::SignedOctEncode(N, roughness) | (hasEmissive ? 2 : 0);
        _mm512_mask_store_epi32(fb.GetAttachmentBuffer<uint32_t>(0, vars.TileOffset), vars.TileMask, G2);
    }

    void Compose(swr::Framebuffer& fb, bool hasSSAO, swr::Framebuffer& prevFb) {
        glm::mat4 invProj = glm::inverse(ProjMat);
        // Bias matrix to take UVs in range [0..screen] rather than [-1..1]
        invProj = glm::translate(invProj, glm::vec3(-1.0f, -1.0f, 0.0f));
        invProj = glm::scale(invProj, glm::vec3(2.0f / fb.Width, 2.0f / fb.Height, 1.0f));

        fb.IterateTiles([&](uint32_t x, uint32_t y) {
            uint32_t tileOffset = fb.GetPixelOffset(x, y);
            VFloat tileDepth = VFloat::load(&fb.DepthBuffer[tileOffset]);
            VMask skyMask = tileDepth >= 1.0f;

            VInt tileX = (int32_t)x + swr::FragPixelOffsetsX;
            VInt tileY = (int32_t)y + swr::FragPixelOffsetsY;
            VFloat3 worldPos = VFloat3(PerspectiveDiv(TransformVector(invProj, { conv2f(tileX), conv2f(tileY), tileDepth, 1.0f })));

            VFloat3 finalColor;

            if (!all(skyMask)) {
                // Unpack G-Buffer
                VFloat4 G1 = swr::pixfmt::RGBA8u::Unpack(VInt::load(&fb.ColorBuffer[tileOffset]));
                VInt G2r = VInt::load(fb.GetAttachmentBuffer<uint32_t>(0, tileOffset));
                VMask emissiveMask = (G2r & 2) != 0;
                VFloat4 G2 = utils::SignedOctDecode(G2r);

                VFloat3 baseColor = utils::SrgbToLinear(VFloat3(G1));
                VFloat3 N = VFloat3(G2);
                VFloat metalness = G1.w;

                VFloat perceptualRoughness = max(G2.w, 0.045f);  // clamp to prevent div by zero
                VFloat roughness = perceptualRoughness * perceptualRoughness;

                // Microfacet BRDF
                VFloat3 V = normalize(ViewPos - worldPos);
                VFloat3 L = normalize(LightPos);
                VFloat3 H = normalize(V + L);

                VFloat NoV = max(dot(N, V), 1e-4f);
                VFloat NoL = max(dot(N, L), 0.0f);
                VFloat NoH = max(dot(N, H), 0.0f);
                VFloat LoH = max(dot(L, H), 0.0f);

                float reflectance = 0.5f;

                VFloat D = D_GGX(NoH, roughness);
                VFloat G = V_SmithGGXCorrelatedFast(NoV, NoL, roughness);
                VFloat3 f0 = (0.16f * reflectance * reflectance) * (1.0f - metalness) + baseColor * metalness;
                VFloat3 F = F_Schlick(LoH, f0);

                // specular
                VFloat3 Fr = D * G * F;

                // diffuse
                VFloat3 diffuseColor = (1.0f - metalness) * VFloat3(baseColor);
                VFloat3 Fd = (1.0f - F) * diffuseColor * Fd_Lambert();

                if (ShadowBuffer != nullptr && any(NoL > 0.0f)) {
                    [[clang::always_inline]] NoL *= GetShadow(worldPos, NoL);
                }
                finalColor = (Fd + Fr) * NoL;

                if (IntensityIBL > 0.0f && SkyboxTex != nullptr) {
                    VFloat3 R = reflect(0 - V, N);
                    VFloat3 irradiance = IrradianceMap->SampleCube<EnvSampler>(R);
                    VFloat3 radiance = FilteredEnvMap->SampleCube<EnvSampler>(R, perceptualRoughness * (int32_t)FilteredEnvMap->MipLevels);
                    VFloat2 dfg = BRDFEnvLut.Sample<EnvSampler>(NoV, perceptualRoughness);
                    VFloat3 specularColor = f0 * dfg.x + dfg.y;
                    VFloat3 ibl = irradiance * diffuseColor + radiance * specularColor;

                    // TODO: multi-scatter IBL

                    //if (hasSSAO) {
                    //    ibl = ibl * GetAO(fb, x, y);
                    //}
                    finalColor = finalColor + ibl * IntensityIBL;
                }

                // This is technically not PBR but if we add AO to IBL it will never be intense enough.
                // (though it's not like anything here is strictly correct anyway...)
                if (hasSSAO) {
                    finalColor = finalColor * GetAO(fb, x, y);
                }

                // Emissive color
                if (any(emissiveMask)) {
                    VInt G3r = VInt::load(fb.GetAttachmentBuffer<uint32_t>(4, tileOffset));
                    G3r = csel(emissiveMask, G3r, 0);

                    VFloat3 emissiveColor = VFloat3(swr::pixfmt::RGBA8u::Unpack(G3r));
                    finalColor = finalColor + utils::SrgbToLinear(emissiveColor);
                }
            }

            if (any(skyMask) && SkyboxTex != nullptr) {
                auto& envTex = BlurSkybox ? FilteredEnvMap : SkyboxTex;
                VFloat3 skyColor = envTex->SampleCube<EnvSampler>(worldPos - ViewPos, 1);

                finalColor.x = csel(skyMask, skyColor.x, finalColor.x);
                finalColor.y = csel(skyMask, skyColor.y, finalColor.y);
                finalColor.z = csel(skyMask, skyColor.z, finalColor.z);
            }

            finalColor = utils::Tonemap_Unreal(finalColor * Exposure);

            VInt packedColor = swr::pixfmt::RGBA8u::Pack({ finalColor, 1.0f });
            packedColor.store(&fb.ColorBuffer[tileOffset]);

            if (EnableTAA && FrameNo != 0) {
                VFloat4 prevNDC = PerspectiveDiv(TransformVector(PrevProjMat, { VFloat3(worldPos), 1.0f }));
                prevNDC.x -= Jitter.x;
                prevNDC.y -= Jitter.y;
                prevNDC = prevNDC * 0.5 + 0.5;

                VInt prevX = round2i(prevNDC.x * fb.Width);
                VInt prevY = round2i(prevNDC.y * fb.Height);

                // Save prevColor to avoid having to recompute worldDepth on the TAA pass
                VInt prevColor = PrevFrame->SampleColor(prevX, prevY, packedColor);
                prevColor.store(fb.GetAttachmentBuffer<int32_t>(0, tileOffset));
            }
        });

        // Temporal Anti-Alias
        // - https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/
        // - https://alextardif.com/TAA.html
        // - https://sugulee.wordpress.com/2021/06/21/temporal-anti-aliasingtaa-tutorial/
        // 
        // TODO: it might be possible to work in tiles rather than on the entire fb to improve cache-ability
        if (EnableTAA && FrameNo > 0) {
            fb.IterateTiles([&](uint32_t x, uint32_t y) {
                uint32_t tileOffset = fb.GetPixelOffset(x, y);
                VInt currColor = VInt::load(&fb.ColorBuffer[tileOffset]);
                VInt prevColor = VInt::load(fb.GetAttachmentBuffer<int32_t>(0, tileOffset));

                VInt minColor = (int32_t)0xFFFF'FFFF, maxColor = 0;
                VInt tileX = (int32_t)x + swr::FragPixelOffsetsX;
                VInt tileY = (int32_t)y + swr::FragPixelOffsetsY;

                // Sample a 3x3 neighborhood to create a box in color space
                for (int32_t xo = -1; xo <= 1; xo++) {
                    for (int32_t yo = -1; yo <= 1; yo++) {
                        VInt color = fb.SampleColor(tileX + xo, tileY + yo, currColor);
                        minColor = _mm512_min_epu8(minColor, color);
                        maxColor = _mm512_max_epu8(maxColor, color);
                    }
                }
                prevColor = _mm512_min_epu8(_mm512_max_epu8(prevColor, minColor), maxColor);

                //VFloat3 currColorF = VFloat3(swr::pixfmt::RGBA8u::Unpack(currColor));
                //VFloat3 prevColorF = VFloat3(swr::pixfmt::RGBA8u::Unpack(prevColor));
                //VFloat3 resolvedColor = prevColorF + (currColorF - prevColorF) * 0.1f;
                //VInt finalColor = swr::pixfmt::RGBA8u::Pack({ resolvedColor, 1.0f });

                // Fixed-point is a smidge faster
                VInt alpha = _mm512_set1_epi16(0.1f * ((1 << 15) - 1));
                VInt finalColor = lerp16((prevColor >> 0) & 0x00FF'00FF, (currColor >> 0) & 0x00FF'00FF, alpha) |
                                  lerp16((prevColor >> 8) & 0x00FF'00FF, (currColor >> 8) & 0x00FF'00FF, alpha) << 8;

                finalColor.store(&ResolvedTAABuffer[tileOffset]);
            });
            std::swap(fb.ColorBuffer, ResolvedTAABuffer);
        }
        FrameNo++;
        PrevProjMat = ProjMat;
    }
    
    void UpdateJitter(glm::mat4& projMat, swr::Framebuffer& fb) {
        if (!EnableTAA) return;
        
        PrevJitter = Jitter;
        Jitter = (Halton23[FrameNo % 8] * 2.0f - 1.0f) / glm::vec2(fb.Width, fb.Height);
        projMat[2][0] += Jitter.x;
        projMat[2][1] += Jitter.y;

        if (PrevFrame == nullptr || fb.Width != PrevFrame->Width || fb.Height != PrevFrame->Height) {
            PrevFrame = std::make_unique<swr::Framebuffer>(fb.Width, fb.Height);
            PrevFrame->DepthBuffer = nullptr; // save memory we shouldn't have allocated in the first place.
            ResolvedTAABuffer = swr::alloc_buffer<uint32_t>(fb.Width * fb.Height);
            FrameNo = 0;
        }
        std::swap(fb.ColorBuffer, PrevFrame->ColorBuffer);
    }

    void SetSkybox(swr::HdrTexture2D&& envCube) {
        IrradianceMap = std::make_unique<swr::HdrTexture2D>(GenerateIrradianceMap(envCube));
        FilteredEnvMap = std::make_unique<swr::HdrTexture2D>(GenerateRadianceMap(envCube));
        SkyboxTex = std::make_unique<swr::HdrTexture2D>(std::move(envCube));
    }

    void ComposeDebug(swr::Framebuffer& fb, DebugLayer layer) {
        fb.IterateTiles([&](uint32_t x, uint32_t y) {
            uint32_t tileOffset = fb.GetPixelOffset(x, y);
            VFloat tileDepth = VFloat::load(&fb.DepthBuffer[tileOffset]);
            VMask skyMask = tileDepth >= 1.0f;

            VFloat3 finalColor = 0.0f;

            VFloat4 G1 = swr::pixfmt::RGBA8u::Unpack(VInt::load(&fb.ColorBuffer[tileOffset]));
            VInt G2r = VInt::load(fb.GetAttachmentBuffer<uint32_t>(0, tileOffset));
            VFloat4 G2 = utils::SignedOctDecode(G2r);

            if (layer == DebugLayer::BaseColor) {
                finalColor = VFloat3(G1);
            } else if (layer == DebugLayer::Normals) {
                finalColor = VFloat3(G2) * 0.5f + 0.5f;
            } else if (layer == DebugLayer::MetallicRoughness) {
                finalColor = { G1.w, G2.w, 0.0f };
            } else if (layer == DebugLayer::Occlusion) {
                finalColor = GetAO(fb, x, y);
            } else if (layer == DebugLayer::EmissiveMask) {
                finalColor.x = csel((G2r & 2) != 0, VFloat(1.0f), 0);
            }

            uint32_t backgroundRGB = (x / 4 + y / 4) % 2 ? 0xFF'A0A0A0 : 0xFF'FFFFFF;
            VInt finalRGB = csel(skyMask, (int32_t)backgroundRGB, swr::pixfmt::RGBA8u::Pack({ finalColor, 1.0f }));

            finalRGB.store(&fb.ColorBuffer[tileOffset]);
        });
    }

private:
    VFloat GetShadow(VFloat3 worldPos, VFloat NoL) {
        // I don't even know how I got these values, but the order has some impact on the final result.
        // Filament's disk is quite noisy even with TAA.
        static const float PoissonDisk[2][16] = {
            { -0.03, 0.17, -0.09, -0.43, 0.67, 0.88, -0.85, -0.34, 0.22, -0.64, 0.29, 0.48, -0.1, -0.99, -0.25, 0.63 },
            { -0.09, 0.85, -1.01, 0.02, -0.21, 0.36, 0.4, 0.86, -0.53, -0.69, 0.17, -0.87, 0.43, -0.08, -0.51, 0.7 },
        };

        VFloat4 shadowPos = TransformVector(ShadowProjMat, { worldPos, 1.0f });
        // ShadowProj is orthographic, so W is always 1.0
        // shadowPos.w = rcp14(shadowPos.w);

        VFloat bias = max((1.0f - NoL) * 0.015f, 0.003f);
        VFloat currentDepth = shadowPos.z * shadowPos.w - bias;

        VFloat sx = shadowPos.x * shadowPos.w * 0.5f + 0.5f;
        VFloat sy = shadowPos.y * shadowPos.w * 0.5f + 0.5f;
        sx *= ShadowBuffer->Width;
        sy *= ShadowBuffer->Height;

        // aesenc costs 5c/1t, this hash probably takes around ~8-10c on TGL.
        VInt rng = _mm512_aesenc_epi128(re2i(sx) + (int32_t)FrameNo, _mm512_set1_epi32(0)) ^ 
                   _mm512_aesenc_epi128(re2i(sy), _mm512_set1_epi32(0));

        VFloat rc, rs;
        VFloat randomAngle = conv2f(shrl(rng, 8)) * (tau / (1 << 24));
        sincos(randomAngle, rc, rs);

        auto samples = _mm_set1_epi8(0);

        for (uint32_t i = 0; i < 8; i++) {
            VFloat jx = PoissonDisk[0][i], jy = PoissonDisk[1][i];
            VFloat rx = jx * rc - jy * rs + sx; // 2x fma
            VFloat ry = jx * rs + jy * rc + sy;

            VFloat occlusionDepth = ShadowBuffer->SampleDepth(round2i(rx), round2i(ry));
            samples = _mm_mask_add_epi8(samples, occlusionDepth >= currentDepth, samples, _mm_set1_epi8(1));
        }
        return conv2f(_mm512_cvtepi8_epi32(samples)) * (1.0f / 8);
    }
    static VFloat GetAO(swr::Framebuffer& fb, uint32_t x, uint32_t y) {
        uint8_t* basePtr = fb.GetAttachmentBuffer<uint8_t>(8);
        uint32_t stride = fb.Width / 2;
        uint8_t temp[4 * 4];

        for (uint32_t ty = 0; ty < 4; ty++) {
            for (uint32_t tx = 0; tx < 4; tx++) {
                temp[tx + ty * 4] = basePtr[((x + tx) / 2) + ((y + ty) / 2) * stride];
            }
        }
        return conv2f(_mm512_cvtepu8_epi32(_mm_loadu_epi8(temp))) * (1.0f / 255);
    }

    static swr::HdrTexture2D GenerateIrradianceMap(const swr::HdrTexture2D& envTex) {
        swr::HdrTexture2D tex(32, 32, 1, 6);

        for (uint32_t layer = 0; layer < 6; layer++) {
            swr::texutil::IterateTiles(tex.Width, tex.Height, [&](uint32_t x, uint32_t y, VFloat u, VFloat v) {
                VFloat3 dir = swr::texutil::UnprojectCubemap(u, v, (int32_t)layer);
                VFloat3 color = PrefilterDiffuseIrradiance(envTex, dir);
                tex.WriteTile(swr::pixfmt::R11G11B10f::Pack(color), x, y, layer);
            });
        }
        return tex;
    }
    static swr::HdrTexture2D GenerateRadianceMap(const swr::HdrTexture2D& envTex) {
        swr::HdrTexture2D tex(128, 128, 8, 6);

        for (uint32_t layer = 0; layer < 6; layer++) {
            for (uint32_t level = 0; level < tex.MipLevels; level++) {
                uint32_t w = tex.Width >> level;
                uint32_t h = tex.Height >> level;

                swr::texutil::IterateTiles(w, h, [&](uint32_t x, uint32_t y, VFloat u, VFloat v) {
                    VFloat3 dir = swr::texutil::UnprojectCubemap(u, v, (int32_t)layer);
                    VFloat3 color = PrefilterEnvMap(envTex, level / (tex.MipLevels - 1.0f), dir);
                    tex.WriteTile(swr::pixfmt::R11G11B10f::Pack(color), x, y, layer, level);
                });
            }
        }
        return tex;
    }
    static swr::Texture2D<swr::pixfmt::RG16f> GenerateBRDFEnvLut() {
        swr::Texture2D<swr::pixfmt::RG16f> tex(128, 128, 1, 1);

        swr::texutil::IterateTiles(tex.Width, tex.Height, [&](uint32_t x, uint32_t y, VFloat u, VFloat v) {
            VFloat2 f_ab = IntegrateBRDF(u, v);
            tex.WriteTile(swr::pixfmt::RG16f::Pack(f_ab), x, y);
        });
        return tex;
    }

    static VFloat3 PrefilterDiffuseIrradiance(const swr::HdrTexture2D& envTex, VFloat3 N) {
        VMask nzMask = abs(N.z) < 0.999f;
        VFloat3 up = { csel(nzMask, VFloat(0.0f), 1.0f), 0.0, csel(nzMask, VFloat(1.0f), 0.0f) };
        VFloat3 right = normalize(cross(up, N));
        up = cross(N, right);

        VFloat3 color = 0.0f;
        uint32_t sampleCount = 0;

        const float twoPi = tau, halfPi = pi * 0.5;
        float deltaPhi = twoPi / 360.0;
        float deltaTheta = halfPi / 90.0;

#ifndef NDEBUG
        deltaPhi = twoPi / 16.0;
        deltaTheta = halfPi / 4.0;
#endif

        for (float phi = 0.0; phi < twoPi; phi += deltaPhi) {
            for (float theta = 0.0; theta < halfPi; theta += deltaTheta) {
                // Spherical to World Space in two steps...
                VFloat3 tempVec = cosf(phi) * right + sinf(phi) * up;
                VFloat3 sampleVector = cosf(theta) * N + sinf(theta) * tempVec;
                VFloat3 envColor = envTex.SampleCube<EnvSampler>(sampleVector, 2);

                color = color + TonemapSample(envColor) * cosf(theta) * sinf(theta);
                sampleCount++;
            }
        }
        return TonemapSampleInv(color * (pi * 1.0f / sampleCount));
    }

    // From Karis, 2014
    static VFloat3 PrefilterEnvMap(const swr::HdrTexture2D& envTex, float roughness, VFloat3 R) {
#ifdef NDEBUG
        const uint32_t numSamples = 128;
#else
        const uint32_t numSamples = 16;
#endif
        VFloat3 N = R;
        VFloat3 V = R;

        roughness = std::max(roughness, 0.01f);

        VFloat3 prefilteredColor = 0.0f;
        VFloat totalWeight = 0.0f;

        for (uint32_t i = 0; i < numSamples; i++) {
            glm::vec2 Xi = Hammersley(i, numSamples);
            VFloat3 H = ImportanceSampleGGX(Xi, roughness, N);
            VFloat VoH = dot(V, H);
            VFloat NoH = max(VoH, 0.0f);  // Since N = V in our approximation
            // Use microfacet normal H to find L
            VFloat3 L = 2.0f * VoH * H - V;
            VFloat NoL = max(dot(N, L), 0.0f);

            if (any(NoL > 0.0f)) {
                // Based off https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch20.html
                // Typically you'd have the following:
                // float pdf = D_GGX(NoH, roughness) * NoH / (4.0 * VoH);
                // but since V = N => VoH == NoH
                VFloat pdf = D_GGX(NoH, roughness) / 4.0f + 0.001f;
                // Solid angle of current sample -- bigger for less likely samples
                VFloat omegaS = 1.0f / (numSamples * pdf);
                // Solid angle of texel
                float omegaP = 4.0f * pi / (6 * envTex.Width * envTex.Width);
                // Mip level is determined by the ratio of our sample's solid angle to a texel's solid angle
                VFloat mipLevel = max(0.5f * approx_log2(omegaS / omegaP), 0.0f);

                VFloat3 envColor = envTex.SampleCube<EnvSampler>(L, mipLevel);

                prefilteredColor = prefilteredColor + TonemapSample(envColor) * NoL;
                totalWeight += NoL;
            }
        }
        return TonemapSampleInv(prefilteredColor / totalWeight);
    }

    static VFloat2 IntegrateBRDF(VFloat NoV, VFloat roughness) {
        VFloat3 N = { 0, 0, 1 };
        VFloat3 V = { sqrt(1.0f - NoV * NoV), 0.0f, NoV };
        VFloat2 r = { 0.0f, 0.0f };
        const uint32_t sampleCount = 1024;

        for (uint32_t i = 0; i < sampleCount; i++) {
            glm::vec2 Xi = Hammersley(i, sampleCount);
            VFloat3 H = ImportanceSampleGGX(Xi, roughness, N);
            VFloat3 L = 2.0f * dot(V, H) * H - V;

            VFloat VoH = max(0.0f, dot(V, H));
            VFloat NoL = max(0.0f, L.z);
            VFloat NoH = max(0.0f, H.z);

            if (any(NoL > 0.0f)) {
                VFloat G = V_SmithGGXCorrelatedFast(NoV, NoL, roughness * roughness);
                VFloat Gv = G * 4.0f * VoH * NoL / NoH;
                Gv = csel(NoL > 0.0f, Gv, 0.0f);

                VFloat Fc = pow5(1 - VoH);
                r.x += Gv * (1 - Fc);
                r.y += Gv * Fc;
            }
        }
        return { r.x * (1.0f / sampleCount), r.y * (1.0f / sampleCount) };
    }

    // Tonemap importance samples to prevent fireflies - http://graphicrants.blogspot.com/2013/12/tone-mapping.html
    static VFloat3 TonemapSample(VFloat3 color) {
        VFloat luma = dot(color, { 0.299f, 0.587f, 0.114f });
        return color * rcp14(1.0f + luma);
    }
    static VFloat3 TonemapSampleInv(VFloat3 color) {
        VFloat luma = dot(color, { 0.299f, 0.587f, 0.114f });
        return color * rcp14(1.0f - luma);
    }

    static VFloat D_GGX(VFloat NoH, VFloat roughness) {
        VFloat a = NoH * roughness;
        VFloat k = roughness / (1.0f - NoH * NoH + a * a);
        return k * k * (1.0f / pi);
    }
    static VFloat V_SmithGGXCorrelatedFast(VFloat NoV, VFloat NoL, VFloat roughness) {
        VFloat a = 2.0f * NoL * NoV;
        VFloat b = NoL + NoV;
        return 0.5f / lerp(a, b, roughness);
    }
    static VFloat3 F_Schlick(VFloat u, VFloat3 f0) {
        VFloat f = pow5(1.0 - u);
        return f + f0 * (1.0f - f);
    }
    static VFloat Fd_Lambert() { return 1.0f / pi; }

    static VFloat pow5(VFloat x) { return (x * x) * (x * x) * x; }

    static glm::vec2 Hammersley(uint32_t i, uint32_t numSamples) {
        uint32_t bits = i;
        bits = (bits << 16) | (bits >> 16);
        bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
        bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
        bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
        bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);
        return { i / (float)numSamples, bits * (1.0 / UINT_MAX) };
    }
    static VFloat3 ImportanceSampleGGX(glm::vec2 Xi, VFloat roughness, VFloat3 N) {
        VFloat a = roughness * roughness;

        VFloat phi = 2.0 * pi * Xi.x;
        VFloat cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
        VFloat sinTheta = sqrt(1.0 - cosTheta * cosTheta);

        VFloat3 H = { cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta };

        // vec3 upVector = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
        VMask nzMask = abs(N.z) < 0.999f;
        VFloat3 upVector = { csel(nzMask, VFloat(0.0f), 1.0f), 0.0, csel(nzMask, VFloat(1.0f), 0.0f) };
        VFloat3 tangentX = normalize(cross(upVector, N));
        VFloat3 tangentY = cross(N, tangentX);
        return tangentX * H.x + tangentY * H.y + N * H.z;
    }

    // https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile
    static VFloat2 EnvBRDFApprox(VFloat NoV, VFloat roughness) {
        const VFloat4 c0 = { -1, -0.0275, -0.572, 0.022 };
        const VFloat4 c1 = { 1, 0.0425, 1.04, -0.04 };
        VFloat4 r = roughness * c0 + c1;
        VFloat a004 = min(r.x * r.x, approx_exp2(-9.28 * NoV)) * r.x + r.y;
        return { a004 * -1.04 + r.z, a004 * 1.04 + r.w };
    }

    // Temporal anti-alias sub-pixel jitter offsets - Halton(2, 3)
    static inline const glm::vec2 Halton23[16] = {
        { 0.50000, 0.33333 }, { 0.25000, 0.66667 }, { 0.75000, 0.11111 }, { 0.12500, 0.44444 },  //
        { 0.62500, 0.77778 }, { 0.37500, 0.22222 }, { 0.87500, 0.55556 }, { 0.06250, 0.88889 },  //
        { 0.56250, 0.03704 }, { 0.31250, 0.37037 }, { 0.81250, 0.70370 }, { 0.18750, 0.14815 },  //
        { 0.68750, 0.48148 }, { 0.43750, 0.81481 }, { 0.93750, 0.25926 }, { 0.03125, 0.59259 },
    };
};
struct DepthOnlyShader {
    static const uint32_t NumCustomAttribs = 0;

    glm::mat4 ProjMat;

    void ShadeVertices(const swr::VertexReader& data, swr::ShadedVertexPacket& vars) const {
        VFloat4 pos = { data.ReadAttribs<VFloat3>(&glim::Vertex::x), 1.0f };
        vars.Position = TransformVector(ProjMat, pos);
    }

    void ShadePixels(swr::Framebuffer& fb, swr::VaryingBuffer& vars) const {
        _mm512_mask_storeu_ps(&fb.DepthBuffer[vars.TileOffset], vars.TileMask, vars.Depth);
    }
};
struct OverdrawShader {
    static const uint32_t NumCustomAttribs = 0;

    glm::mat4 ProjMat;

    void ShadeVertices(const swr::VertexReader& data, swr::ShadedVertexPacket& vars) const {
        VFloat4 pos = { data.ReadAttribs<VFloat3>(&glim::Vertex::x), 1.0f };
        vars.Position = TransformVector(ProjMat, pos);
    }

    void ShadePixels(swr::Framebuffer& fb, swr::VaryingBuffer& vars) const {
        VInt color = VInt::load(&fb.ColorBuffer[vars.TileOffset]);
        color = _mm512_adds_epu8(color, _mm512_set1_epi32((int32_t)0xFF'000020));
        fb.WriteTile(vars.TileOffset, 0xFFFF, color, vars.Depth);
    }
};

}; // namespace renderer