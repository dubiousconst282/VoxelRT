#pragma once

#include <cstdint>

namespace swr {

struct ProfilerStats {
    enum Key {
        TrianglesDrawn,
        TrianglesClipped,
        VerticesShaded,
        BinsFilled,

        SetupTime,
        RasterizeTime,
        ComposeTime,

        ShadowTime,
        FrameTime,

        _Count,
        _TimeFirst = SetupTime,
    };
    struct Entry {
        uint64_t Value, Avg;
    };

    Entry Keys[_Count];

    void Reset() {
        for (uint32_t i = 0; i < _Count; i++) {
            Entry& e = Keys[i];

            const uint64_t a = 95;
            e.Avg = (e.Avg * a + e.Value * (100 - a)) / 100;
            e.Value = 0;
        }
    }

    static uint64_t CurrentTime();
};
extern ProfilerStats g_Stats;

#define STAT_INCREMENT(key, amount) (swr::g_Stats.Keys[swr::ProfilerStats::Key::key].Value += amount)
#define STAT_TIME_BEGIN(key) uint64_t _stt_##key = swr::ProfilerStats::CurrentTime()
#define STAT_TIME_END(key) (swr::g_Stats.Keys[swr::ProfilerStats::Key::key##Time].Value += swr::ProfilerStats::CurrentTime() - _stt_##key)

#define STAT_GET_TIME(key) (swr::g_Stats.Keys[swr::ProfilerStats::Key::key##Time].Avg / 1000000.0)
#define STAT_GET_COUNT(key) (swr::g_Stats.Keys[swr::ProfilerStats::Key::key].Value / 1000.0)

}; //namespace swr