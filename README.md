# VoxelRT
Voxel rendering experiments

## Benchmark of acceleration structures

### Implementations
- PlainDDA: Incremental DDA over flat grid
- MultiDDA: Incremental DDA over 2-level grid (8³ bricks)
- eXtendedBrickMap: Space skipping over 3-level grid + 4³ occupancy bitmasks (4³ sectors -> 8³ bricks)
- ManhattanDF/EuclideanDF: Space skipping over tiled 256³ distance fields at 1:4 resolution + 4³ occupancy bitmasks
- OctantDF: Space skipping over tiled 256³ 8-directional distance fields at 1:4 resolution + 4³ occupancy bitmasks
- ESVO: Port of "Efficient Sparse Voxel Octrees", no contours
- Tree64: Sparse voxel 4³-tree
- BrickBVH: Binary BVH with 8³ brick leafs + DDA (software impl)
- GreedyMesh: Rasterized greedy mesh

Maybe TODO:
- HybridBVH: BVH2 _or_ CWBVH with leafs made of XBrickMap(32³ sectors) _or_ Tree64 + partial splits

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
|PlainDDA    |31.3        |18.8 (0.60x)|391.7       |67.7        |125.6 ms    |53.8 ms     |
|MultiDDA    |146.3       |64.7 (0.44x)|58.1        |79.3        |148.0 ms    |68.2 ms     |
|XBrickMap   |167.8       |123.9 (0.74x)|19.2        |186.1       |114.2 ms    |145.6 ms    |
|ESVO        |94.3        |66.9 (0.71x)|46.9        |147.9       |0.0 ms      |614.2 ms    |
|Tree64      |156.7       |105.8 (0.68x)|19.4        |200.6       |0.0 ms      |1284.2 ms   |
|BrickBVH    |176.6       |104.5 (0.59x)|27.3        |123.0       |0.0 ms      |478.7 ms    |
|ManhattanDF |180.9       |89.2 (0.49x)|34.3        |106.2       |1157.8 ms   |570.2 ms    |
|EuclideanDF |178.0       |28.2 (0.16x)|39.7        |97.2        |1495.4 ms   |864.0 ms    |
|OctantDF    |122.7       |86.9 (0.71x)|31.1        |168.1       |455.2 ms    |634.4 ms    |

--------

