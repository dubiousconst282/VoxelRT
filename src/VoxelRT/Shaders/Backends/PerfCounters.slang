public enum PerfKey {
    RayCasts,
    TraversalIters,
    ClocksPerRay,

    ReservedStart_ = 12,
    Count_ = 16,
};
public struct PerfStats {
    public uint64_t Counters[16]; // enum value not constexpr? int(PerfKey.Count_)
    public uint32_t RayCastItersHistogram[32];
};

public static PerfStats* g_PerfTracker = (PerfStats*)0;

