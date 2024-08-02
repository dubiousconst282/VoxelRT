# VoxelRT
Voxel rendering experiments

## Analysis of acceleration structures
TODO

### Methods
- PlainDDA: Incremental DDA on flat grid (1-bit per voxel)
- MultiDDA: 2-level incremental DDA (with 8³ skips)
- eXtendedBrickMap: 2-level sparse grid with hierarchical space skipping (4³ sectors of 8³ bricks)
  - This is the "default" backend and currently the only one that implements voxel material/attributes.
- ManhattanDF/EuclideanDF: flat grid with space skipping through 128³ distance fields
- ESVO: Port of "Efficient Sparse Voxel Octrees", no contours

TODO:
- DirectionalSVDF: sparse grid with space skipping through 256³ 8-directional sub-voxel distance fields at 1:4 scale
- Tree64: 4³-tree
- Tree512: 8³-tree


### Characteristics
|                   | PlainDDA          | MultiDDA      | XBrickMap         | ESVO          | SimpleDF      | N³-tree       |
| ----------------- | ----------------- | ------------- | ----------------- | ------------- | ------------- | ------------- |
| Accel method      | none              | brick-level steps | hierarchical      | hierarchical  | ray marching  | hierarchical DDA |
| Memory overhead   | none              | 1 bit per 8³  | 8³ bits per brick + <br>12 bytes per sector | ~4 bytes per node| 1 byte per 1³ | TBD (4 + (n³/8) bytes per node) | 
| Edit cost (ins)   | O(1)              | O(1)          | O(1), sector-level reallocs | TBD, O(log n)? | O(n) | same as ESVO, log_N |
| Dynamic LODs      | no                | limited       | limited           | yes           | no            | yes           |
| Strengths         | good start point  | easy to impl, massive <br/> speedup over PlainDDA| good compromise <br/> between grids and trees |well known,<br/> contours| ?                 | less overhead than octrees |
| Drawbacks         | not fast enough   | simd divergence | ?               | somewhat complicated,<br> meh performance | heavy on mem and<br>bandwidth, slow edits  |  ?  |

---

### Benchmarks
TODO

Scene #1: Sponza 2048x1024x2048
|               | PlainDDA      | MultiDDA      | XBrickMap     | ESVO          | ManhattanDF   | 4³-tree       |
| ------------- | ------------- | ------------- | ------------- | ------------- | ------------- | ------------- |
| Memory usage  |
| Avg primary mrays/s | 
| Avg diffuse mrays/s |
| Avg iters/ray |
| Avg clocks/iter |
| GPU gen cost/8³ |  xx ns
| CPU gen cost/8³ |  xx ns


### Observations and speculations
TODO 

## Gallery
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

_Early screenshots of the CPU(bottom) and GPU(top) renderers, with 2-bounce diffuse lighting._

## Building
Build requirements: CMake, vcpkg (set `VCPKG_ROOT` envvar), and Clang. (clang-cl shipped with Visual Studio should also work.)  
Run requirements: Vulkan 1.3 and AVX2. Probably also luck for my dicey Vulkan and Slang code.


TODO
