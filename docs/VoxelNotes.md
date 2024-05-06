# Voxel ray tracing notes

**Key points:**
- Parametric voxel traversal algorithm (as per [ESVO](#sparse-voxel-octrees-svos-and-dags), [ADT07], and others)
  - Easy to accelerate with space skipping
  - Faster than standard incremental DDA
  - Needs workarounds to deal with limited float precision
- 64-bit occupancy bitmasks within 4x4x4 tiles
  - Useful for sparse storage allocation and space skipping LODs
  - Provides up to 3 LODs with a single 64-bit access

## Introductory fluff
A natural approach for rendering voxels on a GPU is by rasterizing meshes that represent the voxel geometry. It's a proven method that works reasonably well and is very flexible, but can be relatively complicated to scale up - from my outsider view, this would probably involve stuff like minimizing draw calls, compressing vertices, multi-threading the mesher because it's slow af, occlusion culling, and probably getting cursed by greedy meshing at some point...

Ray tracing isn't without its problems and difficulties of course, but arguably a lot simpler to get working - simply mem-copy voxel data as is to a GPU buffer and start casting rays through it in a shader. Even without any acceleration structures, it can be surprisingly fast, as demonstrated by these remarkable shadertoys:

<table align="center">
    <tr align="center">
        <td width="33%">
            <a href="https://www.shadertoy.com/view/4dX3zl"> <img src="VoxelNotes/stdemo_branchless_dda.png"></a> <br/>
            <p>Branchless Voxel Raycasting</p>
        </td>
        <td width="33%">
            <a href="https://www.shadertoy.com/view/MtcGDH"> <img src="VoxelNotes/stdemo_sh16c_voxel_game.png"> </a> <br/>
            <p>[SH16C] Voxel Game</p>
        </td>
        <td width="33%">
            <a href="https://www.shadertoy.com/view/wsByWV"> <img src="VoxelNotes/stdemo_voxel_game_evolution.png">  </a> <br/>
            <p>Voxel game Evolution</p>
        </td>
    </tr>
</table>

## Ray Casting Voxels
The classic way to cast rays through voxels is by using the [Fast Voxel Traversal](https://github.com/cgyurgyik/fast-voxel-traversal-algorithm/blob/master/overview/FastVoxelTraversalOverview.md) algorithm by Amanatides and Woo, which boils down to a simple DDA loop that incrementally traverses through all intersecting voxels by comparing plane intersection distances and stepping through whichever side has progressed least at each iteration:

<div align="center">
  <img src="VoxelNotes/dda_anim.gif" width="33%">
  <p><i>2D DDA animation. <b>dx</b> and <b>dy</b> are the "side distances" or "tMax".</i></p>
</div>

---

Acceleration techniques based on space skipping relies on the ray caster being able to step by variable amounts, which is not trivial to implement in the incremental DDA efficiently - most approaches I have seen are hybrids that look somewhat susceptible to thread divergence, like switching between ray marching and DDA, or nesting multi-level DDAs (which actually seems to be used by Teardown, so it's probably not that bad?).

On modern (decades old) hardware, floating-point multiply-adds are equally as cheap as individual adds or multiplies, so calculating the side distances per iteration isn't as much of a big deal as when the incremental algorithm was developed. Much like in the [slab AABB intersection algorithm](https://en.wikipedia.org/wiki/Slab_method), these distances can be computed efficiently by solving for the parametric equation `P(t) = origin + dir * t`. The trick here is that, like in DDA, only the distances to the 3 front faces need to be computed.

This yields a relatively compact casting function, that is actually slightly faster than the [incremental DDA](https://www.shadertoy.com/view/4dX3zl) even without any acceleration structures - or at least that's what my very [precarious benchmark](./sketches/dda_vs_parametric.glsl) suggests.

```glsl
bool rayCast(vec3 origin, vec3 dir, out HitInfo hit) {
    vec3 invDir = 1.0 / dir;
    vec3 tStart = (step(0.0, dir) - origin) * invDir;
    ivec3 voxelPos = ivec3(floor(origin));

    for (uint i = 0; i < MAX_TRAVERSAL_ITERS; i++) {
        vec3 sideDist = tStart + vec3(voxelPos) * invDir;
        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z) + 0.001;
        vec3 currPos = origin + tmin * dir;
        
        voxelPos = ivec3(floor(currPos));

        if (!isInBounds(voxelPos)) break;
        if (!getStepPos(voxelPos, dir)) {
            hit.mat = getVoxelMaterial(voxelPos);
            hit.pos = currPos;
            
            bvec3 sideMask = greaterThanEqual(vec3(tmin), sideDist);
            hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
            hit.norm = mix(vec3(0), -sign(dir), sideMask);
            return true;
        }
    }
    return false;
}
// Attempts to step `pos` by a safe amount.
// Returns false if inside a non-empty voxel.
bool getStepPos(inout ivec3 pos, vec3 dir) {
    return isEmptyVoxel(pos);
}
```

The bias added to `tmin` is needed to ensure that the resulting intersection position gets rounded _through_ voxel boundaries, otherwise the ray will get stuck. Sadly, this leads to some artifacts around rays that are either too near or too far from voxels, in which case edges are missed _or_ the bias gets rounded off due to limited float precision.

<div align="center">
  <img src="VoxelNotes/prt_precision_near.png" width="48%">
  <img src="VoxelNotes/prt_precision_far.png" width="33.6%">
</div>

---

Instead of using a constant, I found that biasing by a few ULPs is enough to get rid of 99% artifacts: `tmin = uintBitsToFloat(floatBitsToUint(tmin) + 5);`. This is essentially adding a small value that varies according to the float magnitude. Shader specs demand some minimum precision around float ops, but this might not work universally (and it does in fact fail on my CPU AVX implementation, although `-ffast-math` may be to blame).

Like in [ESVO](#sparse-voxel-octrees-svos-and-dags), a possibly better approach would be to use normalized coordinates across a finite grid, to  emulate a fixed-point format without resorting to integers (which usually have lower throughput on GPUs). This would at the very least allow the bias to be embedded in `tStart` instead of inside the loop, and possibly make it easier to implement _sub-voxels_.

There's a few more things:
- The ray origin should be kept close to 0,0,0 to maximize available float precision, translation should be applied to the world instead (integer voxel coordinates). This can be done by offsetting both `voxelPos` by `floor(actualCameraPos)` and ray origin by `fract(actualCameraPos)`. Translating the view-projection matrix instead of ray origin by the fraction will cause noticeable jittering artifacts when FOV is too low and mess up with temporal reprojection (mat4 precision seems to be way lower than I thought?).
- Rays originating from outside the grid need to be clipped to the grid bounds using an [AABB intersection algorithm](https://en.wikipedia.org/wiki/Slab_method).

## Acceleration structures
Voxel grids are usually very sparse, so most of the time spent by the ray tracer will be wasted stepping through empty space. Acceleration structures help minimize this problem and enable real-time rendering on even lower-end GPUs.

### Distance Fields (DFs)
Distance fields are perhaps the simplest such acceleration structure, and they're quite effective at it. The idea is that for every empty voxel, the distance to the nearest occupied voxel is stored along with it, so the ray caster can step to a new position by offsetting the current position along the ray based on that distance, very much like ray marching.

The main issue with DFs is that they're hard to update incrementally, as even a single change could invalidate the entire field. One idea to limit this problem would be to have fixed-size DF chunks that are independent of each other, but that would limit efficiency around borders in a similar way as octrees, as I will show in the next section.

<div align="center">
  <img src="VoxelNotes/distfield_demo.png" width="33%">
</div>

---

DFs based on the Manhattan or Chebyshev metrics can be generated in linear complexity by propagating distances along individual rows over one pass per axis. A similar algorithm for generation of squared Euclidean DFs exists, but [PC94] shows that Manhattan DFs actually provide jump steps that are on average 3.7% longer than Euclidean DFs, which combined with the much simpler implementation, makes it a much more appealing choice.

```glsl
layout(r8ui) uniform uimage3D u_DistField;

uniform int u_Size;
uniform int u_Axis;

void initialize() {
    for (int i = 0; i < u_Size; i++) {
        ivec3 pos = ivec3(i, gl_GlobalInvocationID.xy);

        uint d = isEmptyVoxel(pos) ? 255 : 0;
        imageStore(u_DistField, pos, uvec4(d));
    }
}

#define PROPAGATE(_s) \
    for (int i = 1; i < u_Size; i++) {                                          \
        ivec3 pos = ivec3(i, gl_GlobalInvocationID.xy)._s;                      \
                                                                                \
        uint d = imageLoad(u_DistField, pos).r;                                 \
        d = min(d, imageLoad(u_DistField, pos - ivec3(1, 0, 0)._s).r + 1);      \
        imageStore(u_DistField, pos, uvec4(d));                                 \
    }                                                                           \
    for (int i = u_Size - 2; i >= 0; i--) {                                     \
        ivec3 pos = ivec3(i, gl_GlobalInvocationID.xy)._s;                      \
                                                                                \
        uint d = imageLoad(u_DistField, pos).r;                                 \
        d = min(d, imageLoad(u_DistField, pos + ivec3(1, 0, 0)._s).r + 1);      \
        imageStore(u_DistField, pos, uvec4(d));                                 \
    }                                                                           


layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    switch (u_Axis) {
        case 0: { initialize(); PROPAGATE(xyz); break; }
        case 1: {               PROPAGATE(yxz); break; }
        case 2: {               PROPAGATE(zyx); break; }
    }
}
```

Step positions from Manhattan or Chebyshev distances can be calculated by scaling with the projection of the ray direction ([PC94], [RDT00]):

```glsl
bool getStepPos(inout ivec3 pos, vec3 dir) {
    vec3 distScale = dir / (abs(dir.x) + abs(dir.y) + abs(dir.z));    // Manhattan
    vec3 distScale = dir / (max(dir.x, max(dir.y, dir.z));            // Chebyshev (untested)

    uint distToNearest = texelFetch(u_DistField, pos, 0).r;
    if (distToNearest > 0) {
        pos += ceil(distToNearest * distScale - step(0.0, dir));
        return true;
    }
    return false;
}
```

---

**Papers:**
- [RDT00]: [Fast ray-tracing of rectilinear volume data using distance transforms](https://www.researchgate.net/publication/3410902_Fast_ray-tracing_of_rectilinear_volume_data_using_distance_transforms)
- [ADT07]: [Accelerated regular grid traversals using extended anisotropic chessboard
distance fields on a parallel stream processor](https://www.sciencedirect.com/science/article/abs/pii/S0743731507001177)
- [PC94]: [Proximity clouds — an acceleration technique for 3D grid traversal](https://link.springer.com/article/10.1007/BF01900697)

[RDT00] and [ADT07] propose an interesting extension of DFs that helps mitigate the convergence issue around rays that are nearly parallel to voxels by building multiple DFs, each considering only voxels ahead of a specific direction octant. The memory and generation costs makes them not very practical for bigger scenes.

**Parabolic Euclidean Distance Transform:**
- https://prideout.net/blog/distance_fields/
- https://acko.net/blog/subpixel-distance-transform/
- https://github.com/seung-lab/euclidean-distance-transform-3d

### Occupancy Maps (OCMs)
These are simply bitmaps where each bit tells the presence of a voxel. Creating an hierarchy of OCMs makes for a structure that is identical to octrees, but without all the indirection and sparseness.

They're cheap and trivial to generate, and still make for a very effective acceleration structure (but not quite as DFs). Here's a simple implementation in terms of `getStepPos()`:

```glsl
bool getStepPos(inout ivec3 pos, vec3 dir) {
    // Find highest empty LOD (mip level), k
    int k = 1;
    while (k < maxLevel && getOccupancy(pos >> k, k) == 0) {
        k++;
    }
    k--;
    if (k == 0) return false;

    // Align pos to border of LOD (round up or down depending on dir)
    int m = (1 << k) - 1;
    pos.x = dir.x < 0 ? (pos.x & ~m) : (pos.x | m);
    pos.y = dir.y < 0 ? (pos.y & ~m) : (pos.y | m);
    pos.z = dir.z < 0 ? (pos.z & ~m) : (pos.z | m);

    return true;
}
uint getOccupancy(ivec3 pos, int k) {
    // u_OccupancyStorage is a usampler3D of layout(r8ui), containing 2x2x2 voxels per byte.
    // Odd mips are redundant and can be evaluated by `cell != 0`, but requires multiple
    // textures or a custom buffer for actual memory savings.
    uint cell = texelFetch(u_OccupancyStorage, pos >> 1, k).r;
    uint idx = (pos.x & 1) | (pos.y & 1) << 1 | (pos.z & 1) << 2;
    return (cell >> idx) & 1;
}
```

An obvious inefficiency with this code is that it needs to search for the highest non-occupied mip from scratch on every iteration. The [ESVO](#sparse-voxel-octrees-svos-and-dags) paper proposes the use of a stack to enter and leave octree nodes, but a similar optimization can be done more easily here by inlining getStepPos() and preserving the mip-level `k` between iterations, so the search can continue at the level found in the previous iteration.

```glsl
    int k = 3; // arbitrary initial level
    for (int i = 0; i < MAX_TRAVERSAL_ITERS; i++) {
        // ...

        if (!isInBounds(voxelPos)) break;

        if (getOccupancy(voxelPos >> k, k) != 0) {
            while (--k >= 0 && getOccupancy(voxelPos >> k, k) != 0) ;
            if (k < 0) { /* ... found occupied voxel, end traversal */ return true; }
        } else {
            while (++k < maxLevel && getOccupancy(voxelPos >> k, k) == 0) ;
            k--;
        }
    }
```

(These guarded loops are probably not very GPU friendly and look like they might suffer from divergence. A possibly better impl would use a single loop and a "step-up/down" flag.)

---

So far, I was not very satisfied with OCMs because they lead to many extra hops through cell boundaries. Like with anisotropic DFs, this issue can be reduced somewhat with a more sophisticated occupancy sampling function that takes the ray direction into account:

<div align="center">
  <img src="VoxelNotes/traversal_2d_iso.png" width="33%">
  <img src="VoxelNotes/traversal_2d_ani.png" width="33%">
  <p><i>2D occupancy map traversal demo. Isotropic traversal on the left, greedy anisotropic correction on the right.</i></p>
</div>

Similarly to greedy meshing, this can be implemented by expanding the cell size along the ray octant as much as possible based on neighboring occupancy state. Limiting the expansion size to 1 allows the use of a small lookup table instead of loops, at the fixed cost of 7 extra occupancy fetches to build the index.

I didn't spend too much time tuning the code because the actual performance gains weren't that promising, although the global number of iterations decreases significantly by around 1/2 and 1/3 on average - which to be fair is not that surprising given that most cells are grown to twice their original size. Taking 7 extra memory fetches per iteration is clearly not quite worth it, or at least not for primary rays that are very coherent.

```glsl
    // 0 1   8 16   x+
    // 2 4  32 64   y+
    //        z+
    uint mask = 0;
    for (int j = 0; j < 7; j++) {
        ivec3 cornerPos = (ivec3(j + 1) >> ivec3(0, 1, 2)) & 1;
        mask |= getOccupancy((pos >> k) + cornerPos * sign(rayDir), k) << j;
    }

    // See GenAnisoExpansionTable.js
    const uint[] expansionTable = {
        0x45654567, 0x01210123, 0x41614163, 0x01210123, 0x45254523, 0x01210123, 0x41214123, 0x01210123,
        0x45654563, 0x01210123, 0x41614163, 0x01210123, 0x45254523, 0x01210123, 0x41214123, 0x01210123
    };
    uint tableIdx = mask * 4;
    uint expandMask = expansionTable[tableIdx >> 5] >> (tableIdx & 31);
    ivec3 stepSize = (ivec3(expandMask) >> ivec3(0, 1, 2) & 1) << k;  // mask.xyz ? (2^k) : 0
    pos += stepSize * sign(rayDir);
```

<div align="center">
  <img src="VoxelNotes/ocm_traversal_iso.png" width="45%">
  <img src="VoxelNotes/ocm_traversal_ani.png" width="45%">
  <p><i>3D occupancy map traversal heatmap (grayscale between 0..64+ iterations). Isotropic traversal on the left, greedy anisotropic correction on the right.</i></p>
</div>

## Augmenting brickmaps with OCMs for efficient space skipping
So far I had been using flat grids for voxel storage, but they become impractical extremely quickly for even moderately bigger dimensions - at 1 byte per voxel, a 2048³ grid already requires 8GB of memory, which is the cap where many current consumer GPUs are at.

For simplicity, the structure I settled for is a 2-level grid consisting of 8³ voxel bricks within 4³ sectors, similarly to [RTEV15]. (In retrospective, 8³ feels a bit too small and 16³ feels too big without having more LOD levels, although the later is probably still a better choice for bigger scenes due to reduced overhead.)

For GPU storage, I use a single buffer and a [free list](https://en.wikipedia.org/wiki/Free_list) to allocate brick slots (addresses). Each sector contains a _64-bit allocation mask_ and a single _base brick slot_ within the storage buffer, allowing individual brick slots to be computed implicitly (without pointers) by counting preceding allocations using a popcount instruction: `baseSlot + bitCount(allocMask & ((1ull << brickIdx) - 1))`. (On the GPU, this is actually implemented using two popcount instructions, as they don't seem to provide a 64-bit version of it.)

The key realization I had is that the sector allocation masks can also be used to derive 3 occupancy LODs without any extra memory fetches, which alone can speedup the traversal to surprisingly useable levels. To further reduce traversal cost, I have two extra layers of these occupancy masks - one at sector level and the other at voxel level. The voxel OCM is generated by a compute shader after brick data is uploaded.

A potential issue with this structure however, is that reallocations may lead to fragmentation over time. I have not found this to be a major issue yet, but presumably a smarter allocation algorithm would help. For very long running applications facing frequent reallocations, a defragmentation procedure might be inevitable. (anecdotal as VMA supports manual defragmentation.)

---

On tested scenes (voxelized Bistro at 4096x1024x4096 resolution), my implementation runs at around 250-350M rays per second on a Intel Xe iGPU with 96 EUs - well enough for >60FPS at 1080p assuming only primary rays are cast. Diffuse bounce rays are naturally incoherent and lead to considerable divergence and cache misses in the tracing loop, reducing throughput by around 50-70%. A possible mitigation for this issue would be to [bin rays](https://interplayoflight.wordpress.com/2022/05/08/increasing-wave-coherence-with-ray-binning/) based on their direction and position to increase wave coherence.

```glsl
// Given an occupancy mask and starting position (as a linear index in a 4³ grid),
// returns the size of the biggest empty LOD.
// The mask is split into lower and upper bits - uvec2(0..31, 32..63).
int getIsotropicLod(uvec2 mask, uint idx) {
    if (mask == uvec2(0)) {
        return 4;
    }
    uint currMask = idx < 32 ? mask.x : mask.y;

    if ((currMask >> (idx & 0xAu) & 0x00330033u) == 0) {
        return 2;
    }
    if ((currMask >> (idx & 31u) & 1u) == 0) {
        return 1;
    }
    return 0;
}
```

---

**Refs:**
- [RTEV15]: [Real-time Ray tracing and Editing of Large Voxel Scenes](https://studenttheses.uu.nl/handle/20.500.12932/20460)
  - https://github.com/stijnherfst/BrickMap

### Extra: Surface LODs and culling
TODO - Similar to the brickmap paper, my rough idea is to bit-pack LOD and a ray-visited flag in the BaseSlot field (2 + 1 bits). If the visited flag is false, append sector position into a request buffer for CPU read back. Unsure how to deallocate non-visible bricks.

### Extra: Anisotropic LODs
Anisotropic LODs restricted to 4³ bitmasks and only 2-3 levels were considerably less effective than my older OCM implementation. The simplest solution I could come up to sample the bitmasks was through a series of [comparisons per axis](https://github.com/dubiousconst282/VoxelRT/blob/247079bef4d3e0fa5ccff6ba1a82e77b02e924c9/src/VoxelRT/Shaders/VoxelTraversal.glsl#L36), but that was still not fast enough to beat plain isotropic sampling - overall, the number of iterations decreases by around 60% while traversal time increases by 10-15%.

<table align="center">
    <tr>
        <th> <p align="center">Isotropic</p> </th>
        <th> <p align="center">Anisotropic</p> </th>
    </tr>
    <tr>
        <td> <img src="VoxelNotes/aniso/mask_lod_iso1.png"> </td>
        <td> <img src="VoxelNotes/aniso/mask_lod_ani1.png"> </td>
    </tr>
    <tr>
        <td> <img src="VoxelNotes/aniso/mask_lod_iso2.png"> </td>
        <td> <img src="VoxelNotes/aniso/mask_lod_ani2.png"> </td>
    </tr>
</table>

---

[Doubling the speed of my game's graphics](https://www.youtube.com/watch?v=P2bGF6GPmfc) by Douglas Dwyer presents a related approach called "mask optimization", to filter the occupancy state using a pre-generated table of _interaction masks_ that cover all voxels where a given ray direction and starting position could possibly intersect with.

Generating these masks is simple enough, and so is sampling, as the following snippets show. I noticed that using a constant array for the table made the shader _excruciatingly_ slow, so I used an uniform buffer instead, which is created at the same time as the shader. (The least worrying explanation for this would be a driver bug or missed optimization, which is not to be unexpected from Intel's OpenGL Windows drivers.)

The performance improvement varies depending on view angle and how geometry aligns to LOD cells, but from my tests the gains range around 5-15%. The impact is much more profound when the masks are used for skipping exclusively in 4³ or 1³ size steps.

```cpp
// Based on https://www.youtube.com/watch?v=P2bGF6GPmfc
static void GenerateRayInteractionMaskLUT(uint64_t table[64 * 8]) {
    for (uint64_t dirOct = 0; dirOct < 8; dirOct++) {
        glm::ivec3 dir = (glm::ivec3(dirOct) >> glm::ivec3(0, 1, 2) & 1) * 2 - 1;

        for (uint64_t originIdx = 0; originIdx < 64; originIdx++) {
            uint64_t mask = 0;

            for (uint64_t j = 0; j < 64; j++) {
                glm::ivec3 pos = MaskIndexer::GetPos(originIdx) + MaskIndexer::GetPos(j) * dir;

                if (MaskIndexer::CheckInBounds(pos)) {
                    mask |= 1ull << MaskIndexer::GetIndex(pos);
                }
            }
            table[originIdx + dirOct * 64] = mask;
        }
    }
}
```

Masking before LOD derivation:
```glsl
    uint dirOctant = (dir.x < 0 ? 0 : 1) + (dir.y < 0 ? 0 : 2) + (dir.z < 0 ? 0 : 4);
    occMask &= RayInteractionMaskLUT[maskIdx + dirOctant * 64];
```

---

Generating a larger table that is more fine grained around ray directions might help, but evidently not much, since the origin cell position also has a significant impact over how "fine" the mask can be - it must be conservative and cover all voxels that could possibly intersect the ray at any given origin, and at the coarseness of integer coordinates, it doesn't get much finer than when considering only ray octants.

My sketches implementing this idea are available in [ray_filter_mask.js](./sketches/ray_filter_mask.js) and [RayMaskFilterLUT.cpp](./sketches/RayMaskFilterLUT.cpp).

---

A nested DDA traversal over the bitmask could prove to be helpful after all, since unlike the LOD tests, it would always advance the ray position. Because each iteration of the main traversal loop would invoke it unconditionally, there would be no divergence/wasted lanes like in the naive multi-level DDA.

### Extra: Misc ideas for bulk updates
While this structure makes it trivial to update existing voxels on the GPU, creating new bricks is less straightforward. One idea would be to keep a large pool of free memory available, so that new bricks can be created by simply bump-allocating via an atomic counter. (But the devil probably hides in the details about host synchronization.)

For much simpler cases involving embarrassingly parallel bulk updates, taking advantage of [SIMD](https://en.algorithmica.org/hpc/simd) is relatively easy with a "dispatch" helper and a good SIMD library (or a dedicated language like ISPC, which might allow for quicker dev cycles).

As a demonstration, the following is the entire implementation for the apply of my brush tool, which is based on filling a distance field of a line. At static 150 radius (27M voxels), it takes around 30ms to run on a single thread, or around a little over 1ns per voxel. Apart from the need to manually flatten control flow, the code is not really much different from what one would normally write.

```cpp
// https://iquilezles.org/articles/distfunctions/
static VFloat sdCapsule(VFloat3 p, glm::vec3 a, glm::vec3 b, float r) {
    VFloat3 pa = p - a, ba = b - a;
    VFloat h = simd::clamp(simd::dot(pa, ba) / simd::dot(ba, ba), 0.0, 1.0);
    return simd::length(pa - ba * h) - r;
}
void BrushSession::Dispatch(VoxelMap& map) {
    glm::ivec3 minPos = glm::min(Pars.PointA, Pars.PointB) - (int)(Pars.Radius + 0.5);
    glm::ivec3 maxPos = glm::max(Pars.PointA, Pars.PointB) + (int)(Pars.Radius + 0.5);
    bool isErasing = Pars.Material.IsEmpty();
    VRandom rng(Pars.RandomSeed);

    // TODO: skip unaffected sectors/bricks (sample DF at center to check)
    map.RegionDispatchSIMD(minPos, maxPos, !isErasing, [&](VoxelDispatchInvocationPars& invoc) {
        // This lambda is invoked over the entire region AABB and processes multiple voxels at once using
        // SIMD in a GPU style, where each 32-bit vector lane is treated as an individual "thread" running in lock step.
        // For SSE/NEON, AVX2, or AVX512, this means each vector packs 4, 8, or 16 fields of distinct elements, respectively.
        VFloat3 pos = VFloat3(simd::conv2f(invoc.X), simd::conv2f(invoc.Y), simd::conv2f(invoc.Z)) + 0.5f;
        VMask mask = sdCapsule(pos, Pars.PointA, Pars.PointB, Pars.Radius) < 0.0;

        if (Pars.Probability < 1.0f) {
            mask &= rng.NextUnsignedFloat() < Pars.Probability;
        }
        if (Pars.Action == BrushAction::Replace) {
            mask &= invoc.VoxelIds != 0;
        }
        // Branchless version of `if (mask[i]) invoc.VoxelIds[i] = Pars.Material.Data;` over all lanes.
        // This compiles into one or two instructions.
        invoc.VoxelIds.set_if(mask, Pars.Material.Data);

        // If any changes were made to `invoc.VoxelIds`, return true so that they are
        // committed to memory and the containing brick gets marked as dirty (pending GPU upload).
        return simd::any(mask);
    });
}
```

### Extra: Run-length compression
Using the same popcnt trick as sparse brick storage, voxel data can be run-length compressed very quickly in a way that allows for O(1) random accesses. Here's a simple example of how it works:

```
      Input: 11111 22 3 444 5 6 000000...
RestartMask: 00001 01 1 001 1 1 000000...1
 Compressed: 1234560

DecompressedValue = Compressed[popcnt64(RestartMask & ((1ull << TileVoxelIdx) - 1))];
```

I have benchmarked a rudimentary implementation, but did not finish integrating it as I concluded that LODs might be a simpler and more reliable solution for reducing GPU memory usage.

- _Bistro_ and _Sponza_ are non-solid voxelized meshes and have up to 240 unique voxel types.
- _Terrain_ is a simple terrain based on warped 3D noise and contains only one voxel type.
- RLE sizes include overhead of 1-byte per tile for storage offsets, followed by 8 bytes per non-empty tile (indicated by a sentinel offset value). This gives a maximum compression ratio of about `64/10` - when bricks are filled with exactly one block type.

| Scene         | Source     | Non-empty  | RLE        |
|---------------|------------|------------|------------|
| Sponza 2k     | 238.023 MB | 36.736 MB  | 50.624 MB  |
| Bistro 4k     | 471.307 MB | 68.4949 MB | 88.8887 MB |
| Bistro 8k     | 2034.28 MB | 282.806 MB | 364.978 MB |
| Terrain 2k    | 417.41 MB  | 391.784 MB | 57.8487 MB |

## Sparse Voxel Octrees (SVOs) and DAGs
Octrees are evidently the most popular and influential data structure for voxel storage. SVDAGs further compress memory requirements by reusing tree paths.

...and that's all I will bother writing, because I have not implemented them myself and have only skimmed through the linked papers so far. My impression is that the generalization over fixed-depth hierarchical grids complicates everything else for arguably little benefit, given the scale I'm targetting.

- [ESVO]: [Efficient Sparse Voxel Octrees - Analysis, Extensions, and Implementation](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation)
- [Interactively Modifying Compressed Sparse Voxel Representations](https://graphics.tudelft.nl/Publications-new/2020/CBE20/ModifyingCompressedVoxels-main.pdf)
  - https://github.com/Phyronnaz/HashDAG
- [Editing Compressed High-resolution Voxel Scenes with Attributes](https://onlinelibrary.wiley.com/doi/10.1111/cgf.14757)
  - Most recent(?) paper on SVDAGs. Combines geometry SVDAG with decoupled and chunked attribute data.

Complementary:
- [Advanced Octrees 2: node representations](https://geidav.wordpress.com/2014/08/18/advanced-octrees-2-node-representations/)
- https://github.com/DavidWilliams81/cubiquity
  - https://github.com/DavidWilliams81/cubiquity/blob/master/src/application/commands/view/glsl/pathtracing.frag#L333
- https://github.com/AsperTheDog/GPU_SVOEngine
  - https://reddit.com/r/VoxelGameDev/comments/1copcpp/people_who_want_to_implement_an_svo_what_parts/

## Extra resources

### Ray-Aligned Occupancy Maps
[ROMA23] presents a very interesting approach for approximate ray tracing in close to O(1) complexity by exploiting [bit-scan instructions](https://en.wikipedia.org/wiki/Find_first_set). The idea is to create several occupancy maps that are each rotated by a random direction, so ray tracing can choose the one that is most aligned to the ray direction and then use the bit-scan instructions to compute step sizes.

Since grid rotations are lossy, ROMAs can only approximate intersections and are only suitable for diffuse light rays when used in conjunction to temporal accumulation. (Though it may be possible to use a conservative rotation algorithm that over-fills to avoid over-steps for primary rays.)

- [ROMA23]: [Ray-aligned Occupancy Map Array for Fast Approximate Ray Tracing](https://zheng95z.github.io/publications/roma23)

### Parallax Ray Marching
Parallax Ray Marching avoids most of the acceleration problem by first rasterizing opaque bounding-boxes at a coarser voxel resolution, so a initial ray origin can be set to the world-space position given to the fragment shader.

The ESVO paper proposes a similar approach to PRM called "Beam Optimization", to first render the scene at some fraction of the native resolution over coarser LODs to generate depth values for origin estimation.

Of course, these methods only work for the primary camera ray, so renderers need to use other approaches for lighting effects.

- [Drawing MILLIONS of voxels on an integrated GPU with parallax ray marching [Voxel Devlog #4]](https://youtu.be/h81I8hR56vQ&t=243)
- [Teardown Frame Teardown](https://acko.net/blog/teardown-frame-teardown)
- [Teardown Teardown](https://juandiegomontoya.github.io/teardown_breakdown.html) (more detailed)

### Greedy Binary Meshing, Global Lattice (N+N+N planes)
Greedy meshing is typically restricted to voxels and lighting of the same type, but a more compact mesh can be generated by considering only binary occupancy states. The actual voxel types can be sampled in the fragment shader from a 3D texture or buffer. Texture UVs and normals can be easily derived from the interpolated world space position (using fract() and screen-space derivatives), so vertices only need to store position and possibly lighting information.

This idea can be further extended into the "global lattice" method, where no voxel mesh needs to be generated at all and instead N*3 static planes can be drawn directly. One problem with this method is the significant overdraw caused by drawing thousands of overlapping planes, which could perform worse than plain DDA ray tracing.

- [Greedy Meshing Voxels Fast - Optimism in Design Handmade Seattle 2022](https://www.youtube.com/watch?v=4xs66m1Of4A)
  - https://new.reddit.com/r/VoxelGameDev/comments/1ceau2a/global_lattice_implementation
- https://www.reddit.com/r/VoxelGameDev/comments/nip02b/comment/gz3urmf
- https://www.youtube.com/watch?v=40JzyaOYJeY - Voxel Meshing Optimizations
- https://www.youtube.com/watch?v=qnGoGq7DWMc - Blazingly Fast Greedy Mesher

### Octree Splatting
Another way to render voxel octrees is by recursively projecting and filling octree nodes to screen coordinates.

- https://github.com/dairin0d/OctreeSplatting/blob/main/Notes/Overview.md
