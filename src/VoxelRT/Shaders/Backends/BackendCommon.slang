public interface IRayCastable {
    public HitInfo RayCast(int3 worldOrigin, float3 origin, float3 dir, bool coarse);
}

public struct HitInfo {
    public float Dist;
    public float3 Pos;
    public float3 Normal;
    public float2 FaceUV;
    public uint MaterialId;

    public property bool Miss {
        get { return MaterialId == 0; }
    }
}

public static const int kDefaultOpaqueMaterialId = 254;

public struct Material {
    public uint2 Data;

    public property float3 Color {
        get {
            uint3 mask = uint3(31, 63, 31);
            float3 color = float3(Data.xxx >> uint3(11, 5, 0) & mask) / float3(mask);
            return color * color;  // srgb gamma hack
        }
    }
    public property float Emission {
        get {
            return f16tof32(Data.x >> 16);
        }
    }
    public property float MetalFuzziness {
        get {
            return (Data.y & 255) / 255.0;
        }
    }
}

public float3 ClipRayToAABB(float3 origin, float3 dir, float3 bbMin, float3 bbMax) {
    float3 invDir = 1.0 / dir;
    float3 t1 = (bbMin - origin) * invDir;
    float3 t2 = (bbMax - origin) * invDir;
    float3 temp = t1;
    t1 = min(temp, t2);
    t2 = max(temp, t2);

    float tmin = max(t1.x, max(t1.y, t1.z));
    float tmax = min(t2.x, min(t2.y, t2.z));

    return tmin > 0 && tmin < tmax ? origin + dir * tmin : origin;
}

// Returns linear index for 3D grid in XZY order. Sizes **must** be power of two.
public uint GetLinearIndex(uint3 pos, uint sizeXZ, uint sizeY) {
    return (pos.x & (sizeXZ - 1)) +
           (pos.z & (sizeXZ - 1)) * sizeXZ +
           (pos.y & (sizeY - 1)) * (sizeXZ * sizeXZ);
}
// Returns linear index for 3D grid in XZY order. Sizes **must** be constant otherwise this will generate divisions.
public uint3 GetPosFromLinearIndex(uint idx, uint sizeXZ, uint sizeY) {
    uint3 pos;
    pos.x = idx % sizeXZ;
    pos.z = (idx / sizeXZ) % sizeXZ;
    pos.y = (idx / sizeXZ) / sizeXZ;
    return pos;
}

public bool CheckBounds(uint3 pos, uint sizeXZ, uint sizeY) {
    return (pos.x | pos.z) < sizeXZ && pos.y < sizeY;
}


// Count number of set bits in variable range [0..width]
public uint popcnt_var64(uint64_t mask, uint width) {
    // return popcnt64(mask & ((1ull << mask) - 1));
    uint himask = uint(mask);
    uint count = 0;

    if (width >= 32) {
        count = countbits(himask);
        himask = uint(mask >> 32);
    }
    uint m = 1u << (width & 31u);
    count += countbits(himask & (m - 1u));

    return count;
}
public uint popcnt64(uint64_t mask) {
    uint a = countbits(uint(mask >> 0));
    uint b = countbits(uint(mask >> 32));
    return a + b;
}
public uint tzcnt64(uint64_t mask) {
    uint a = uint(mask >> 0);
    uint b = uint(mask >> 32);
    uint c = firstbitlow(a != 0 ? a : b);
    return a != 0 ? c : c + 32;
}

public void InterlockedSetBit(__ref uint64_t dest, uint index, bool value) {
    // Workaround for 64-bit atomics which are slightly fucked on slang's spirv target atm
    uint mask = 1 << (index & 31);
    uint* ptr = (uint*)&dest + (index / 32u);

    if (value) {
        InterlockedOr(*ptr, mask);
    } else {
        InterlockedAnd(*ptr, ~mask);
    }
}
