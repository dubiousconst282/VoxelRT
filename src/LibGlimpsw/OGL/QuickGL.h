#pragma once

#include <cstdint>
#include <vector>
#include <cstdarg>
#include <cstdio>

#include <glad/glad.h>

// Basic OpenGL object wrappers for quick prototyping
namespace ogl {

inline void DebugMessage(GLenum type, GLenum severity, const char* fmt, ...) {
    char msg[256];

    va_list args;
    va_start(args, fmt);
    int len = vsprintf_s(msg, fmt, args);
    va_end(args);

    glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, type, clock(), severity, len, msg);
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
}

struct Object {
    Object() { }

    Object(const Object&) = delete;             // non construction-copyable
    Object& operator=(const Object&) = delete;  // non copyable
};

enum class DataType : GLenum {
    S8 = GL_BYTE, U8 = GL_UNSIGNED_BYTE,
    S16 = GL_SHORT, U16 = GL_UNSIGNED_SHORT,
    S32 = GL_INT, U32 = GL_UNSIGNED_INT,
    F16 = GL_HALF_FLOAT, F32 = GL_FLOAT,
};

struct Texture2D : public Object {
    GLuint Handle;
    uint32_t Width, Height, MipLevels;

    Texture2D(uint32_t width, uint32_t height, uint32_t mipLevels, GLuint internalFmt) {
        Width = width;
        Height = height;
        MipLevels = mipLevels;

        glCreateTextures(GL_TEXTURE_2D, 1, &Handle);
        glTextureStorage2D(Handle, mipLevels, internalFmt, width, height);

        SetMipMode(GL_LINEAR, GL_LINEAR_MIPMAP_LINEAR);
        SetWrapMode(GL_REPEAT);
    }
    ~Texture2D() { glDeleteTextures(1, &Handle); }

    void SetMipMode(GLenum magFilter, GLenum minFilter) {
        glTextureParameteri(Handle, GL_TEXTURE_MAG_FILTER, magFilter);
        glTextureParameteri(Handle, GL_TEXTURE_MIN_FILTER, minFilter);
    }
    void SetWrapMode(GLenum mode) {
        glTextureParameteri(Handle, GL_TEXTURE_WRAP_S, mode);
        glTextureParameteri(Handle, GL_TEXTURE_WRAP_T, mode);
    }

    void SetPixels(GLenum format, GLenum type, const void* pixels, uint32_t stride) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);

        glTextureSubImage2D(Handle, 0, 0, 0, Width, Height, format, type, pixels);
        glGenerateTextureMipmap(Handle);

        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
};

struct DataBuffer : public Object {
    GLuint Handle;
    size_t Length;

    DataBuffer(size_t byteLength, GLbitfield flags = GL_MAP_WRITE_BIT) {
        glCreateBuffers(1, &Handle);
        glNamedBufferStorage(Handle, byteLength, nullptr, flags);

        Length = byteLength;
    }
    ~DataBuffer() { glDeleteBuffers(1, &Handle); }

    template<typename T>
    T* Map(GLbitfield access = GL_WRITE_ONLY, size_t offset = 0, size_t count = 0) {
        if (count == 0) count = Length;
        return glMapNamedBufferRange(Handle, offset, count, access);
    }
    void Unmap() {
        glUnmapNamedBuffer(Handle);
    }
};

struct VertexLayout {
    struct AttribDesc {
        uint16_t Offset, Count;
        DataType Type;
        char Name[64];  // Shader attribute name
    };

    std::vector<AttribDesc> Attribs;
    uint32_t Stride;
};

struct BufferSpan {
    const DataBuffer& Buffer;
    size_t Offset, Count;

    BufferSpan(const ogl::DataBuffer& buffer) : Buffer(buffer) {
        Offset = 0;
        Count = buffer.Length;
    }
    BufferSpan(const ogl::DataBuffer& buffer, size_t offset, size_t count) : Buffer(buffer) {
        Offset = offset;
        Count = count;
    }
};

