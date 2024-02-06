#pragma once

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <functional>

#include <glad/glad.h>
#include <glm/glm.hpp>

// Basic OpenGL object wrappers for quick prototyping.
namespace ogl {

inline void DebugMessage(GLenum type, GLenum severity, const char* fmt, ...) {
    char msg[1024];

    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, type, clock(), severity, -1, msg);
}

inline void EnableDebugCallback() {
    static const struct {
        GLenum Id;
        const char* Text;
    } EnumStrings[] = {
        { GL_DEBUG_TYPE_ERROR, "Error" },
        { GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR, "Deprecated Behavior" },
        { GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR, "Undefined Behavior" },
        { GL_DEBUG_TYPE_PORTABILITY, "Portability" },
        { GL_DEBUG_TYPE_PERFORMANCE, "Performance" },
        { GL_DEBUG_TYPE_MARKER, "Marker" },
        { GL_DEBUG_TYPE_PUSH_GROUP, "Push Group" },
        { GL_DEBUG_TYPE_POP_GROUP, "Pop Group" },
        { GL_DEBUG_TYPE_OTHER, "Other" },

        { GL_DEBUG_SEVERITY_HIGH, "High" },
        { GL_DEBUG_SEVERITY_MEDIUM, "Medium" },
        { GL_DEBUG_SEVERITY_LOW, "Low" },
        { GL_DEBUG_SEVERITY_NOTIFICATION, "Info" },
    };
    static const auto ToStr = [](GLenum id) {
        for (auto& entry : EnumStrings) {
            if (entry.Id == id) return entry.Text;
        }
        return "?";
    };

    glDebugMessageCallback(
        [](GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam) {
            fprintf(stderr, "GL [%s, %s]: %s\n", ToStr(type), ToStr(severity), message);
            fflush(stderr);
        },
        nullptr);
    glEnable(GL_DEBUG_OUTPUT);

    DebugMessage(GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION, "Driver: %s %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
}

struct Object {
    Object() { }

    Object(const Object&) = delete;             // non construction-copyable
    Object& operator=(const Object&) = delete;  // non copyable
};

struct Buffer : public Object {
    GLuint Handle;
    size_t Size;

    Buffer(size_t numBytes, GLbitfield flags) {
        glCreateBuffers(1, &Handle);
        glNamedBufferStorage(Handle, numBytes, nullptr, flags);

        Size = numBytes;
    }
    ~Buffer() { glDeleteBuffers(1, &Handle); }

    template<typename T = uint8_t>
    T* Map(GLbitfield access, size_t offset = 0, size_t length = 0) {
        if (length == 0) length = Size;
        return (T*)glMapNamedBufferRange(Handle, offset, length, access);
    }
    void Unmap() { glUnmapNamedBuffer(Handle); }
    
    void FlushMappedRange(size_t offset, size_t length) { glFlushMappedNamedBufferRange(Handle, offset, length); }
};

struct BufferSpan {
    const Buffer& Target;
    size_t Offset, Size;

    BufferSpan(const ogl::Buffer& target) : Target(target) {
        Offset = 0;
        Size = target.Size;
    }
    BufferSpan(const ogl::Buffer& target, size_t offset, size_t size) : Target(target) {
        if (offset + size > target.Size) {
            throw std::range_error("Range outside buffer bounds");
        }
        Offset = offset;
        Size = size;
    }
};

struct Texture : public Object {
    GLuint Handle;

    Texture(GLenum target) {
        glCreateTextures(target, 1, &Handle);
        SetMipMode(GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR);
        SetWrapMode(GL_REPEAT);
    }
    ~Texture() { glDeleteTextures(1, &Handle); }

    void SetMipMode(GLenum magFilter, GLenum minFilter) {
        glTextureParameteri(Handle, GL_TEXTURE_MAG_FILTER, magFilter);
        glTextureParameteri(Handle, GL_TEXTURE_MIN_FILTER, minFilter);
    }
    void SetWrapMode(GLenum mode) {
        glTextureParameteri(Handle, GL_TEXTURE_WRAP_S, mode);
        glTextureParameteri(Handle, GL_TEXTURE_WRAP_T, mode);
        glTextureParameteri(Handle, GL_TEXTURE_WRAP_R, mode);
    }
};

struct Texture2D : public Texture {
    uint32_t Width, Height, MipLevels;

    Texture2D(uint32_t width, uint32_t height, uint32_t mipLevels, GLuint internalFmt) : Texture(GL_TEXTURE_2D) {
        Width = width;
        Height = height;
        MipLevels = mipLevels;

        glTextureStorage2D(Handle, mipLevels, internalFmt, width, height);
    }
    void SetPixels(GLenum format, GLenum type, const void* pixels, uint32_t stride = 0) {
        if (stride != 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);

        glTextureSubImage2D(Handle, 0, 0, 0, Width, Height, format, type, pixels);
        glGenerateTextureMipmap(Handle);

        if (stride != 0) glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    // Copies texture data to the specified buffer asynchronously.
    // Mapping the buffer will block the caller thread until the data is ready.
    void GetPixelsAsync(GLenum format, GLenum type, const BufferSpan& buffer,
                        glm::uvec2 offset = { 0, 0 }, glm::uvec2 size = { UINT_MAX, UINT_MAX },
                        uint32_t mipLevel = 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer.Target.Handle);
        glGetTextureSubImage(
            Handle, 
            mipLevel, offset.x, offset.y, 0, 
            std::min(size.x, Width), std::min(size.y, Height), 1,
            format, type, buffer.Size, (void*)buffer.Offset);
    }
};

struct Texture3D : public Texture {
    uint32_t Width, Height, Depth, MipLevels;

    Texture3D(uint32_t width, uint32_t height, uint32_t depth, uint32_t mipLevels, GLuint internalFmt) : Texture(GL_TEXTURE_3D) {
        Width = width;
        Height = height;
        Depth = depth;
        MipLevels = mipLevels;

        glTextureStorage3D(Handle, mipLevels, internalFmt, width, height, depth);
    }

    void SetPixels(GLenum format, GLenum type, const void* pixels, uint32_t strideX = 0, uint32_t strideZ = 0, int32_t mipLevel = 0,
                   glm::uvec3 offset = { 0, 0, 0 }, glm::uvec3 size = { UINT_MAX, UINT_MAX, UINT_MAX }) {
        if (strideX != 0 || strideZ != 0) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, strideX);
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, strideZ);
        }
        size.x = std::min(size.x, Width >> mipLevel);
        size.y = std::min(size.y, Height >> mipLevel);
        size.z = std::min(size.z, Depth >> mipLevel);

        glTextureSubImage3D(
            Handle, 
            mipLevel, offset.x, offset.y, offset.z, size.x, size.y, size.z,
            format, type, pixels);
            
        if (strideX != 0 || strideZ != 0) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
        }
    }
    // Copies texture data to the specified buffer asynchronously.
    // Mapping the buffer will block the caller thread until the data is ready.
    void GetPixelsAsync(GLenum format, GLenum type, const BufferSpan& buffer,
                        glm::uvec3 offset = { 0, 0, 0 }, glm::uvec3 size = { UINT_MAX, UINT_MAX, UINT_MAX },
                        uint32_t mipLevel = 0) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer.Target.Handle);
        glGetTextureSubImage(
            Handle, 
            mipLevel, offset.x, offset.y, offset.z, 
            std::min(size.x, Width), std::min(size.y, Height), std::min(size.z, Depth),
            format, type, buffer.Size, (void*)buffer.Offset);
    }
};

