#include "ShaderLib.h"
#include <fstream>

namespace ogl {

void ShaderLib::AttachStages(ShaderCompilation& comp) {
    for (ShaderStageDesc& stage : comp.Sources) {
        std::string source = "#version " + DefaultVersion + "\n";
        ReadSource(source, stage.Filename, &comp.RelatedSourceFiles);

        comp.Instance->Attach(stage.Type, source);
    }
    comp.Instance->Link();
}

std::shared_ptr<Shader> ShaderLib::Load(const ShaderStageDesc* stages, uint32_t numStages) {
    auto shader = std::make_shared<Shader>();

    ShaderCompilation comp = {
        .Instance = shader,
        .Sources = std::vector<ShaderStageDesc>(&stages[0], &stages[numStages]),
    };
    AttachStages(comp);

    if (_watcher != nullptr) {
        _compiledShaders.push_back(std::move(comp));
    }
    return shader;
}

void ShaderLib::ReadSource(std::string& source, std::string_view filename, std::unordered_set<std::filesystem::path>* relatedSources) {
    auto stream = std::ifstream(BasePath / filename);

    if (!stream.is_open()) {
        throw std::ios_base::failure("Could not open source file for reading");
    }

    std::string line;

    while (std::getline(stream, line)) {
        // Expand `#include "fn"` directives
        if (line.starts_with("#include")) {
            size_t pathStart = line.find('"', 9);
            size_t pathEnd = line.find('"', pathStart + 1);

            if (pathStart == std::string::npos || pathEnd == std::string::npos) {
                throw std::format_error("Malformed include directive");
            }
            std::string_view includedFile = std::string_view(&line[pathStart + 1], &line[pathEnd]);

            source += "// " + line + " ---- begin \n";
            ReadSource(source, includedFile, relatedSources);
            source += "// " + line + " ---- end\n";
        } else {
            source += line;
            source += '\n';
        }
    }

    if (relatedSources != nullptr) {
        relatedSources->insert(filename);
    }
}

void ShaderLib::Refresh() {
    if (_watcher == nullptr) return;

    std::vector<std::filesystem::path> changedFiles;
    _watcher->PollChanges(changedFiles);

    for (auto& filePath : changedFiles) {
        for (auto& shader : _compiledShaders) {
            if (shader.RelatedSourceFiles.contains(filePath)) {
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
        AttachStages(shader);

        DebugMessage(
            GL_DEBUG_TYPE_MARKER, GL_DEBUG_SEVERITY_NOTIFICATION, 
            "Successfully recompiled shader '%s'.", shader.Sources[0].Filename.c_str());
    } catch (std::exception& ex) {
        // This is kinda hacky, but whatever.
        glDeleteProgram(shader.Instance->Handle);
        shader.Instance->Handle = oldHandle;

        DebugMessage(
            GL_DEBUG_TYPE_ERROR, GL_DEBUG_SEVERITY_MEDIUM, 
            "Failed to recompile shader '%s'.\n\n%s", shader.Sources[0].Filename.c_str(), ex.what());
    }
}

};  // namespace ogl

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace ogl::detail {

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
    if (!GetOverlappedResult(_data->_fileHandle, &_data->_overlapped, &numBytesReceived, false)) return;

    uint8_t* eventPtr = _data->_eventBuffer;
    while (true) {
        auto event = (FILE_NOTIFY_INFORMATION*)eventPtr;

        if (event->Action == FILE_ACTION_MODIFIED) {
            changedFiles.push_back(std::wstring_view(event->FileName, event->FileNameLength / 2));
        }

        if (event->NextEntryOffset == 0) break;
        eventPtr += event->NextEntryOffset;
    }
    _data->ReadChangesAsync();
}

};  // namespace ogl::detail

#endif