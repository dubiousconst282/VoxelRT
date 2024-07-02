[require(glsl_spirv_1_6, SPV_EXT_descriptor_indexing, GL_EXT_nonuniform_qualifier)]
module Havk;

public struct ImageHandle<T> {
    uint DescriptorIndex;
};

#define HANDLE_TYPE(_T, _E, bdn) \
    [vk::binding(bdn, 0)] _T<_E> havk__DescriptorHeap__ ##_T ##_ ##_E []; \
    public extension ImageHandle<_T<_E>> {\
        public property _T<_E> Instance {\
            get { return havk__DescriptorHeap__ ##_T ##_ ##_E [DescriptorIndex];  }\
        }\
    }

HANDLE_TYPE(Texture2D, float, 0)
HANDLE_TYPE(Texture2D, float2, 0)
HANDLE_TYPE(Texture2D, float4, 0)
HANDLE_TYPE(Texture2D, uint, 0)
HANDLE_TYPE(Texture2D, uint2, 0)
HANDLE_TYPE(Texture2D, uint4, 0)
HANDLE_TYPE(TextureCube, float3, 0)

HANDLE_TYPE(RWTexture2D, float, 1)
HANDLE_TYPE(RWTexture2D, float2, 1)
HANDLE_TYPE(RWTexture2D, float4, 1)
HANDLE_TYPE(RWTexture2D, uint, 1)
HANDLE_TYPE(RWTexture2D, uint2, 1)
HANDLE_TYPE(RWTexture2D, uint4, 1)
HANDLE_TYPE(RWTexture2DArray, float3, 1)

public typealias TextureHandle2D<F = float4> = ImageHandle<Texture2D<F>>;
public typealias TextureHandle3D<F = float4> = ImageHandle<Texture3D<F>>;
public typealias ImageHandle2D<F = float4> = ImageHandle<RWTexture2D<F>>;
public typealias ImageHandle3D<F = float4> = ImageHandle<RWTexture3D<F>>;

public typealias TextureHandleCube<F = float4> = ImageHandle<TextureCube<F>>;
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