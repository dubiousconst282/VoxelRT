#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

#include <imgui.h>

namespace glim {

struct Setting {
    std::string Name;
    std::string ValueStorage; // binary
    std::function<bool(Setting&)> Render;
    std::function<void(Setting&)> OnChange;

    template<typename T>
    T& Value() {
        static_assert(std::is_trivially_copyable<T>::value);

        if (ValueStorage.size() < sizeof(T)) {
            ValueStorage.resize(sizeof(T), '\0');
        }
        return *(T*)ValueStorage.data();
    }
};
struct TimeMeasurer {
    void Begin();
    void End();

    void GetElapsedMs(double& mean, double& stdDev) const;

private:
    float _samples[64];
    float _samplesDur;
    uint32_t _sampleIdx;
    int64_t _measureStart;
};

struct SettingGroup {
    std::string Name;
    std::vector<std::unique_ptr<Setting>> Settings;

    SettingGroup& AddCheckbox(std::string_view name, std::function<void(bool)> onToggle, bool defaultValue = false) {
        Setting& s = Add({
            .Name = std::string(name),
            .Render = [=](Setting& s) { return ImGui::Checkbox(s.Name.data(), &s.Value<bool>()); },
            .OnChange = [=](Setting& s) { onToggle(s.Value<bool>()); },
        });
        s.Value<bool>() = defaultValue;
        return *this;
    }

    TimeMeasurer* AddTimeMetric(std::string_view name) {
        Setting& s = Add({
            .Name = std::string(name),
            .Render = [](Setting& s) {
                double mean, stdDev;
                s.Value<TimeMeasurer>().GetElapsedMs(mean, stdDev);
                ImGui::Text("%s: %.2fms ±%.2fms", s.Name.data(), mean, stdDev);
                return false;    
            },
        });
        return &s.Value<TimeMeasurer>();
    }

    Setting& Add(Setting&& s) {
        for (auto& os : Settings) {
            if (os->Name == s.Name) {
                os->Render = s.Render;
                os->OnChange = s.OnChange;
                return *os;
            }
        }
        return *Settings.emplace_back(std::make_unique<Setting>(std::move(s)));
    }
};
using SettingStore = std::vector<std::shared_ptr<SettingGroup>>;

// ImGui setting manager
struct SettingManager {
    SettingStore Groups;

    // Gets or adds a group of settings.
    //
    // `ownerStore` is used for reference counting. The group will be displayed until released from it, 
    // at which point firing callbacks would not be safe as they could refer to freed instance data.
    SettingGroup& AddGroup(std::string_view name, SettingStore& ownerStore) {
        std::shared_ptr<SettingGroup> ptr;

        for (auto& group : Groups) {
            if (group->Name == name) {
                ptr = group;
                break;
            }
        }
        if (ptr == nullptr) {
            ptr = std::make_shared<SettingGroup>();
            ptr->Name = name;
            Groups.push_back(ptr);
        }
        ownerStore.push_back(ptr);
        return *ptr;
    }

    void Render();

    Setting* FindSetting(std::string_view groupName, std::string_view name);

    bool Load(std::string_view filename);
    void Save(std::string_view filename);
};

};  // namespace glim
