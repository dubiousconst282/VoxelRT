module Havk;

[vk::binding(0, 0)] __DynamicResource havk__TextureHeap[];
[vk::binding(1, 0)] __DynamicResource havk__RWTextureHeap[];

public struct ImageHandle<T> {
    uint DescriptorIndex;
}

__generic<E, Shape: __ITextureShape, let isArray: int, let isMS: int, let sampleCount: int, let access: int, let isShadow: int, let isCombined: int, let format: int>
public extension ImageHandle<__TextureImpl<E, Shape, isArray, isMS, sampleCount, access, isShadow, isCombined, format>> {
    typealias TextureType = __TextureImpl<E, Shape, isArray, isMS, sampleCount, access, isShadow, isCombined, format>;

    public property TextureType Instance {
        get { return (access != 0 ? havk__RWTextureHeap : havk__TextureHeap)[DescriptorIndex].as<TextureType>(); }
    }
}

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

__generic<T, let access : int, let isShadow : int, let isCombined : int, let format : int>
public extension __TextureImpl<T, __Shape3D, 0, 0, 0, access, isShadow, isCombined, format> {
    public property uint3 Size {
        get {
            uint width, height, depth;
            GetDimensions(width, height, depth);
            return uint3(width, height, depth);
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