struct VertexLayout {
    struct AttribDesc {
        uint16_t BufferId = 0;      // Not implemented
        uint16_t Offset = 0, Count = 1;
        GLenum Type = GL_FLOAT;
        char Name[64];              // Shader attribute name
    };

    std::vector<AttribDesc> Attribs;
    uint32_t Stride;
};

struct Shader : public Object {
private:
    static const uint32_t MaxBoundResources = 16;
    struct UniformLocInfo {
        enum UniformKind { Data, Sampler, Image, Buffer };

        GLint Location;
        GLuint BindingId : 24;
        UniformKind Kind : 3;
    };

    struct string_hash {
        using is_transparent = void;
        std::size_t operator()(std::string_view str) const { return std::hash<std::string_view>{}(str); }
    };
    std::unordered_map<std::string, UniformLocInfo, string_hash, std::equal_to<>> _uniforms;
    GLuint _boundTextures[MaxBoundResources]{0};
    GLuint _boundImages[MaxBoundResources]{0};
    GLuint _numTextureBindings = 0, _numImageBindings = 0;

    GLuint _boundBuffers[MaxBoundResources]{0};
    GLintptr _boundBufferOffsets[MaxBoundResources]{0};
    GLintptr _boundBufferSizes[MaxBoundResources]{0};
    GLuint _numBufferBindings = 0;

