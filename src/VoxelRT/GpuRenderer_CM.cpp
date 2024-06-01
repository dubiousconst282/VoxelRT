#include "Renderer.h"
#include "BrickSlotAllocator.h"

struct GpuVoxelStorage {};

GpuRenderer::GpuRenderer(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    _ctx = ctx;
    _map = std::move(map);
    _gbuffer = std::make_unique<GBuffer>(ctx);
    _gbuffer->NumDenoiserPasses = 0;
}
GpuRenderer::~GpuRenderer() {}

void GpuRenderer::RenderFrame(glim::Camera& cam, havk::Image* target, havk::CommandList& cmds) {}
void GpuRenderer::DrawSettings(glim::SettingStore& settings) {
    ImGui::SeparatorText("Renderer##GPU");

    ImGui::PushItemWidth(150);
    settings.Combo("Debug Channel", &_gbuffer->DebugChannelView);
    settings.Slider("Light Bounces", &_numLightBounces, 1, 0u, 5u);
    settings.Slider("Denoiser Passes", &_gbuffer->NumDenoiserPasses, 1, 0u, 5u);
    // settings.Checkbox("Anisotropic LODs", &_useAnisotropicLods);
    ImGui::PopItemWidth();

    ImGui::Separator();
    _frameTime.Draw("Frame Time");
}
