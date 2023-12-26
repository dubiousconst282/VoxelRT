#pragma once

#include <filesystem>
#include <unordered_set>

#include "QuickGL.h"

namespace ogl {
namespace detail {

struct FileWatcher : public Object {
    FileWatcher(const std::filesystem::path& path);
    ~FileWatcher();

    // Returns relative paths to files changed inside the base directory.
    void PollChanges(std::vector<std::filesystem::path>& changedFiles);

private:
    struct ImplData;
    ImplData* _data; // This is to avoid leaking WinAPI stuff, to avoid collisions.
};

};  // namespace detail

struct ShaderStageDesc {
    GLenum Type;
    std::string Filename;
};

struct ShaderLib {
    std::filesystem::path BasePath;
    std::string DefaultVersion = "450";

    ShaderLib(const std::filesystem::path& basePath, bool watchSourceChanges) {
        BasePath = basePath;

        if (watchSourceChanges) {
            _watcher = std::make_unique<detail::FileWatcher>(BasePath);
        }
    }

    std::shared_ptr<Shader> LoadVertFrag(const std::string& name) {
        ShaderStageDesc stages[2]{
            { GL_VERTEX_SHADER, name + ".vert" },
            { GL_FRAGMENT_SHADER, name + ".frag" },
        };
        return Load(stages, 2);
    }
    std::shared_ptr<Shader> Load(const ShaderStageDesc* stages, uint32_t numStages);
    
    void ReadSource(std::string& source, std::string_view filename, std::unordered_set<std::filesystem::path>* relatedSources = nullptr);

    // Re-compile shaders whose source has been changed. 
    void Refresh();

private:
    struct ShaderCompilation {
        std::shared_ptr<Shader> Instance;
        std::vector<ShaderStageDesc> Sources;
        std::unordered_set<std::filesystem::path> RelatedSourceFiles;
    };

    std::unique_ptr<detail::FileWatcher> _watcher;
    std::vector<ShaderCompilation> _compiledShaders;

    void AttachStages(ShaderCompilation& shader);
    void Recompile(ShaderCompilation& shader);
};

};  // namespace ogl