    uint32_t _vertexStride = 0;
    GLuint _vaoHandle = 0;

public:
    GLuint Handle;

    Shader() {
        Handle = glCreateProgram();
    }
    ~Shader() {
        if (Handle != 0) {
            glDeleteProgram(Handle);
            Handle = 0;
        }
        if (_vaoHandle != 0) {
            glDeleteVertexArrays(1, &_vaoHandle);
            _vaoHandle = 0;
        }
    }

    void DrawIndexedTriangles(const BufferSpan& vbo, const BufferSpan& ebo, GLenum indexType) {
        uint32_t indexSize =
            indexType == GL_UNSIGNED_BYTE ? 1 :
            indexType == GL_UNSIGNED_SHORT ? 2 :
            indexType == GL_UNSIGNED_INT ? 4 :
            throw std::invalid_argument("Index type must be either U8, U16, or U32.");

        BindState();

        glVertexArrayVertexBuffer(_vaoHandle, 0, vbo.Target.Handle, vbo.Offset, _vertexStride);
        glVertexArrayElementBuffer(_vaoHandle, ebo.Target.Handle);

        glDrawElements(GL_TRIANGLES, ebo.Size / indexSize, indexType, (void*)(size_t)ebo.Offset);
    }
    void DrawTriangles(const BufferSpan& vbo) {
        BindState();

        glVertexArrayVertexBuffer(_vaoHandle, 0, vbo.Target.Handle, vbo.Offset, _vertexStride);
        glDrawArrays(GL_TRIANGLES, 0, vbo.Size / _vertexStride);
    }
    void DispatchFullscreen() {
        if (_vaoHandle == 0) {
            glCreateVertexArrays(1, &_vaoHandle);
        }
        BindState();
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    void DispatchCompute(uint32_t numGroupsX, uint32_t numGroupsY, uint32_t numGroupsZ) {
        BindState();
        glDispatchCompute(numGroupsX, numGroupsY, numGroupsZ);
    }

    void BindState() {
        glUseProgram(Handle);

        if (_vaoHandle != 0) {
            glBindVertexArray(_vaoHandle);
        }
        glBindTextures(0, _numTextureBindings, _boundTextures);
        glBindImageTextures(0, _numImageBindings, _boundImages);
        glBindBuffersRange(GL_SHADER_STORAGE_BUFFER, 0, _numBufferBindings, _boundBuffers, _boundBufferOffsets, _boundBufferSizes);
    }
    // Bind texture or image
    //
    // NOTE: for image bindings, the texture internal format *must* match the shader
    // binding layout, otherwise accesses may silently fail on some vendors. (Intel)
    void SetUniform(std::string_view name, const Texture& tex) {
        auto itr = _uniforms.find(name);
        if (itr == _uniforms.end()) return;

        GLuint id = itr->second.BindingId;

        switch (itr->second.Kind) {
            case UniformLocInfo::Sampler: _boundTextures[id] = tex.Handle; break;
            case UniformLocInfo::Image: _boundImages[id] = tex.Handle; break;
            default: throw std::domain_error("Can only bind texture to sampler or image uniforms.");
        }
    }
    // Bind SSBO
    void SetUniform(std::string_view name, const BufferSpan& buffer) {
        auto itr = _uniforms.find(name);
        if (itr == _uniforms.end()) return;

        GLuint id = itr->second.BindingId;

        switch (itr->second.Kind) {
            case UniformLocInfo::Buffer:
                _boundBuffers[id] = buffer.Target.Handle;
                _boundBufferOffsets[id] = (GLintptr)buffer.Offset;
                _boundBufferSizes[id] = (GLintptr)buffer.Size;
                break;
            default: throw std::domain_error("Can only bind buffer to buffer uniform.");
        }
    }
    // Bind float uniform
    void SetUniform(std::string_view name, const float* values, uint32_t count) {
        GLint loc = glGetUniformLocation(Handle, name.data());

        switch (count) {
            case 1: glProgramUniform1f(Handle, loc, *values); break;
            case 2: glProgramUniform2fv(Handle, loc, 1, values); break;
            case 3: glProgramUniform3fv(Handle, loc, 1, values); break;
            case 4: glProgramUniform4fv(Handle, loc, 1, values); break;
            case 16: glProgramUniformMatrix4fv(Handle, loc, 1, false, values); break;
            default: throw std::exception();
        }
    }
    void SetUniform(std::string_view name, const int* values, uint32_t count) {
        GLint loc = glGetUniformLocation(Handle, name.data());

        switch (count) {
            case 1: glProgramUniform1i(Handle, loc, *values); break;
            case 2: glProgramUniform2iv(Handle, loc, 1, values); break;
            case 3: glProgramUniform3iv(Handle, loc, 1, values); break;
            case 4: glProgramUniform4iv(Handle, loc, 1, values); break;
            default: throw std::exception();
        }
    }

    void Attach(GLuint type, std::string_view source) {
        GLuint shaderId = glCreateShader(type);

        const char* pSource = source.data();
        glShaderSource(shaderId, 1, &pSource, nullptr);

        glCompileShader(shaderId);

        GLint status;
        glGetShaderiv(shaderId, GL_COMPILE_STATUS, &status);

        if (status != GL_TRUE) {
            GLchar infoStr[1024];
            GLsizei infoLen;
            glGetShaderInfoLog(shaderId, sizeof(infoStr), &infoLen, infoStr);

            throw std::runtime_error(std::string("Failed to attach shader to program: ") + infoStr);
        }
        glAttachShader(Handle, shaderId);
    }

    void Link() {
        glLinkProgram(Handle);
        DeleteAttachedShaders();

        GLint status;
        glGetProgramiv(Handle, GL_LINK_STATUS, &status);

        if (status != GL_TRUE) {
            GLchar infoStr[1024];
            GLsizei infoLen;
            glGetProgramInfoLog(Handle, sizeof(infoStr), &infoLen, infoStr);

            throw std::runtime_error(std::string("Failed to link shader program: ") + infoStr);
        }

        // Assign uniform locations
        _uniforms.clear();
        _numTextureBindings = _numImageBindings = _numBufferBindings = 0;

        GLint numActiveUniforms;
        glGetProgramInterfaceiv(Handle, GL_UNIFORM, GL_ACTIVE_RESOURCES, &numActiveUniforms);

        for (GLuint i = 0; i < numActiveUniforms; i++) {
            GLenum props[3] = { GL_NAME_LENGTH, GL_TYPE, GL_LOCATION };
            GLuint res[3];
            glGetProgramResourceiv(Handle, GL_UNIFORM, i, std::size(props), props, std::size(res), nullptr, (GLint*)res);

            std::string name((GLuint)res[0] - 1, '\0');
            glGetProgramResourceName(Handle, GL_UNIFORM, i, name.capacity() + 1, nullptr, name.data());

            UniformLocInfo info = { .Location = (GLint)res[2] };

            if (IsSamplerType(res[1])) {
                info.Kind = UniformLocInfo::Sampler;
                info.BindingId = _numTextureBindings++;
                glProgramUniform1i(Handle, info.Location, info.BindingId);
            } else if (IsImageType(res[1])) {
                info.Kind = UniformLocInfo::Image;
                info.BindingId = _numImageBindings++;
                glProgramUniform1i(Handle, info.Location, info.BindingId);
            } else {
                info.Kind = UniformLocInfo::Data;
            }
            _uniforms.insert({ name, info });
        }

        glGetProgramInterfaceiv(Handle, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &numActiveUniforms);

        for (GLuint i = 0; i < numActiveUniforms; i++) {
            GLenum props[1] = { GL_NAME_LENGTH };
            GLuint res[1];
            glGetProgramResourceiv(Handle, GL_SHADER_STORAGE_BLOCK, i, std::size(props), props, std::size(res), nullptr, (GLint*)res);

            std::string name((GLuint)res[0] - 1, '\0');
            glGetProgramResourceName(Handle, GL_SHADER_STORAGE_BLOCK, i, name.capacity() + 1, nullptr, name.data());

            UniformLocInfo info = { .Location = (GLint)i, .BindingId = _numBufferBindings++, .Kind = UniformLocInfo::Buffer };
            glShaderStorageBlockBinding(Handle, info.Location, info.BindingId);
            _uniforms.insert({ name, info });
        }
    }

    void SetVertexLayout(const VertexLayout& layout) {
        if (_vaoHandle == 0) {
            glCreateVertexArrays(1, &_vaoHandle);
        }

        for (auto& attrib : layout.Attribs) {
            GLint location = glGetAttribLocation(Handle, attrib.Name);

            if (location < 0) {
                DebugMessage(GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION, "Skipping unused vertex attribute '%s' in shader program #%d.", attrib.Name, Handle);
                continue;
            }

            glEnableVertexArrayAttrib(_vaoHandle, location);
            glVertexArrayAttribBinding(_vaoHandle, location, 0);

            GLenum type;
            glGetActiveAttrib(Handle, location, 0, nullptr, nullptr, &type, nullptr);

            if (!IsIntegerType(type)) {
                glVertexArrayAttribFormat(_vaoHandle, location, attrib.Count, (GLenum)attrib.Type, true, attrib.Offset);
            } else {
                glVertexArrayAttribIFormat(_vaoHandle, location, attrib.Count, (GLenum)attrib.Type, attrib.Offset);
            }
        }
        _vertexStride = layout.Stride;
    }

    static bool IsIntegerType(GLenum type) {
        return type == GL_INT || type == GL_INT_VEC2 || type == GL_INT_VEC3 || type == GL_INT_VEC4 || 
               type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_VEC2 || type == GL_UNSIGNED_INT_VEC3 || type == GL_UNSIGNED_INT_VEC4;
    }
    static bool IsSamplerType(GLenum type) { return ContainsType(s_SamplerTypes, type); }
    static bool IsImageType(GLenum type) { return ContainsType(s_ImageTypes, type); }

private:
    void DeleteAttachedShaders() {
        GLuint shaders[16];
        GLsizei count;

        glGetAttachedShaders(Handle, 16, &count, shaders);

        for (GLsizei i = 0; i < count; i++) {
            glDetachShader(Handle, shaders[i]);
            glDeleteShader(shaders[i]);
        }
    }

    static bool ContainsType(const GLenum* types, GLenum type) {
        for (; *types != 0; types++) {
            if (*types == type) return true;
        }
        return false;
    }

    // Generated from https://registry.khronos.org/OpenGL-Refpages/gl4/html/glGetActiveUniform.xhtml
    // clang-format off
    static inline const GLenum s_SamplerTypes[] = {
        GL_SAMPLER_1D, GL_SAMPLER_2D, GL_SAMPLER_3D, GL_SAMPLER_CUBE, GL_SAMPLER_1D_SHADOW, 
        GL_SAMPLER_2D_SHADOW, GL_SAMPLER_1D_ARRAY, GL_SAMPLER_2D_ARRAY, GL_SAMPLER_1D_ARRAY_SHADOW, 
        GL_SAMPLER_2D_ARRAY_SHADOW, GL_SAMPLER_2D_MULTISAMPLE, GL_SAMPLER_2D_MULTISAMPLE_ARRAY, 
        GL_SAMPLER_CUBE_SHADOW, GL_SAMPLER_BUFFER, GL_SAMPLER_2D_RECT, GL_SAMPLER_2D_RECT_SHADOW, 
        GL_INT_SAMPLER_1D, GL_INT_SAMPLER_2D, GL_INT_SAMPLER_3D, GL_INT_SAMPLER_CUBE, GL_INT_SAMPLER_1D_ARRAY, 
        GL_INT_SAMPLER_2D_ARRAY, GL_INT_SAMPLER_2D_MULTISAMPLE, GL_INT_SAMPLER_2D_MULTISAMPLE_ARRAY, 
        GL_INT_SAMPLER_BUFFER, GL_INT_SAMPLER_2D_RECT, GL_UNSIGNED_INT_SAMPLER_1D, GL_UNSIGNED_INT_SAMPLER_2D, 
        GL_UNSIGNED_INT_SAMPLER_3D, GL_UNSIGNED_INT_SAMPLER_CUBE, GL_UNSIGNED_INT_SAMPLER_1D_ARRAY, 
        GL_UNSIGNED_INT_SAMPLER_2D_ARRAY, GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE, GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ARRAY, 
        GL_UNSIGNED_INT_SAMPLER_BUFFER, GL_UNSIGNED_INT_SAMPLER_2D_RECT,
        0
    };
    // clang-format off
    static inline const GLenum s_ImageTypes[] = {
        GL_IMAGE_1D, GL_IMAGE_2D, GL_IMAGE_3D, GL_IMAGE_2D_RECT, GL_IMAGE_CUBE, GL_IMAGE_BUFFER, 
        GL_IMAGE_1D_ARRAY, GL_IMAGE_2D_ARRAY, GL_IMAGE_2D_MULTISAMPLE, GL_IMAGE_2D_MULTISAMPLE_ARRAY, 
        GL_INT_IMAGE_1D, GL_INT_IMAGE_2D, GL_INT_IMAGE_3D, GL_INT_IMAGE_2D_RECT, GL_INT_IMAGE_CUBE, 
        GL_INT_IMAGE_BUFFER, GL_INT_IMAGE_1D_ARRAY, GL_INT_IMAGE_2D_ARRAY, GL_INT_IMAGE_2D_MULTISAMPLE, 
        GL_INT_IMAGE_2D_MULTISAMPLE_ARRAY, GL_UNSIGNED_INT_IMAGE_1D, GL_UNSIGNED_INT_IMAGE_2D, 
        GL_UNSIGNED_INT_IMAGE_3D, GL_UNSIGNED_INT_IMAGE_2D_RECT, GL_UNSIGNED_INT_IMAGE_CUBE, 
        GL_UNSIGNED_INT_IMAGE_BUFFER, GL_UNSIGNED_INT_IMAGE_1D_ARRAY, GL_UNSIGNED_INT_IMAGE_2D_ARRAY, 
        GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE, GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY,
        0
    };
};

}; // namespace ogl