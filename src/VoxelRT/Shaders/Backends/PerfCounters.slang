public enum PerfKey {
    RayCasts,
    TraversalIters,
    ClocksPerRay,

    ReservedStart_ = 10,
    Count_ = 16,
};
public struct PerfStats {
    public uint64_t Counters[int(PerfKey.Count_)];
};

public static PerfStats* g_PerfTracker = nullptr;

public static int g_CurrTraversalIters = 0;