#include "ShaderLib.h"
#include <fstream>

namespace ogl {

void ShaderLib::AttachStages(ShaderCompilation& comp) {
    for (auto& stage : comp.LoadParams.Stages) {
        std::string source = "#version " + DefaultVersion + "\n";

        for (auto& def : comp.LoadParams.Defines) {
            source += "#define " + def.Name + " " + def.Value + "\n";
        }
        ReadSource(source, stage.Filename, comp.IncludedFiles);

        comp.Instance->Attach(stage.Type, source);
    }
    comp.Instance->Link();
}

std::shared_ptr<Shader> ShaderLib::Load(ShaderLoadParams pars) {
    auto shader = std::make_shared<Shader>();

    ShaderCompilation comp = {
        .Instance = shader,
        .LoadParams = std::move(pars)
    };
    AttachStages(comp);

    if (_watcher != nullptr) {
        _compiledShaders.push_back(std::move(comp));
    }
    return shader;
}

static const char* g_BuiltinShaders[][2] = {
    {
        "_builtin/fullscreen_triangle.vert",

        // Drawing a single triangle instead of a quad will avoid helper fragment invocations
        // around the diagonals, assuming the hardware uses guard-band clipping.
        // See https://stackoverflow.com/a/59739538
        //     https://wallisc.github.io/rendering/2021/04/18/Fullscreen-Pass.html
        "out vec2 v_FragCoord;\n"
        "void main() {\n"
        "    const vec2 vertices[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));\n"
        "    gl_Position = vec4(vertices[gl_VertexID], 0, 1);\n"
        "    v_FragCoord = gl_Position.xy * 0.5 + 0.5;\n"
        "}\n",
    },
};

static std::unique_ptr<std::istream> OpenFileStream(const std::filesystem::path& basePath, std::string_view filename) {
    for (auto& entry : g_BuiltinShaders) {
        if (filename.compare(entry[0]) == 0) {
            return std::make_unique<std::stringstream>(std::string(entry[1]));
        }
    }

    auto stream = std::make_unique<std::ifstream>(basePath / filename);

    if (!stream->is_open()) {
        throw std::ios_base::failure("Could not open source file for reading");
    }
    return stream;
}
static std::string GetRelativePath(const std::filesystem::path& basePath, std::string_view filename) {
    auto baseDir = basePath.parent_path();
    return std::filesystem::relative(baseDir / filename, baseDir).string();
}

void ShaderLib::ReadSource(std::string& source, std::string_view filename, std::unordered_set<std::string>& includedFiles) {
    auto stream = OpenFileStream(BasePath, filename);
    std::string line;
    uint32_t lineNo = 1;

    includedFiles.emplace(GetRelativePath(BasePath, filename));

    source += "#line 1 // begin of " + std::string(filename) + "\n";

    while (std::getline(*stream, line)) {
        // Expand `#include "fn"` directives
        if (line.starts_with("#include")) {
            size_t pathStart = line.find('"', 9);
            size_t pathEnd = line.find('"', pathStart + 1);

            if (pathStart == std::string::npos || pathEnd == std::string::npos) {
                throw std::format_error("Malformed include directive");
            }

            auto includeName = GetRelativePath(BasePath / filename, std::string_view(&line[pathStart + 1], &line[pathEnd]));

            if (includedFiles.insert(includeName).second) {
                ReadSource(source, includeName, includedFiles);
                source += "#line " + std::to_string(lineNo) + " // end of " + includeName + "\n";
            }
        } else {
            source += line;
            source += '\n';
        }
        lineNo++;
    }
}

void ShaderLib::Refresh() {
    if (_watcher == nullptr) return;

    std::vector<std::filesystem::path> changedFiles;
    _watcher->PollChanges(changedFiles);

    for (uint32_t i = 0; i < _compiledShaders.size(); i++) {
        ShaderCompilation& shader = _compiledShaders[i];
        
        if (shader.Instance.use_count() < 2) {
            DebugMessage(GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION, "Deleting unused shader '%s'", shader.GetLogName());
            _compiledShaders.erase(_compiledShaders.begin() + i);
            i--;
            continue;
        }

        for (auto& filePath : changedFiles) {
            if (shader.IncludedFiles.contains(filePath.string())) {
                Recompile(shader);
                break;
            }
        }
    }
}
void ShaderLib::Recompile(ShaderCompilation& shader) {
    uint32_t oldHandle = shader.Instance->Handle;

    try {
        shader.Instance->Handle = glCreateProgram();
        shader.IncludedFiles.clear();
        
        AttachStages(shader);
        glDeleteProgram(oldHandle);

        DebugMessage(
            GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION, 
            "Successfully recompiled shader '%s'.", shader.GetLogName());
    } catch (std::exception& ex) {
        // This is kinda hacky, but whatever.
        glDeleteProgram(shader.Instance->Handle);
        shader.Instance->Handle = oldHandle;

        DebugMessage(
            GL_DEBUG_TYPE_ERROR, GL_DEBUG_SEVERITY_MEDIUM, 
            "Failed to recompile shader '%s'.\n\n%s", shader.GetLogName(), ex.what());
    }
}

};  // namespace ogl

namespace ogl::detail {

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

struct FileWatcher::ImplData {
    HANDLE _fileHandle;
    OVERLAPPED _overlapped = {};
    uint8_t _eventBuffer[4096];

    void ReadChangesAsync() {
        _overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        ReadDirectoryChangesW(_fileHandle, _eventBuffer, sizeof(_eventBuffer), true, FILE_NOTIFY_CHANGE_LAST_WRITE, NULL, &_overlapped, NULL);
    }
};

FileWatcher::FileWatcher(const std::filesystem::path& path) {
    _data = new FileWatcher::ImplData();

    _data->_fileHandle = CreateFileW(
        path.c_str(), GENERIC_READ, 
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
        NULL, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    _data->ReadChangesAsync();
};
FileWatcher::~FileWatcher() {
    CloseHandle(_data->_fileHandle);
    CloseHandle(_data->_overlapped.hEvent);
    delete _data;
}

void FileWatcher::PollChanges(std::vector<std::filesystem::path>& changedFiles) {
    DWORD numBytesReceived;
    while (GetOverlappedResult(_data->_fileHandle, &_data->_overlapped, &numBytesReceived, false)) {
        uint8_t* eventPtr = _data->_eventBuffer;
        while (true) {
            auto event = (FILE_NOTIFY_INFORMATION*)eventPtr;
            auto fileName = std::filesystem::path(event->FileName, event->FileName + event->FileNameLength / 2);

            if (event->Action == FILE_ACTION_MODIFIED &&
                // Some apps (VSCode) will cause multiple events to be fired for the same file.
                (changedFiles.size() == 0 || changedFiles[changedFiles.size() - 1] != fileName)) {
                changedFiles.push_back(fileName);
            }

            if (event->NextEntryOffset == 0) break;
            eventPtr += event->NextEntryOffset;
        }
        _data->ReadChangesAsync();
    }
}
#else // !_WIN32

#warning "ShaderLib::FileWatcher is not implemented for this platform"

FileWatcher::FileWatcher(const std::filesystem::path& path) {
};
FileWatcher::~FileWatcher() {
}

#endif


};  // namespace ogl::detail
