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


struct ShaderLoadParams {
    struct StageDesc {
        GLenum Type;
        std::string Filename;
    };
    struct PrepDef {
        std::string Name, Value;
    };

    std::vector<StageDesc> Stages;
    std::vector<PrepDef> Defines;
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

    std::shared_ptr<Shader> LoadVertFrag(const std::string& name, std::vector<ShaderLoadParams::PrepDef> prepDefs = {}) {
        return Load({
            .Stages = {
                { GL_VERTEX_SHADER, name + ".vert" },
                { GL_FRAGMENT_SHADER, name + ".frag" },
            },
            .Defines = std::move(prepDefs),
        });
    }
    // Loads a fragment shader that is intended to be applied over a full screen triangle.
    // See `Shader::DispatchFullscreen()`.
    std::shared_ptr<Shader> LoadFrag(const std::string& name, std::vector<ShaderLoadParams::PrepDef> prepDefs = {}) {
        return Load({
            .Stages = {
                { GL_FRAGMENT_SHADER, name + ".frag" },
                { GL_VERTEX_SHADER, "_builtin/fullscreen_triangle.vert" },
            },
            .Defines = std::move(prepDefs),
        });
    }
    std::shared_ptr<Shader> LoadComp(const std::string& name, std::vector<ShaderLoadParams::PrepDef> prepDefs = {}) {
        return Load({
            .Stages = {
                { GL_COMPUTE_SHADER, name + ".comp" },
            },
            .Defines = std::move(prepDefs),
        });
    }

    std::shared_ptr<Shader> Load(ShaderLoadParams pars);
    
    // Reads and expands includes for the given file, appending result to `source`.
    void ReadSource(std::string& source, std::string_view filename, std::unordered_set<std::string>& includedFiles);

    // Re-compile shaders whose source has been changed. 
    void Refresh();

private:
    struct ShaderCompilation {
        std::shared_ptr<Shader> Instance;
        std::unordered_set<std::string> IncludedFiles;
        ShaderLoadParams LoadParams;

        const char* GetLogName() { return LoadParams.Stages[0].Filename.c_str(); }
    };

    std::unique_ptr<detail::FileWatcher> _watcher;
    std::vector<ShaderCompilation> _compiledShaders;

    void AttachStages(ShaderCompilation& shader);
    void Recompile(ShaderCompilation& shader);
};

};  // namespace ogl
