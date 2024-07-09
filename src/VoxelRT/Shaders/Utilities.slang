public struct Random {
    uint _state;

    public __init(uint3 p) {
        uint2 q = 1103515245U * p.xy;
        uint n = q.x ^ (q.y >> 3U);
        _state = n + p.z * 12345u;
    }

    // https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/
    [__ref]
    public uint NextU32() {
        uint state = _state;
        _state = _state * 747796405u + 2891336453u;
        uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        return (word >> 22u) ^ word;
    }
    [__ref]
    public float NextFloat() {
        return float(NextU32()) * (1.0 / 4294967296.0);
    }
};

public struct BlueNoise {
    Texture2D<uint2> stbnTexture;
    int3 tilePos;
    uint sampleIdx;

    public __init(Texture2D<uint2> stbn, int2 samplePos, uint frameNo, uint sampleIdx_ = 0) {
        stbnTexture = stbn;
        tilePos = int3(samplePos, frameNo);
        sampleIdx = sampleIdx_ + (frameNo & ~63); // offset by frameNo to hide noise period
    }

    // Returns 2 blue noise samples at the given position and internal increasing sample index.
    // https://developer.nvidia.com/blog/rendering-in-real-time-with-spatiotemporal-blue-noise-textures-part-1/
    [__ref]
    public float2 GetSample() {
        float2 sampleOffset = MartinR2(sampleIdx++);
        int2 pos = (tilePos.xy + int2(sampleOffset * 128)) & 127;
        pos.y += (tilePos.z & 63) * 128;

        return (stbnTexture[pos] + 0.5) / 256.0;
    }
};

// R2 quasirandom sequence
public float2 MartinR2(uint index) {
    return fract(index * float2(0.75487766624669276005, 0.56984029099805326591) + 0.5);
}

public float3 GetSphereDir(float2 sample) {
    float a = sample.x * 6.283185307179586;
    float y = sample.y * 2 - 1;

    float sy = sqrt(1.0 - y * y);
    return float3(sin(a) * sy, y, cos(a) * sy);
}