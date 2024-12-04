#include "Renderer.h"
#include <magic_enum.hpp>

namespace {

struct PerfSample {
    double FrameTimeMs;
    int64_t TotalRayCasts;
    double AvgItersPerRay;
    double AvgClocksPerIter;

    PerfSample() = default;
    PerfSample(FramePerfStats& stats) {
        FrameTimeMs = (stats.Counters[FramePerfStats::Frame_EndTS] - stats.Counters[FramePerfStats::Frame_StartTS]) / 1000000.0;
        TotalRayCasts = stats.Counters[FramePerfStats::RayCasts];
        AvgItersPerRay = stats.Counters[FramePerfStats::TraversalIters] / (double)TotalRayCasts;
        AvgClocksPerIter = stats.Counters[FramePerfStats::ClocksPerRay] / (double)(stats.Counters[FramePerfStats::TraversalIters] + TotalRayCasts);
    }
};
struct ScenePreset {
    glm::vec3 CamPos;
    glm::vec2 CamRot;
    uint32_t MapSize;
    std::string_view Path;
    std::string_view Label;
};

enum class RunnerState {
    Idle,
    SetupNext,
    Sync,
    Profile,
};

struct RendererBenchmark : public Renderer {
    static constexpr int kNumSamplesPerTarget = 128;
    static constexpr RendererId kTargetIds[] = {
        RendererId::PlainDDA, RendererId::MultiDDA, RendererId::XBrickMap,   RendererId::ESVO,
        RendererId::Tree64,   RendererId::StdBVH,   RendererId::ManhattanDF, RendererId::EuclideanDF,
        // RendererId::OctantDF,
    };
    static constexpr ScenePreset kScenePresets[] = {
        { { 340,  160, 1024 }, {  1.57,  0.00 }, 2048, "logs/voxels_2k_sponza.dat", "Sponza 2k" },
        { { 1600, 400, 1500 }, { -1.10,  0.05 }, 2048, "logs/voxels_2k_ecohouse.dat", "Eco House 2k" },
        { { 1165, 250, 2020 }, {  1.2,   0.05 }, 4096, "logs/voxels_4k_bistro.dat", "Bistro 4k" },
        { { 1780, 450, 2020 }, {  1.85, -0.50 }, 4096, "logs/voxels_4k_san_miguel.dat", "San Miguel 4k" },
        { { 1130, 450, 2080 }, { -0.85, -0.20 }, 4096, "logs/voxels_4k_forestlake.dat", "Forest Lake 2k" },
        { { 4000, 700, 1000 }, { -2.00, -0.5  }, 4096, "logs/voxels_4k_highway_i95.dat", "NY Highway I95 4k" },
    };

    RendererBenchmark(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) : Renderer(ctx, map) { }

