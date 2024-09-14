# VoxelRT
Voxel rendering experiments

## Benchmark of acceleration structures

### Implementations
- PlainDDA: Incremental DDA over flat grid
- MultiDDA: Incremental DDA over 2-level grid (8³ bricks)
- eXtendedBrickMap: Space skipping over 3-level grid + 4³ occupancy bitmasks (4³ sectors -> 8³ bricks)
- ManhattanDF/EuclideanDF: Space skipping over tiled 256³ distance fields at 1:4 resolution + 4³ occupancy bitmasks
- ESVO: Port of "Efficient Sparse Voxel Octrees", no contours
- Tree64: Sparse voxel 4³-tree
- BrickBVH: Binary BVH with 8³ brick leafs + DDA (software impl)
- GreedyMesh: Rasterized greedy mesh

Maybe TODO:
- HybridBVH: BVH2 _or_ CWBVH with leafs made of XBrickMap(32³ sectors) _or_ Tree64 + partial splits
- DirectionalSVDF: tiled TBD³ 8-directional sub-voxel distance fields at 1:TBD resolution (distances at voxel scale rather than field resolution)

### Characteristics

| Method    | Accel method  | Mem cost (impl) |Edit cost | Advantages            | Drawbacks         |
|-----------|---------------|---------------|---------------|-----------------------|-------------------|
| PlainDDA  | none          | 1 bit per 1³  | O(1)          | good start point      |not fast enough    |
| MultiDDA  | space part    | 1 bit per 1³ +<br>1 bit per 8³  | O(1)          | ?                     |simd divergence    |
| XBrickMap | space part    | 8³ bits per brick +<br> 12 bytes per sector | ~O(1), *1| ? | ?   |
| FlatDF    | ray march     | 1 byte per 4³ +<br> 1 bit per 1³ | O(m)       | ?                     | ? |
| ESVO      | space part    | ~4 bytes per node | ~O(log m), *2  | contours?        | hard to edit      |
| Tree64    | space part    | 12 bytes per node | ~O(log m), *2  | less overhead<br> than octrees | (similar to ESVO) |
| BrickBVH  | geom part     | 16 bytes per node +<br> 8³ bits per brick | varies    | flexible, hw accel,<br>well researched | ?       |
| Meshing   | raster        | 8 bytes per quad | O(m)     | flexible               | hard to scale,<br>only primary rendering,<br>indirect |

- *1: XBrickMap supports arbitrary edits within a brick, but insertions and deletions may require reallocations at sector level to make space for new bricks.
- *2: Tree edits may require reallocations up to the root node to make space for new children nodes.  This is not currently implemented.

---

### Results

TL;DR:

<img src="./docs/img/chart_bench_prim.svg" width="700">

Throughput ratio for primary rays vs +1 bounce diffuse:

<img src="./docs/img/chart_bench_ratio_pt1.svg" width="700">

---

Labels:
- `Mrays/s`: million ray casts per second
- `Mrays/s PT1`: total mrays over 1 primary + 1 diffuse bounce
- `Iters/ray`: average number of traversal iterations per ray
- `Clocks/iter`: average GPU clocks per traversal iteration (based on subgroup `clockARB()`)
- `Sync`: upload and initialization cost of GPU buffers (per scene, not super accurate. Voxel materials only implemented in XBrickMap and Tree64)

All tests were run on an integrated GPU. I guesstimate at least 5-10x throughput on real hardware.

--------

