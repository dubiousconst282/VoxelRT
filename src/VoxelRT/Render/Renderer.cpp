#include "Renderer.h"

void GpuRenderer::DrawPerfCounters() {
    double elapsedMs = _renderQueryTs->GetElapsedMillis(_stats.Counters[PerfStats::Frame_StartQTS],
                                                        _stats.Counters[PerfStats::Frame_EndQTS]);
    double raysPerSec = _stats.Counters[PerfStats::RayCasts] * (1000.0 / elapsedMs);
    ImGui::Text("Render time: %.2fms (%.2fmrays/s)", elapsedMs, raysPerSec / 1000000.0);

    double avgItersPerRay = _stats.Counters[PerfStats::TraversalIters] / (double)_stats.Counters[PerfStats::RayCasts];
    double avgClocksPerIter = _stats.Counters[PerfStats::ClocksPerRay] / (double)(_stats.Counters[PerfStats::TraversalIters] + _stats.Counters[PerfStats::RayCasts]);
    ImGui::Text("Traversal: %.2f iters/ray, %.2f clocks/iter", avgItersPerRay, avgClocksPerIter);

    std::vector<float> iterBins;
    bool hasHistogramData = false;
    for (uint32_t bin : _stats.RayCastItersHistogram) {
        iterBins.push_back(bin / 1000.0);
        hasHistogramData |= bin > 0;
    }

    if (hasHistogramData) {
        ImGui::PlotHistogram("##TraversalItersHistogram", iterBins.data(), iterBins.size(), 0, nullptr, FLT_MAX, FLT_MAX, ImVec2(0, 80));
    }
}