# VoxelRT
This is somewhat of a progression of my [CPU rasterizer](https://github.com/dubiousconst282/GLimpSW) to ray tracing. Forever WIP, probably.

Here are some pretty shots of the CPU path tracer (currently without any fancy BRDF and just 3 diffuse bounces, ~300 frames accumulated):

![cpurt_bistro2](https://github.com/dubiousconst282/VoxelRT/assets/87553666/75634b45-e004-41dc-a640-ed7cb057f6a5)
![cpurt_sponza1](https://github.com/dubiousconst282/VoxelRT/assets/87553666/309d3bda-4fc8-4a3d-9c62-bb441d18affa)

---

The GPU renderer uses a 2-level brickmap of 8x8x8 voxels within 4x4x4 brick sectors, with 64-bit occupancy bitmasks that are used for both sparse storage allocation and traversal acceleration. These bitmasks allow for derivation of to 3 LODs without any extra memory fetches (only a single 64-bit load, which are actually broken up to 2x 32-bit loads for portability).

The CPU renderer uses the same bitmasks for acceleration but a flat grid for material storage because I didn't feel like porting the sparse storage from the GPU renderer. This limits it to a grid of at most 2^31 voxels (2048x512x2048) due to indexing with signed 32-bit ints. The code is not micro-optimized so I suspect throughput could be improved to some extent by unrolling the RayCast function to work over 2x vectors at once for better ILP, since memory gathers are high latency and there's plenty of ALU between them (something that GPUs do auto-magically).

---

I wrote a few notes/overview about voxel ray tracing acceleration methods: [docs/VoxelNotes.md](./docs/VoxelNotes.md)
