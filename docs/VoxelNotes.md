# Voxel ray tracing notes
Despite the popularity of voxel rendering, technical resources about it seem to be quite scarse and vague. My writing skills are lacking, so this will only be some sort of _devlog / half-assed tutorial / dump of links that i've only skimmed over_, about some of the stuff I learned while researching about it.

---

The natural approach for rendering voxels on a GPU would be using meshes, it's a proven method that works well for voxels at a reasonable scale without resorting to LODs. Scaling it up to more voxels can get quite complicated though - think draw calls, buffer management, overdraw, chunk occlusion, greedy meshing (and its too many curses) ...the sort of stuff that actually makes for a pretty good way to learn graphics APIs and programming, I guess.

Ray tracing is not easier to implement efficiently either (surprise!), but conceptually much simpler and flexible, and still surprisingly fast even without any acceleration structures, as demonstrated by these remarkable shadertoys:

- [Branchless Voxel Raycasting](https://www.shadertoy.com/view/4dX3zl)
- [[SH16C] Voxel Game](https://www.shadertoy.com/view/MtcGDH)
- [Voxel game Evolution](https://www.shadertoy.com/view/wsByWV)

---

The classic way to ray trace voxels is by using the [Fast Voxel Traversal](https://github.com/cgyurgyik/fast-voxel-traversal-algorithm/blob/master/overview/FastVoxelTraversalOverview.md) algorithm, which is a simple DDA loop that incrementally steps through whichever voxel has the closest side intersecting the ray until it hits a non-empty voxel.

There's a slightly different parametric algorithm that allows for much simpler and more efficient variable-size steps, whereas adapting the incremental DDA isn't straightforward as it uses a relatively complicated distance setup. Here's my (shortened) implementation that is based on a few other samples:

```glsl
bool rayTrace(vec3 origin, vec3 dir, out HitInfo hit) {
    vec3 invDir = 1.0 / dir;
    vec3 tStart = (max(sign(dir), 0.0) - origin) * invDir;
    vec3 currPos = origin;
    vec3 sideDist = vec3(0);

    for (int i = 0; i < MAX_ITERS; i++) {
        ivec3 voxelPos = ivec3(floor(currPos));

        if (!isInBounds(voxelPos)) break;
        if (!isEmptyVoxel(voxelPos)) {
            bvec3 sideMask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));

            hit.mat = getVoxelMaterial(voxelPos);
            hit.pos = currPos;
            hit.uv = fract(mix(currPos.xz, currPos.yy, sideMask.xz));
            hit.norm = mix(vec3(0), -sign(dir), sideMask);
            return true;
        }
        sideDist = tStart + getStepPos(voxelPos, dir) * invDir;
        float tmin = min(min(sideDist.x, sideDist.y), sideDist.z) + 0.001; // dist to min crossing side
        currPos = origin + tmin * dir;
    }
    return false;
}
ivec3 getStepPos(ivec3 pos, vec3 dir) {
    ivec3 stepSize = ivec3(0);
    return pos + stepSize * (dir < 0 ? -1 : +1);
}
```

From my experiments, even without any acceleration this version is slightly faster than [Branchless DDA](https://www.shadertoy.com/view/4dX3zl), although in terms of ALU usage they aren't really much different - just a few MADs and integer conversions that should be really cheap. I haven't seriously profiled anything, but I'm very convinced that this algorithm would be an improvement over some other DDA hybrids I've seen people describing (which all sound like recipes for divergence/poor occupancy) - like switching between ray marching and DDA, or nesting multi-level DDAs (which actually seems to be used by Teardown).

A few notes though:
- The small bias added to `tmin` is needed to prevent the ray from getting stuck due to limited floating-point precision. A high bias will lead to artifacts around voxel edges (due to hit misses), and a small one will get rounded off on big values and have no effect. I found `0.001` to be reasonably effective and it should work with coords up to 16383, although a more reliable approach is to step to the next representable float value by incrementing its [binary representation directly](https://stackoverflow.com/questions/1245870/next-higher-lower-ieee-double-precision-number): `tmin = uintBitsToFloat(floatBitsToUint(tmin) + 4);`.
- To avoid hitting other FP limitations around big/distant coordinates, the ray origin should be keept close to 0,0,0 and translation should be applied to the world instead. I've done this by keeping the view-projection matrix at `fract(actualCameraPos)` and offseting voxel queries by `floor(actualCameraPos)`, an ivec3 uniform passed to the shader.
- Rays originating from outside the grid need to be clipped to the grid bounds using an [AABB intersection algorithm](https://en.wikipedia.org/wiki/Slab_method).

## Acceleration structures
Voxel grids are usually very sparse, so most of the time spent by the ray tracer will be wasted stepping through empty space. There exists many accceleration techniques that help mitigate this issue and enable real-time tracing on even lower-end GPUs.

### Distance Fields (DFs)
Distance fields are perhaps the simplest such acceleration structure, and it's a quite effective one. The idea is that for every empty voxel, the distance to the nearest occupied voxel is stored along with it, so the ray tracer can step to a new position by offseting the current position along the ray based on that distance.

DFs based on the Manhattan or Chebyshev metrics can be generated in linear complexity by propagating distances along individual rows over one pass per axis. A similar algorithm for generation of squared Euclidean DFs exists, but [^PC94] shows that Manhattan DFs actually provide jump steps that are on average 3.7% longer than Euclidean DFs, leaving few reasons to bother using it.

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

Step sizes from non-Euclidean distances can be computed as follow:

```glsl
vec3 distScale = dir / (abs(dir.x) + abs(dir.y) + abs(dir.z));    // Manhattan
vec3 distScale = dir / (max(dir.x, max(dir.y, dir.z));            // Chebyshev (untested)

// ...
vec3 stepSize = vec3(0);
if (distToNearest > 0) {
    stepSize = ceil(distToNearest * distScale - max(sign(dir), 0));
}
```

---

Ultimately, DFs are not quite well suitable for dynamic voxel scenes because mutations require recomputation of the entire DF or use of an iterative worklist algorithm for propagation between voxels/bricks up to a set distance limit. (Although they could also be isolated within individual bricks/sectors at the cost of decreased effectiveness.)

---

**Papers:**
- [^RDT00]: [Fast ray-tracing of rectilinear volume data using distance transforms](https://www.researchgate.net/publication/3410902_Fast_ray-tracing_of_rectilinear_volume_data_using_distance_transforms)
- [^ADT07]: [Accelerated regular grid traversals using extended anisotropic chessboard
distance fields on a parallel stream processor](https://www.sciencedirect.com/science/article/abs/pii/S0743731507001177)
- [^PC94]: [Proximity clouds — an acceleration technique for 3D grid traversal](https://link.springer.com/article/10.1007/BF01900697)

[^RDT00] and [^ADT07] propose and interesting extension of DFs that helps mitigate the convergence issue around rays that are nearly parallel to voxels by building multiple DFs, each considering only voxels ahead of a specific direction octant. Not very practical due to memory requirements.

**Parabolic Euclidean Distance Transform:**
- https://prideout.net/blog/distance_fields/
- https://acko.net/blog/subpixel-distance-transform/
- https://github.com/seung-lab/euclidean-distance-transform-3d

### Occupancy Maps (OCMs)
These are simply bitmaps where each bit tells the presence of a voxel. Creating mip-maps of OCMs makes for a structure that is identical to octrees, but without all the indirection and sparseness.

They're cheap and trivial to generate, and still make for a very effective acceleration structure (but not quite as DFs). Here's how they could be implemented in terms of `getStepPos()`:

```glsl
ivec3 getStepPos(ivec3 pos, vec3 dir) {
    // Find highest empty mip level, k
    int k = 1;
    while (k < maxLevel && getOccupancy(pos >> k, k) == 0) {
        k++;
    }
    k--;
    
    // Adjust `pos` to be at border of mip cell (round up or down depending on dir)
    int m = (1 << k) - 1;
    pos.x = dir.x < 0 ? (pos.x & ~m) : (pos.x | m);
    pos.y = dir.y < 0 ? (pos.y & ~m) : (pos.y | m);
    pos.z = dir.z < 0 ? (pos.z & ~m) : (pos.z | m);

    return pos;
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

An obvious inefficiency with this code is that it needs to search for the highest non-occupied mip from scratch on every iteration. The ESVO impl uses a stack to enter and leave octree nodes, but a similar optimization can be done more easily here by inlining getStepPos() and preserving the mip-level `k` between iterations, so the search can continue at the level found in the previous iteration. This also has the nice bonus that voxel materials only need to be queried at the end of traversal, saving precious memory bandwidth.

```glsl
    int k = 3; // arbitrary initial level
    for (int i = 0; i < MAX_ITERS; i++) {
        ivec3 voxelPos = ivec3(floor(currPos));
        if (!isInBounds(voxelPos)) break;

        if (getOccupancy(voxelPos >> k, k) != 0) {
            while (--k >= 0 && getOccupancy(voxelPos >> k, k) != 0) ;
            if (k < 0) { /* ... found occupied voxel, end traversal */ return true; }
        } else {
            while (++k < maxLevel && getOccupancy(voxelPos >> k, k) == 0) ;
            k--;
        }
        // ...
```

(These guarded loops are probably not very GPU friendly and look like they might suffer from divergence or something. An alternative impl would use a single loop and a "step-up/down" flag instead.)

---

So far, I was not very satisfied with OCMs because they lead to many extra hops through cell boundaries. Like with anisotropic DFs, this issue can be reduced somewhat with a more sophisticated occupancy sampling function that takes the ray direction into account:

<div style="text-align: center; font-style: italic;">
  <img src="VoxelNotes/traversal_2d_iso.png" width="33%">
  <img src="VoxelNotes/traversal_2d_ani.png" width="33%">
  <p>2D occupancy map traversal demo. Isotropic traversal on the left, greedy anisotropic correction on the right.</p>
</div>

Similarly to greedy meshing, this can be implemented by expanding the cell size along the ray octant as much as possible based on neighboring occupancy state. Limiting the expansion size to 1 allows the use of a small lookup table instead of loops, at the fixed cost of 7 extra occupancy fetches to build the index.

I didn't spend too much time tuning the code because the actual performance gains weren't that promising, although the global number of iterations decreases significantly to around 1/2 and 1/3 on average - which to be fair is not that surprising given that most cells are grown to twice their original size. Taking 7 extra memory fetches per iteration is clearly not quite worth it, or at least not for primary rays that are very coherent.

I'll describe a better implementation of OCMs using 64-bit masks in the [brickmap section](#anisotropic-lods).

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

<div style="text-align: center; font-style: italic;">
  <img src="VoxelNotes/ocm_traversal_iso.png" width="45%">
  <img src="VoxelNotes/ocm_traversal_ani.png" width="45%">
  <p>3D occupancy map traversal heatmap (grayscale between 0..64+ iterations). Isotropic traversal on the left, greedy anisotropic correction on the right. 1024³ grid, 1080p, Intel Xe iGPU.</p>
</div>

### Sparse Voxel Octrees (SVOs)
Oh yeah, SVOs... I can't say much about them because I have only skimmed through the ESVO paper, which is actually quite readable and has some neat ideas, but overall gives me the impression that octrees are somewhat complicated and still not flexible enough - at least not for what I care anyway.

- [Efficient Sparse Voxel Octrees - Analysis, Extensions, and Implementation](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation)
- [Advanced Octrees 2: node representations](https://geidav.wordpress.com/2014/08/18/advanced-octrees-2-node-representations/)
- https://github.com/DavidWilliams81/cubiquity
  - SVO-DAG impl: https://github.com/DavidWilliams81/cubiquity/blob/master/src/application/commands/view/glsl/pathtracing.frag#L333

## Brickmaps
Brickmaps (or chunking) can be thought as a compromise between flat grids and octrees for voxel storage, they help reduce memory waste by dividing the world into fixed-size chunks (bricks), allowing storage to be allocated only to non-empty chunks.

One slight difference between Minecraft chunks and brickmaps is that their chunks are "2D" and represent a vertical column of 16³ sections, which might be more efficient when using a top-level hashmap on short worlds.

(I have no idea where the term came from but I've grown fond of it...)

**Links**
- https://github.com/stijnherfst/BrickMap

### What I did
I settled for a 2-level brickmap of 8³ bricks within 4³ sectors. (8³ feels a bit too small so I want to consider changing it to 16³ at some point, or maybe adding an extra LOD mask.)

For GPU storage, I use a single buffer and a [free list](https://en.wikipedia.org/wiki/Free_list) to allocate brick slots. Each sector contains a _64-bit allocation mask_ and a single _base brick slot_ within the storage buffer, allowing individual brick slots to be computed implicitly (without pointers) by counting preceding allocations using a popcount instruction: `baseSlot + bitCount(allocMask >> ((1ull << brickIdx) - 1))`.

The sector allocation masks can also be used to compute 3 occupancy LODs without any extra memory fetches, which alone can speedup the traversal to surprisingly useable levels. Nevertheless, for the sake of traversal efficiency I added another OCM at the voxel-level that is generated by a compute shader after brick data is uploaded. This OCM is also tiled to 64-bit masks of 4³ voxels, so the sector LOD function can be reused for both.

```glsl
// 64-bit mask split into two uints as uvec2(0..31, 32..63)
int getIsotropicLod(uvec2 mask, uint idx) {
    if ((mask.x | mask.y) == 0) {
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

### Anisotropic LODs
TODO


## Extra: Ray-Aligned Occupancy Maps
[^ROMA23] presents a very interesting approach for approximate ray tracing in close to O(1) complexity by exploiting [bit-scan instructions](https://en.wikipedia.org/wiki/Find_first_set). The idea is to create several occupancy maps that are each rotated by a random direction, so ray tracing can choose the one that is most aligned to the ray direction and then use the bit-scan instructions to compute step sizes.

Since grid rotations are lossy, ROMAs can only approximate intersections and are only suitable for diffuse light rays when used in conjunction to temporal accumulation. (Though it may be possible to use a conservative rotation algorithm that over-fills to avoid over-steps for primary rays.)

- [^ROMA23]: [Ray-aligned Occupancy Map Array for Fast Approximate Ray Tracing](https://zheng95z.github.io/publications/roma23)

## Extra: Parallax Ray Marching
Parallax Ray Marching avoids most of the acceleration problem by first rasterizing opaque bounding-boxes at a coarser voxel resolution, so a initial ray origin can be set to the world-space position given to the fragment shader.

The ESVO paper proposes a similar approach to PRM called "Beam Optimization", to first render the scene at some fraction of the native resolution over coarser LODs to generate depth values for origin estimation.

Of course, these methods only work for the primary camera ray, so renderers need to use other approaches for lighting effects.

- [Drawing MILLIONS of voxels on an integrated GPU with parallax ray marching [Voxel Devlog #4]](https://youtu.be/h81I8hR56vQ&t=243)
- [Teardown Frame Teardown](https://acko.net/blog/teardown-frame-teardown)

## Extra: Greedy Binary Meshing, Global Lattice (N+N+N planes)
Greedy meshing is typically restricted to voxels and lighting of the same type, but a more compact mesh can be generated by considering only binary occupancy states. The actual voxel types can be sampled in the fragment shader from a 3D texture or buffer. Texture UVs and normals can be easily derived from the interpolated world space position (using fract() and screen-space derivatives), so vertices only need to store position and possibly lighting information.

This idea can be further extended into the "global lattice" method, where no voxel mesh needs to be generated at all and instead N*3 static planes can be drawn directly. One problem with this method is the significant overdraw caused by drawing thousands of overlapping planes, which could perform worse than plain DDA ray tracing.

- [Greedy Meshing Voxels Fast - Optimism in Design Handmade Seattle 2022](https://www.youtube.com/watch?v=4xs66m1Of4A)
- https://www.reddit.com/r/VoxelGameDev/comments/nip02b/comment/gz3urmf

## Extra: Octree Splatting
Another way to render voxel octrees is by recursively projecting and filling octree nodes to screen coordinates.

- https://github.com/dairin0d/OctreeSplatting/blob/main/Notes/Overview.md