    void RenderFrame(havx::Camera& cam, GBuffer* gbuffer, havk::CommandList& cmds) override {
        ImGui::Begin("Benchmark Runner", nullptr, ImGuiWindowFlags_NoCollapse);

        if (ImGui::Button(_state == RunnerState::Idle ? "Run" : "Cancel")) {
            ChangeState(_state == RunnerState::Idle ? RunnerState::SetupNext : RunnerState::Idle);
            _presetIdx = 0;
            _targetIdx = 0;
        }

        if (_state == RunnerState::Idle && !_resultsMarkdown.empty()) {
            ImGui::InputTextMultiline("Markdown", _resultsMarkdown.data(), _resultsMarkdown.size(),
                                      ImGui::GetContentRegionAvail(), ImGuiInputTextFlags_ReadOnly);
        }

        // Switch renderer
        if (_state == RunnerState::SetupNext && _targetIdx >= _accumData.size()) {
            _resultsMarkdown += GenerateResultsMarkdown();
            printf("---- BENCHMARK RESULTS ----\n%s\n\n", _resultsMarkdown.data());

            if (_presetIdx < std::size(kScenePresets)) {
                auto& preset = kScenePresets[_presetIdx];
                cam.ViewPosition = cam.Position = preset.CamPos;
                cam.Euler = preset.CamRot;

                _map->Sectors.clear();
                _map->Deserialize(preset.Path);

                _accumData.clear();
                for (RendererId id : kTargetIds) {
                    //if (preset.MapSize > 2048 && (id == RendererId::ManhattanDF || id == RendererId::EuclideanDF)) continue;

                    _accumData.push_back({ .TargetId = id });
                }

                int32_t numBricks = 0;
                for (auto& sector : _map->Sectors) {
                    numBricks += std::popcount(sector.second.GetAllocationMask());
                }
                _resultsMarkdown += "\n\n--------\n\n**Scene**: " + std::string(preset.Label) + " (";
                _resultsMarkdown += std::to_string(numBricks / 1000) + "k * 8³ voxels)\n";

                _presetIdx++;
                _targetIdx = 0;
            } else {
                ChangeState(RunnerState::Idle);
            }
        }
        if (_state == RunnerState::SetupNext) {
            ChangeState(RunnerState::Sync);
            _currentRenderer = Renderer::Create(_ctx, _map, _accumData[_targetIdx].TargetId);
        }

        if (_currentRenderer != nullptr) {
            TargetData& data = _accumData[_targetIdx];
            auto renderer = dynamic_cast<GpuRenderer*>(_currentRenderer.get());

            if (_state == RunnerState::Sync) {
                auto syncStart = std::chrono::steady_clock::now();
                renderer->TimeQueryPool->WriteTimestamp(cmds, 2, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

                if (!_currentRenderer->SyncMap(cam, cmds)) {
                    ChangeState(RunnerState::Profile);
                }

                renderer->TimeQueryPool->WriteTimestamp(cmds, 3, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                auto syncEnd = std::chrono::steady_clock::now();

                data.CpuSyncMs += (syncEnd - syncStart).count() / 1000000.0;
            }

            _currentRenderer->RenderFrame(cam, gbuffer, cmds);

            FramePerfStats& stats = renderer->LastFrameStats;
            data.GpuSyncMs += (stats.Counters[FramePerfStats::Sync_EndTS] - stats.Counters[FramePerfStats::Sync_StartTS]) / 1000000.0;

            if (_state == RunnerState::Profile) {
                if (gbuffer->NumLightBounces == 0) {
                    data.Samples.push_back(PerfSample(renderer->LastFrameStats));
                } else {
                    data.SamplesSecBounce.push_back(PerfSample(renderer->LastFrameStats));
                }
 
                gbuffer->NumLightBounces = _stateCounter > kNumSamplesPerTarget ? 1 : 0;

                if (_stateCounter > kNumSamplesPerTarget * 2) {
                    ChangeState(RunnerState::SetupNext);
                    _targetIdx++;
                }
                _stateCounter++;
            }
        }

        ImGui::Text("State: %s", magic_enum::enum_name(_state).data());

        if (_state != RunnerState::Idle) {
            ImGui::Text("Current: %s (%d/%zu, scene %d/%zu, frame %d/%d)",
                        _targetIdx < _accumData.size() ? magic_enum::enum_name(_accumData[_targetIdx].TargetId).data() : "none",
                        _targetIdx, _accumData.size(),
                        _presetIdx, std::size(kScenePresets),
                        _stateCounter, kNumSamplesPerTarget);
        }
        ImGui::End();
    }

private:
    uint32_t _presetIdx = 0, _targetIdx = 0, _stateCounter = 0;
    RunnerState _state = RunnerState::Idle;

    void ChangeState(RunnerState newState) {
        if (_state == RunnerState::SetupNext || _state == RunnerState::Idle) {
            _currentRenderer = nullptr;
        }
        _state = newState;
        _stateCounter = 0;
    }

    struct TargetData {
        RendererId TargetId;
        std::vector<PerfSample> Samples;
        std::vector<PerfSample> SamplesSecBounce;
        double GpuSyncMs = 0;
        double CpuSyncMs = 0;
    };
    std::vector<TargetData> _accumData;
    std::unique_ptr<Renderer> _currentRenderer;
    std::string _resultsMarkdown;

    std::string GenerateResultsMarkdown() {
        std::string str = "|";

        uint32_t columnSize = 12;
        uint32_t numColumns = _accumData.size() + 1;

        const auto PrintColumn = [&](const char* fmt, auto... args) {
            size_t cursor = str.size();
            str.resize(cursor + 1024);
            uint32_t colSize = uint32_t(snprintf(&str[cursor], 1024, fmt, args...));
            str.resize(cursor + colSize);

            if (colSize < columnSize) {
                str.append(columnSize - colSize, ' ');
            }
            str += "|";
        };


        PrintColumn("%s", "Method");
        PrintColumn("%s", "Mrays/s");
        PrintColumn("%s", "Mrays/s PT1");
        PrintColumn("%s", "Iters/ray");
        PrintColumn("%s", "Clocks/iter");
        PrintColumn("%s", "GPU sync");
        PrintColumn("%s", "CPU sync");
        str += "\n|------------|------------|------------|------------|------------|------------|------------";

        for (auto& data : _accumData) {
            str += "\n|";
            // TODO: taking min instead of median by frame time might make more sense
            auto& samples = data.Samples;
            std::sort(samples.begin(), samples.end(), [](PerfSample& a, PerfSample& b) { return a.FrameTimeMs < b.FrameTimeMs; });
            auto& s = samples[samples.size() / 2];

            auto& pathSamples = data.SamplesSecBounce;
            std::sort(pathSamples.begin(), pathSamples.end(), [](PerfSample& a, PerfSample& b) { return a.FrameTimeMs < b.FrameTimeMs; });
            auto& spath = pathSamples[pathSamples.size() / 2];

            PrintColumn("%s", magic_enum::enum_name(data.TargetId).data());
            PrintColumn("%.1f", s.TotalRayCasts * (1000.0 / s.FrameTimeMs) / 1000000.0);

            double val1 = s.TotalRayCasts * (1000.0 / s.FrameTimeMs) / 1000000.0;
            double val2 = spath.TotalRayCasts * (1000.0 / spath.FrameTimeMs) / 1000000.0;

            PrintColumn("%.1f (%.2fx)", val2, val2 / val1);

            PrintColumn("%.1f", s.AvgItersPerRay);
            PrintColumn("%.1f", s.AvgClocksPerIter);
            PrintColumn("%.1f ms", std::max(0.0, data.GpuSyncMs));
            PrintColumn("%.1f ms", data.CpuSyncMs);
        }

        return str;
    }
};

};  // namespace

template<>
std::unique_ptr<Renderer> Renderer::Create<RendererId::Benchmark>(havk::DeviceContext* ctx, std::shared_ptr<VoxelMap> map) {
    return std::make_unique<RendererBenchmark>(ctx, map);
}