**Scene**: Eco House 2k (448k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |27.2        |3.1 (0.11x) |434.7       |69.7        |132.8 ms    |33.6 ms     |
|MultiDDA    |72.4        |34.8 (0.48x)|112.6       |88.5        |145.2 ms    |34.3 ms     |
|XBrickMap   |99.0        |77.8 (0.79x)|27.8        |235.6       |157.3 ms    |110.0 ms    |
|ESVO        |68.8        |54.8 (0.80x)|59.9        |169.6       |0.0 ms      |632.8 ms    |
|Tree64      |89.9        |79.7 (0.89x)|26.8        |274.6       |0.0 ms      |1246.3 ms   |
|BrickBVH    |93.7        |65.2 (0.70x)|33.2        |210.6       |0.0 ms      |468.5 ms    |
|ManhattanDF |131.8       |55.5 (0.42x)|34.3        |149.8       |753.8 ms    |354.0 ms    |
|EuclideanDF |77.7        |10.7 (0.14x)|45.2        |196.1       |1061.8 ms   |390.6 ms    |
|OctantDF    |103.5       |72.6 (0.70x)|28.4        |225.2       |320.6 ms    |399.2 ms    |

--------

**Scene**: Bistro 4k (975k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |28.5        |1.5 (0.05x) |436.4       |67.9        |158.2 ms    |102.7 ms    |
|MultiDDA    |80.7        |41.5 (0.51x)|107.4       |81.7        |154.4 ms    |99.2 ms     |
|XBrickMap   |112.0       |88.4 (0.79x)|27.9        |202.2       |254.3 ms    |196.9 ms    |
|ESVO        |69.7        |55.9 (0.80x)|71.1        |140.3       |0.0 ms      |1163.1 ms   |
|Tree64      |91.5        |79.8 (0.87x)|30.9        |230.4       |0.0 ms      |1859.8 ms   |
|BrickBVH    |90.4        |66.4 (0.73x)|50.4        |144.2       |0.0 ms      |1038.5 ms   |
|ManhattanDF |110.8       |42.7 (0.39x)|53.8        |115.2       |2879.0 ms   |1284.0 ms   |
|EuclideanDF |102.1       |19.2 (0.19x)|66.6        |103.9       |2923.7 ms   |1311.5 ms   |
|OctantDF    |88.0        |25.7 (0.29x)|41.4        |190.4       |1101.2 ms   |1421.8 ms   |

--------

**Scene**: San Miguel 4k (681k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |33.8        |2.5 (0.07x) |354.3       |68.9        |168.4 ms    |69.4 ms     |
|MultiDDA    |98.2        |48.3 (0.49x)|66.2        |105.4       |137.3 ms    |55.7 ms     |
|XBrickMap   |111.3       |97.4 (0.88x)|25.2        |224.6       |179.3 ms    |177.1 ms    |
|ESVO        |69.4        |57.7 (0.83x)|68.4        |146.2       |0.0 ms      |752.9 ms    |
|Tree64      |91.9        |85.4 (0.93x)|28.8        |245.2       |0.7 ms      |1389.5 ms   |
|BrickBVH    |90.3        |67.1 (0.74x)|42.7        |170.9       |0.0 ms      |631.7 ms    |
|ManhattanDF |106.9       |52.8 (0.49x)|48.3        |131.7       |1183.3 ms   |696.3 ms    |
|EuclideanDF |100.9       |15.3 (0.15x)|60.9        |114.8       |1839.4 ms   |831.2 ms    |
|OctantDF    |84.2        |66.1 (0.79x)|37.7        |216.4       |595.1 ms    |709.3 ms    |

--------

**Scene**: Forest Lake 2k (807k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |24.8        |24.1 (0.97x)|510.4       |67.4        |157.2 ms    |77.7 ms     |
|MultiDDA    |61.0        |31.7 (0.52x)|119.7       |100.3       |144.5 ms    |93.5 ms     |
|XBrickMap   |91.6        |74.6 (0.81x)|26.2        |275.3       |239.0 ms    |213.9 ms    |
|ESVO        |58.8        |45.0 (0.76x)|63.3        |188.5       |0.0 ms      |1120.5 ms   |
|Tree64      |78.3        |70.2 (0.90x)|27.4        |313.3       |0.0 ms      |1587.7 ms   |
|BrickBVH    |69.4        |47.8 (0.69x)|41.5        |237.9       |0.0 ms      |892.5 ms    |
|ManhattanDF |90.4        |46.4 (0.51x)|43.3        |178.7       |1497.6 ms   |750.3 ms    |
|EuclideanDF |79.4        |9.3 (0.12x) |56.7        |160.1       |1823.8 ms   |773.6 ms    |
|OctantDF    |72.1        |50.2 (0.70x)|35.0        |281.1       |607.3 ms    |793.6 ms    |

--------

**Scene**: NY Highway I95 4k (574k * 8³ voxels)
|Method      |Mrays/s     |Mrays/s PT1 |Iters/ray   |Clocks/iter |GPU sync    |CPU sync    |
|------------|------------|------------|------------|------------|------------|------------
|PlainDDA    |24.4        |24.2 (0.99x)|512.0       |67.4        |137.6 ms    |44.6 ms     |
|MultiDDA    |43.2        |35.5 (0.82x)|312.1       |58.9        |134.0 ms    |43.3 ms     |
|XBrickMap   |115.2       |88.5 (0.77x)|31.3        |181.4       |185.9 ms    |123.0 ms    |
|ESVO        |140.2       |88.9 (0.63x)|33.2        |139.7       |0.0 ms      |660.1 ms    |
|Tree64      |170.0       |114.2 (0.67x)|16.0        |220.3       |0.0 ms      |1327.2 ms   |
|BrickBVH    |216.2       |157.4 (0.73x)|14.2        |177.8       |0.0 ms      |543.7 ms    |
|ManhattanDF |98.0        |52.7 (0.54x)|26.2        |248.9       |1022.1 ms   |828.5 ms    |
|EuclideanDF |25.1        |10.2 (0.41x)|29.0        |971.2       |2366.9 ms   |928.4 ms    |
|OctantDF    |130.5       |87.5 (0.67x)|24.9        |200.7       |729.2 ms    |882.3 ms    |

---

Scene refs:

<p float="left">
    <img src="./docs/img/bench_sponza_1k.jpg" width="250">
    <img src="./docs/img/bench_ecohouse_1k.jpg" width="250">
    <img src="./docs/img/bench_bistro_4k.jpg" width="250">
    <br>
    <img src="./docs/img/bench_sanmiguel_4k.jpg" width="250">
    <img src="./docs/img/bench_forestlake_4k.jpg" width="250">
    <img src="./docs/img/bench_highwayi95_4k.jpg" width="250">
</p>

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

Creating separate fields for each ray octant (known as "directional" or "anisotropic" distance fields) reduces the overall number of iterations by ~20-25%, and increases throughput for incoherent rays by ~30-50%. However, it still performs considerably worse than space-partitioning methods.

---

I have not closely inspected the performance characteristics of ESVO, but believe the main reason for its underwhelming performance is the fact that it takes many extra iterations to ascend and descend the tree and advance positions by DDA. Given that it was primarily designed to encode smooth surfaces, other approaches for octree traversal [[C. Crassin et al, 2009]](https://inria.hal.science/inria-00345899/file/CNLE09.pdf), [[V. Havran, 1999]](https://www.researchgate.net/publication/245091894_A_Summary_of_Octree_Ray_Traversal_Algorithms) could offer better performance.

Despite, the throughput ratio for incoherent rays is comparable to others, possibly due to low memory bandwidth, given that nodes are very small (4 bytes each) and localized (building tree in depth first order results in a z-curve layout).

The traversal is a near 1:1 port of the original ESVO implementation, and only node encoding was changed: relative pointers are "backwards" to allow for trees to be built without intermediate buffers (i.e. parent nodes appear after their children), and far pointers are signaled by a reserved upper range rather than a dedicated bit.

---

The two main advantages wide 4³ trees have over standard octrees are the overall shallower trees and lower overhead per voxel. Based on rudimentary tests, I found that most scenes consume about half as much memory as ESVO.

Traversal was initially implemented using top-down recursive DDA over bitmasks of each node, which performed around 11-15% faster than ESVO for primary rays. Like in XBrickMap, I found that a ray marching algorithm was both simpler and faster, as it allows for coarse space skipping over bitmasks and is less prone to divergence. The complete implementation takes less than 100 LOC, but sadly still underperforms XBrickMap except for open and distant scenes.

All considered, trees have one interesting advantage over grids that is supporting arbitrary voxel scales and sub-divisions, as in dynamic details. Apart from LOD streaming, this could be useful for things like instancing and animations, which can be implemented by simply indirecting leafs to different subtrees.

---

BVHs have several advantages over all other methods. First, they are industry standard and have extensive research, and are quickly gaining hardware acceleration support. They are not limited to voxels and support traditional triangle-based geometry, transformations, and reasonably efficient updates.

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
