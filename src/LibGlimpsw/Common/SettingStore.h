#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <imgui.h>
#include <magic_enum.hpp>

// https://github.com/ocornut/imgui/issues/7081#issuecomment-1901774784
namespace ImGui {
    template<typename> constexpr ImGuiDataType DataTypeEnum = ImGuiDataType_COUNT;
    template<> constexpr inline ImGuiDataType DataTypeEnum<int8_t>   = ImGuiDataType_S8;
    template<> constexpr inline ImGuiDataType DataTypeEnum<uint8_t>  = ImGuiDataType_U8;
    template<> constexpr inline ImGuiDataType DataTypeEnum<int16_t>  = ImGuiDataType_S16;
    template<> constexpr inline ImGuiDataType DataTypeEnum<uint16_t> = ImGuiDataType_U16;
    template<> constexpr inline ImGuiDataType DataTypeEnum<int32_t>  = ImGuiDataType_S32;
    template<> constexpr inline ImGuiDataType DataTypeEnum<uint32_t> = ImGuiDataType_U32;
    template<> constexpr inline ImGuiDataType DataTypeEnum<int64_t>  = ImGuiDataType_S64;
    template<> constexpr inline ImGuiDataType DataTypeEnum<uint64_t> = ImGuiDataType_U64;
    template<> constexpr inline ImGuiDataType DataTypeEnum<float>    = ImGuiDataType_Float;
    template<> constexpr inline ImGuiDataType DataTypeEnum<double>   = ImGuiDataType_Double;
};  // namespace ImGui
    
namespace glim {

struct SettingStore {
    using Key = std::pair<std::string, ImGuiID>;
    struct KeyHasher {
        std::size_t operator()(const Key& p) const {
            auto h1 = std::hash<std::string>{}(p.first);
            auto h2 = std::hash<ImGuiID>{}(p.second);
            return h1 ^ h2;  
        }
    };

    std::unordered_map<Key, std::string, KeyHasher> KnownValues;

    bool Checkbox(std::string_view label, bool* value) {
        bool changed = ImGui::Checkbox(label.data(), value);
        return Sync(label, value, sizeof(bool), changed);
    }
    template<typename T>
    bool Input(std::string_view label, T* value, uint32_t numComponents, const char* fmt = nullptr) {
        bool changed = ImGui::InputScalarN(label.data(), ImGui::DataTypeEnum<T>, value, (int)numComponents, nullptr, nullptr, fmt);
        return Sync(label, value, sizeof(T) * numComponents, changed);
    }
    template<typename T>
    bool Slider(std::string_view label, T* value, uint32_t numComponents, T min, T max, const char* fmt = nullptr, ImGuiSliderFlags flags = 0) {
        bool changed = ImGui::SliderScalarN(label.data(), ImGui::DataTypeEnum<T>, value, (int)numComponents, &min, &max, fmt, flags);
        return Sync(label, value, sizeof(T) * numComponents, changed);
    }
    template<typename T>
    bool Drag(std::string_view label, T* value, uint32_t numComponents, T min, T max, float speed = 1.0f, const char* fmt = nullptr, ImGuiSliderFlags flags = 0) {
        bool changed = ImGui::DragScalarN(label.data(), ImGui::DataTypeEnum<T>, value, (int)numComponents, speed, &min, &max, fmt, flags);
        return Sync(label, value, sizeof(T) * numComponents, changed);
    }
    template<typename T, typename = std::enable_if<std::is_enum_v<T>>>
    bool Combo(std::string_view label, T* value) {
        bool changed = false;

        if (ImGui::BeginCombo(label.data(), magic_enum::enum_name(*value).data())) {
            for (auto& entry : magic_enum::enum_entries<T>()) {
                if (ImGui::Selectable(entry.second.data(), *value == entry.first)) {
                    *value = entry.first;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        return Sync(label, value, sizeof(T), changed);
    }

    // Synchronize value associated with label and GUI scope hash from/to persistent storage
    bool Sync(std::string_view label, void* value, uint32_t size, bool changed) {
        ImGuiID id = ImGui::GetID(label.data(), label.data() + label.size());
        std::string& storage = KnownValues[std::pair(std::string(label), id)];
        std::string_view valueSV((char*)value, size);

        if (changed || storage.size() != size || (storage.compare(valueSV) != 0 && ImGui::GetFrameCount() > _loadSyncId + 1)) {
            storage.assign(valueSV);
            _pendingSave = true;
        } else if (ImGui::GetFrameCount() == _loadSyncId) {
            memcpy(value, storage.data(), size);
            changed = true;
        }

        if (_pendingSave && !_autoSavePath.empty() && ImGui::GetTime() > _lastSaveTime + 15.0) {
            Save(_autoSavePath);
            _lastSaveTime = ImGui::GetTime();
            _pendingSave = false;
        }
        return changed;
    }

    bool Load(std::string_view filename, bool autoSave);
    void Save(std::string_view filename);

private:
    uint32_t _loadSyncId;  // frame number where values should be sync'ed from persistent storage

    std::string _autoSavePath;
    double _lastSaveTime;
    bool _pendingSave;
};

struct TimeStat {
    void Begin();
    void End();
    void AddSample(double elapsedMs);

    void GetElapsedMs(double& mean, double& stdDev) const;

    void Draw(std::string_view label) const {
        double mean, stdDev;
        GetElapsedMs(mean, stdDev);
        ImGui::Text("%s: %.2fms ±%.2fms", label.data(), mean, stdDev);
    }

private:
    float _samples[64]{};
    uint32_t _sampleIdx = 0;
    int64_t _measureStart;
};

};  // namespace glim