struct Shader : public Object {
private:
    static const uint32_t kMaxBoundTextures = 16;

    GLuint _boundTextures[kMaxBoundTextures]{ 0 };
    GLint _textureUnits[kMaxBoundTextures]{ 0 }; //[uniform loc] -> unitId
    uint32_t _numBoundTextures = 0;
    uint32_t _vertexStride = 0;
    
    GLuint _vaoHandle;

public:
    GLuint Handle;

    Shader() {
        Handle = glCreateProgram();
        glCreateVertexArrays(1, &_vaoHandle);
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

    void DrawIndexedTriangles(const BufferSpan& vbo, const BufferSpan& ebo, DataType indexType) {
        uint32_t indexSize =
            indexType == DataType::U8 ? 1 :
            indexType == DataType::U16 ? 2 :
            indexType == DataType::U32 ? 4 :
            throw std::invalid_argument("Index type must be either U8, U16, or U32.");

        BindState();

        glVertexArrayVertexBuffer(_vaoHandle, 0, vbo.Buffer.Handle, vbo.Offset, _vertexStride);
        glVertexArrayElementBuffer(_vaoHandle, ebo.Buffer.Handle);

        glDrawElements(GL_TRIANGLES, ebo.Count / indexSize, (GLenum)indexType, (void*)(size_t)ebo.Offset);
    }
    void DrawTriangles(const BufferSpan& vbo) {
        BindState();

        glVertexArrayVertexBuffer(_vaoHandle, 0, vbo.Buffer.Handle, vbo.Offset, _vertexStride);
        glDrawArrays(GL_TRIANGLES, 0, vbo.Count / _vertexStride);
    }
    void BindState() {
        glUseProgram(Handle);

        glBindVertexArray(_vaoHandle);
        glBindTextures(0, _numBoundTextures, _boundTextures);
    }

    void SetUniform(std::string_view name, const Texture2D& tex) {
        GLint loc = glGetUniformLocation(Handle, name.data());
        if (loc < 0) return;

        int32_t unit = -1;
        for (int32_t i = 0; i < kMaxBoundTextures; i++) {
            if (_textureUnits[i] == loc) {
                unit = i;
                break;
            }
        }
        if (unit < 0) {
            unit = (int32_t)_numBoundTextures++;
            _textureUnits[unit] = loc;
            glProgramUniform1i(Handle, loc, unit);
        }
        _boundTextures[unit] = tex.Handle;
    }
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

            throw std::exception("Failed to attach shader to program");
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

            throw std::exception("Failed to link shader program");
        }
    }

    void SetVertexLayout(const VertexLayout& layout) {
        for (auto& attrib : layout.Attribs) {
            GLint location = glGetAttribLocation(Handle, attrib.Name);

            if (location < 0) {
                DebugMessage(GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION, "No input vertex attribute named '%s' in shader program #%d.", attrib.Name, Handle);
                continue;
            }

            glEnableVertexArrayAttrib(_vaoHandle, location);
            glVertexArrayAttribBinding(_vaoHandle, location, 0);

            GLenum type;
            glGetActiveAttrib(Handle, location, 0, nullptr, nullptr, &type, nullptr);

            if (!IsIntegerAttribType(type)) {
                glVertexArrayAttribFormat(_vaoHandle, location, attrib.Count, (GLenum)attrib.Type, true, attrib.Offset);
            } else {
                glVertexArrayAttribIFormat(_vaoHandle, location, attrib.Count, (GLenum)attrib.Type, attrib.Offset);
            }
        }
        _vertexStride = layout.Stride;
    }

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

    static bool IsIntegerAttribType(GLenum type) {
        return type == GL_INT || type == GL_INT_VEC2 || type == GL_INT_VEC3 || type == GL_INT_VEC4 || 
               type == GL_UNSIGNED_INT || type == GL_UNSIGNED_INT_VEC2 || type == GL_UNSIGNED_INT_VEC3 || type == GL_UNSIGNED_INT_VEC4;
    }
};

}; // namespace ogl