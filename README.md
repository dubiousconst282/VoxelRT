# VoxelRT
Voxel rendering experiments

---

<table>
    <tr>
        <td> <img src="https://github.com/dubiousconst282/VoxelRT/assets/87553666/9e526dde-cc19-4dec-86a4-fc9d9805d4d3"> </td>
        <td> <img src="https://github.com/dubiousconst282/VoxelRT/assets/87553666/feaca004-a2f1-471b-809d-bf24e0305522"> </td>
    </tr>
    <tr>
        <td> <img src="https://github.com/dubiousconst282/VoxelRT/assets/87553666/75634b45-e004-41dc-a640-ed7cb057f6a5"> </td>
        <td> <img src="https://github.com/dubiousconst282/VoxelRT/assets/87553666/309d3bda-4fc8-4a3d-9c62-bb441d18affa"> </td>
    </tr>
</table>

---

The data structure used for rendering is based on a 2-level brickmap of 8x8x8 voxels within 4x4x4 brick sectors, containing 64-bit occupancy bitmasks that are used for both sparse storage allocation and space-skipping. These bitmasks allow for derivation of to 3 LODs without any extra memory fetches, which enables simple and efficient space-skipping.

For more info, see: [docs/VoxelNotes.md](./docs/VoxelNotes.md)

---

Build requires CMake, vcpkg, and either Clang or GCC. clang-cl shipped with VS should also work.