import "./PerfCounters";

#define PERF_LEVEL_OFF 0
#define PERF_LEVEL_MINIMAL 1
#define PERF_LEVEL_NORMAL 2
#define PERF_LEVEL_HIGH 3

#define PERF_TRACK_LEVEL  PERF_LEVEL_NORMAL

#define PERF_PRECISE_ITER_HEATMAP  1

#if PERF_TRACK_LEVEL > PERF_LEVEL_OFF
    #define PERF_STAT_INC(key, amount) \
        if (g_PerfTracker) InterlockedAdd(g_PerfTracker->Counters[int(PerfKey.key)], amount); \
        if (PERF_PRECISE_ITER_HEATMAP != 0 && PerfKey.key == PerfKey.TraversalIters) g_CurrTraversalIters += int(amount);
#else
    #define PERF_STAT_INC(key, amount)
#endif

#if PERF_TRACK_LEVEL >= PERF_LEVEL_NORMAL
    #define PERF_TIME_BEGIN(key) let key##__timeStart = clockARB()
    #define PERF_TIME_END(key)  if (WaveIsFirstLane()) { PERF_STAT_INC(key, (clockARB() - key##__timeStart)); }
#else
    #define PERF_TIME_BEGIN(key)
    #define PERF_TIME_END(key)
#endif