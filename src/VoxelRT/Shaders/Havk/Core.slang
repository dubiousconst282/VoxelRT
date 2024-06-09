[require(glsl_spirv_1_6, SPV_EXT_descriptor_indexing, GL_EXT_nonuniform_qualifier)]
module Havk;

[vk::binding(0, 0)] Texture2D havk__DescriptorHeap_T2D[];
[vk::binding(0, 0)] Texture2D<uint2> havk__DescriptorHeap_T2D_u2[];
[vk::binding(0, 0)] TextureCube<float3> havk__DescriptorHeap_T_C[];
[vk::binding(1, 0)] RWTexture2D havk__DescriptorHeap_RWT2D[];
[vk::binding(1, 0)] RWTexture2D<uint> havk__DescriptorHeap_RWT2D_u1[];
[vk::binding(1, 0)] RWTexture2DArray<float3> havk__DescriptorHeap_RWT2D_A[];

public struct TextureHandle<T> {
    uint DescriptorIndex;
    
    public property T Instance {
        get {
            // TODO: maybe do some SPIRV patching to add dynamic descriptor bindings
            // https://github.com/shader-slang/slang/issues/4150
            if (T is Texture2D<uint2>) {
                return bit_cast<T>(havk__DescriptorHeap_T2D_u2[DescriptorIndex]);
            }
            if (T is TextureCube<float3>) {
                return bit_cast<T>(havk__DescriptorHeap_T_C[DescriptorIndex]);
            }
            return bit_cast<T>(havk__DescriptorHeap_T2D[DescriptorIndex]);
        }
    }
    public property T NonUniformInstance {
        get { return bit_cast<T>(havk__DescriptorHeap_T2D[NonUniformResourceIndex(DescriptorIndex)]); }
    }
};
public struct ImageHandle<T> {
    uint DescriptorIndex;
    
    public property T Instance {
        get {
            if (T is RWTexture2D<uint>) {
                return bit_cast<T>(havk__DescriptorHeap_RWT2D_u1[DescriptorIndex]);
            }
            if (T is RWTexture2DArray<float3>) {
                return bit_cast<T>(havk__DescriptorHeap_RWT2D_A[DescriptorIndex]);
            }
            return bit_cast<T>(havk__DescriptorHeap_RWT2D[DescriptorIndex]);
        }
    }
    public property T NonUniformInstance {
        get { return bit_cast<T>(havk__DescriptorHeap_RWT2D[NonUniformResourceIndex(DescriptorIndex)]); }
    }
};

public typealias TextureHandle2D<F = float4> = TextureHandle<Texture2D<F>>;
public typealias TextureHandle3D<F = float4> = TextureHandle<Texture3D<F>>;
public typealias ImageHandle2D<F = float4> = ImageHandle<RWTexture2D<F>>;
public typealias ImageHandle3D<F = float4> = ImageHandle<RWTexture3D<F>>;

public typealias TextureHandleCube<F = float4> = TextureHandle<TextureCube<F>>;
public typealias ImageHandleArray2D<F = float4> = ImageHandle<RWTexture2DArray<F>>;

__generic<T, let access : int, let isShadow : int, let isCombined : int, let format : int>
public extension __TextureImpl<T, __Shape2D, 0, 0, 0, access, isShadow, isCombined, format> {
    public property uint2 Size {
        get {
            uint width, height;
            GetDimensions(width, height);
            return uint2(width, height);
        }
    }
}
__generic<T, let access : int, let isShadow : int, let isCombined : int, let format : int>
public extension __TextureImpl<T, __Shape2D, 1, 0, 0, access, isShadow, isCombined, format> {
    public property uint3 Size {
        get {
            uint width, height, layers;
            GetDimensions(width, height, layers);
            return uint3(width, height, layers);
        }
    }
}

/*
__generic<T>
__prefix T operator *(TextureHandle<T> value) {
    return value.NonUniformInstance;
}
void Foo(TextureRef2D tex) {
    float4 d = (*tex).Load(int3(0));
}*/

// These would ideally be enums but slang reflection doesn't support getting values yet
public class MagFilter { public static const int Nearest = 0, Linear = 1; };
public class MinFilter { public static const int Nearest = 0, Linear = 1; };
public class MipFilter { public static const int Nearest = 0, Linear = 1, Anisotropic = 2; };

public class WrapMode { public static const int Repeat = 0, ClampToEdge = 1, MirroredRepeat = 2, MirrorClampToEdge = 3; };
public class CompareOp { public static const int None = 0, Eq = 1, Ne = 2, Gt = 3, Lt = 4, Ge = 5, Le = 6; };

// Embeds an immutable sampler to the binding where this attribute is applied.
[__AttributeUsage(_AttributeTargets.Var)]
public struct SamplerDescAttribute {
    int magFilter = MagFilter.Linear;
    int minFilter = MinFilter.Linear;
    int mipFilter = MipFilter.Linear;
    int wrap = WrapMode.Repeat;
};

// Not implemented
[__AttributeUsage(_AttributeTargets.Var)]
public struct ShadowSamplerDescAttribute {
    int magFilter = MagFilter.Linear;
    int minFilter = MinFilter.Linear;
    int mipFilter = MipFilter.Linear;
    int wrap = WrapMode.Repeat;
    int op = CompareOp.Gt;
};