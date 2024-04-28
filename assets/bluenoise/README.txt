Originally from https://github.com/NVIDIAGameWorks/SpatiotemporalBlueNoiseSDK

Combined to a single image for ease of transport.


        auto result = swr::StbImage::Create(128,128*64);
        for (uint32_t i = 0; i < 64; i++) {
            std::string path = "assets/bluenoise/src/stbn_vec2_2Dx1D_128x128x64_" + std::to_string(i) + ".png";
            auto img = swr::StbImage::Load(path);

            std::memcpy(&result.Data[i*(128*128*4)], img.Data.get(), 128*128*4);
        }
        result.SavePng("assets/bluenoise/stbn_vec2_2Dx1D_128x128x64_combined.png");
        return 0;