**Scene**: Sponza 2k (499k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |30.9        |12.8 (0.41x)|391.7       |67.7        |138.0 ms    |52.2 ms     |
|MultiDDA    |146.2       |65.0 (0.44x)|58.1        |78.8        |155.4 ms    |63.0 ms     |
|XBrickMap   |166.9       |123.6 (0.74x)|19.2        |185.7       |143.6 ms    |265.1 ms    |
|ESVO        |94.0        |65.8 (0.70x)|46.9        |147.6       |0.0 ms      |638.7 ms    |
|Tree64      |153.0       |103.6 (0.68x)|19.4        |199.9       |0.0 ms      |1289.4 ms   |
|BrickBVH    |174.1       |102.0 (0.59x)|27.3        |123.1       |0.0 ms      |494.1 ms    |
|ManhattanDF |178.3       |87.7 (0.49x)|34.3        |106.2       |1121.8 ms   |617.7 ms    |
|EuclideanDF |174.0       |28.1 (0.16x)|39.7        |97.1        |1450.0 ms   |620.8 ms    |

--------

**Scene**: Eco House 2k (448k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |26.3        |3.7 (0.14x) |434.7       |69.7        |129.3 ms    |33.6 ms     |
|MultiDDA    |72.1        |34.7 (0.48x)|112.6       |88.8        |146.0 ms    |35.2 ms     |
|XBrickMap   |98.6        |77.8 (0.79x)|27.8        |238.1       |117.2 ms    |129.1 ms    |
|ESVO        |67.9        |53.2 (0.78x)|59.9        |170.3       |0.3 ms      |656.3 ms    |
|Tree64      |89.2        |77.5 (0.87x)|26.8        |275.0       |0.0 ms      |1356.0 ms   |
|BrickBVH    |93.2        |63.1 (0.68x)|33.2        |212.8       |0.0 ms      |486.2 ms    |
|ManhattanDF |131.8       |54.6 (0.41x)|34.3        |151.5       |809.9 ms    |372.1 ms    |
|EuclideanDF |82.7        |11.5 (0.14x)|45.2        |183.9       |944.7 ms    |419.7 ms    |

--------

**Scene**: Bistro 4k (975k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |28.2        |1.5 (0.05x) |436.4       |67.9        |165.0 ms    |109.9 ms    |
|MultiDDA    |80.8        |41.4 (0.51x)|107.4       |81.9        |200.9 ms    |146.8 ms    |
|XBrickMap   |113.8       |90.0 (0.79x)|27.9        |203.5       |212.5 ms    |210.4 ms    |
|ESVO        |69.1        |54.2 (0.78x)|71.1        |140.5       |0.0 ms      |1229.0 ms   |
|Tree64      |92.2        |77.9 (0.85x)|30.9        |230.9       |0.0 ms      |1970.4 ms   |
|BrickBVH    |90.8        |64.1 (0.71x)|50.4        |144.9       |0.0 ms      |1086.0 ms   |
|ManhattanDF |111.8       |43.5 (0.39x)|53.8        |115.4       |2657.5 ms   |1339.0 ms   |
|EuclideanDF |101.8       |28.4 (0.28x)|66.6        |103.2       |2835.4 ms   |1367.6 ms   |

--------

**Scene**: San Miguel 4k (681k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |33.0        |2.5 (0.08x) |354.3       |69.0        |149.6 ms    |56.9 ms     |
|MultiDDA    |97.9        |47.6 (0.49x)|66.2        |105.4       |170.5 ms    |57.5 ms     |
|XBrickMap   |110.8       |97.9 (0.88x)|25.2        |224.9       |173.5 ms    |187.3 ms    |
|ESVO        |69.2        |55.9 (0.81x)|68.4        |146.0       |0.0 ms      |805.5 ms    |
|Tree64      |92.6        |83.7 (0.90x)|28.8        |245.8       |1.0 ms      |1466.4 ms   |
|BrickBVH    |90.9        |64.8 (0.71x)|42.7        |171.4       |1.0 ms      |676.5 ms    |
|ManhattanDF |107.3       |56.8 (0.53x)|48.3        |133.1       |1047.8 ms   |777.0 ms    |
|EuclideanDF |99.1        |15.3 (0.15x)|60.9        |115.4       |1886.4 ms   |757.9 ms    |

--------

**Scene**: Forest Lake 2k (807k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |23.8        |23.3 (0.98x)|510.4       |67.4        |163.0 ms    |88.6 ms     |
|MultiDDA    |60.8        |31.4 (0.52x)|119.7       |101.2       |153.9 ms    |105.5 ms    |
|XBrickMap   |88.7        |71.6 (0.81x)|26.2        |276.9       |205.8 ms    |300.8 ms    |
|ESVO        |57.0        |43.5 (0.76x)|63.3        |188.5       |0.0 ms      |1252.3 ms   |
|Tree64      |76.1        |68.0 (0.89x)|27.4        |313.0       |0.0 ms      |1673.6 ms   |
|BrickBVH    |68.4        |46.7 (0.68x)|41.5        |238.0       |0.0 ms      |963.5 ms    |
|ManhattanDF |90.0        |46.8 (0.52x)|43.3        |179.7       |1405.5 ms   |770.5 ms    |
|EuclideanDF |80.6        |9.6 (0.12x) |56.7        |157.6       |1709.7 ms   |838.4 ms    |

--------

**Scene**: NY Highway I95 4k (574k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |24.3        |24.1 (0.99x)|512.0       |67.4        |151.2 ms    |52.9 ms     |
|MultiDDA    |42.6        |35.0 (0.82x)|312.1       |59.1        |167.4 ms    |76.9 ms     |
|XBrickMap   |114.0       |88.7 (0.78x)|31.3        |181.4       |120.7 ms    |157.0 ms    |
|ESVO        |136.8       |86.5 (0.63x)|33.2        |138.5       |0.0 ms      |727.9 ms    |
|Tree64      |168.3       |111.8 (0.66x)|16.0        |219.5       |0.0 ms      |1505.0 ms   |
|BrickBVH    |216.9       |153.7 (0.71x)|14.2        |173.7       |0.0 ms      |603.7 ms    |
|ManhattanDF |98.9        |53.3 (0.54x)|26.2        |249.7       |1154.5 ms   |930.8 ms    |
|EuclideanDF |29.0        |12.2 (0.42x)|29.0        |829.8       |2366.5 ms   |920.9 ms    |

---

Scene refs:

<img src="./docs/img/bench_sponza_1k.jpg" width="250">
<img src="./docs/img/bench_ecohouse_1k.jpg" width="250">
<img src="./docs/img/bench_bistro_4k.jpg" width="250">
<br>
<img src="./docs/img/bench_sanmiguel_4k.jpg" width="250">
<img src="./docs/img/bench_forestlake_4k.jpg" width="250">
<img src="./docs/img/bench_highwayi95_4k.jpg" width="250">

### Cursory overview and discussion
MultiDDA is a simple brickmap implementation that uses two nested DDA traversal loops to perform space skipping, one at 8³ scale and the other at voxel scale. Despite its simplicity, it performs surprisingly well relative to other techniques when considering only primary rays, and even though skips are limited to only one scale.

The poor performance with incoherent rays happens due to control-flow divergence when entering the inner traversal loop, since not all SIMD lanes will hit a brick at the same time as the others and thus become "stalled" for the duration of the inner loop. Other methods are not immune to incoherent rays due to poor memory access patterns, but divergence in this particular case makes a considerable difference.

I tried to mitigate divergence in two ways by following ideas from BVH traversal literature, without much success. The first attempt was to use wave intrinsics to postpone the inner traversal until enough lanes were active; the second, was to un-nest and pair the DDA loops, such that the inner DDA runs only after the brick DDA finds a hit, following control flow re-convergence.

---

XBrickMap attempts to improve on the idea of skipping through multiple grid levels by taking advantage of tiled bitmasks to perform more general hierarchical skips, using bitwise tests to identify sub-sections of empty voxels (as in 4³, 2³, 1³ cuboids). The bitmasks are also used to omit empty bricks within each sector (left-packing) for additional memory savings. Random access is made possible by using _popcnt_ instructions to count the number of preeceding entries at any given index.

Traversal was implemented using an algorithm similar to ray marching. At each iteration, voxel positions are computed by intersecting with the three back-facing cube planes, then the brickmap layers are queried in order to determine the step size. This is simpler and more efficient than DDA variants for stepping by arbitrary amounts, but requires workarounds such as biasing the intersection distances to prevent infinite backtracking due to floating-point limitations.

---

Ray marching through distance fields is a very simple and intuitive traversal method, but naively storing one distance value per voxel makes it impractical for bigger grids due to the associated memory and especially bandwidth costs, the latter which becomes problematic even for primary rays as they lose coherence the furthest they get from origin. Moreover, traversal is not considerably shorter than that of methods relying on space partitioning (~8%), and pre-processing is significantly more expansive than XBrickMap (~10x).

Memory costs can be reduced by lowering the field resolution, without much detriment to traversal efficiency. Following the theme of 64-bit masks, I picked 1:4 scale (one distance value per 64 voxels) combined with occupancy masks for the final implementation. The masks are queried whenever sampled distance values are zero.

---

I have not closely inspected the performance characteristics of ESVO, but believe the main reason for its underwhelming performance is the fact that it takes many extra iterations to ascend and descend the tree and advance positions by DDA. Given that it was primarily designed to encode smooth surfaces, other approaches for octree traversal [[C. Crassin et al, 2009]](https://inria.hal.science/inria-00345899/file/CNLE09.pdf), [[V. Havran, 1999]](https://www.researchgate.net/publication/245091894_A_Summary_of_Octree_Ray_Traversal_Algorithms) could offer better performance.

Despite, the throughput ratio for incoherent rays is comparable to others, possibly due to low memory bandwidth, given that nodes are very small (4 bytes each) and localized (building tree in depth first order results in a z-curve layout).

The traversal is a near 1:1 port of the original ESVO implementation, and only node encoding was changed: relative pointers are "backwards" to allow for trees to be built without intermediate buffers (i.e. parent nodes appear after their children), and far pointers are signaled by a reserved upper range rather than a dedicated bit.

---

The two main advantages wide 4³ trees have over standard octrees are the overall shallower trees and lower overhead per voxel. Based on rudimentary tests, I found that most scenes consume about half as much memory as ESVO.

Traversal was initially implemented using top-down recursive DDA over bitmasks of each node, which performed around 11-15% faster than ESVO for primary rays. Like in XBrickMap, I found that a ray marching algorithm was both simpler and faster, as it allows for coarse space skipping over bitmasks and is less prone to divergence. The complete implementation takes less than 100 LOC, but sadly still underperforms XBrickMap except for open and distant scenes.

All considered, trees have one interesting advantage over grids that is supporting arbitrary voxel scales and sub-divisions, as in dynamic details. Apart from LOD streaming, this could be useful for things like instancing and animations, which can be implemented by simply indirecting leafs to different subtrees.

---

BVHs have several advantages over all other methods. First, they are an industry standard with extensive research, and are quickly gaining hardware acceleration support. They are not limited to voxels and support traditional triangle-based geometry, transformations, and reasonably efficient updates.

TODO: expand

---

Rasterization without LODs is vastly inefficient with small voxels due to the huge amount of primitives generated. Greedy meshing does not seem to provide significant improvements (~15%) unless the number of materials is very limited or decoupled from the mesh, and voxels form long spans of columns or flat planes (~30%-20x). Although, even Sponza at 1k³ resolution with meshing in 62³ chunks and a single material is nearly in the order of a million quads.

TODO: provide actual numbers, adhoc impl does not integrate with benchmark runner because it has nothing to do with rt?

### Misc refs
- ["Efficient Sparse Voxel Octrees"](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees-analysis-extensions-and-implementation) - Samuli Laine, Tero Karras
- ["A Survey on Bounding Volume Hierarchies for Ray Tracing"](https://meistdan.github.io/publications/bvh_star/paper.pdf) - Daniel Meister, Shinji Ogaki, Carsten Benthin, Michael J. Doyle, Michael Guthe, Jiří Bittner
- [Real-time Ray tracing and Editing of Large Voxel Scenes](https://studenttheses.uu.nl/handle/20.500.12932/20460) - Thijs van Wingerden
- ["A high performance realtime CUDA voxel [brick map] path tracer"](https://github.com/stijnherfst/BrickMap) - stijnherfst
- ["Binary Greedy Meshing"](https://github.com/cgerikj/binary-greedy-meshing) - cgerikj

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
