#include <fstream>
#include <chrono>

#include "SettingManager.h"
#include "BinaryIO.h"

namespace glim {

static bool IsUnreferenced(std::shared_ptr<SettingGroup>& group) {
    if (group.use_count() >= 2) return false;

    for (auto& setting : group->Settings) {
        setting->Render = {};
        setting->OnChange = {};
    }
    return true;
}

void SettingManager::Render() {
    // iterate over copy to allow callbacks to add new settings
    std::vector<SettingGroup*> activeGroups;

    for (auto& group : Groups) {
        if (!IsUnreferenced(group)) {
            activeGroups.push_back(group.get());
        }
    }

    for (auto group : activeGroups) {
        ImGui::SeparatorText(group->Name.data());

        for (auto& setting : group->Settings) {
            if (!setting->Render) continue;

            if (setting->Render(*setting) && setting->OnChange) {
                setting->OnChange(*setting);
            }
        }
    }
}

Setting* SettingManager::FindSetting(std::string_view groupName, std::string_view name) {
    for (auto& group : Groups) {
        if (group->Name != groupName) continue;

        for (auto& setting : group->Settings) {
            if (setting->Name == name) {
                return setting.get();
            }
        }
    }
    return nullptr;
}

void TimeMeasurer::Begin() {
    auto ts = std::chrono::high_resolution_clock::now();
    _measureStart = ts.time_since_epoch().count();
}
void TimeMeasurer::End() {
    auto ts = std::chrono::high_resolution_clock::now();
    int64_t elapsedNs = ts.time_since_epoch().count() - _measureStart;
    double elapsedMs = elapsedNs / 1000000.0;

    _samples[_sampleIdx++ % std::size(_samples)] = elapsedMs;
    _samplesDur += elapsedMs;
}

void TimeMeasurer::GetElapsedMs(double& mean, double& stdDev) const {
    mean = 0.0;
    for (float x : _samples) {
        mean += x;
    }
    mean /= std::size(_samples);

    double variance = 0.0;
    for (float x : _samples) {
        variance += (x - mean) * (x - mean);
    }
    stdDev = sqrt(variance / (std::size(_samples) - 1));
}

static const uint64_t SerMagic = 0x01'74'65'73'6d'69'6c'67;  // glimset\1

bool SettingManager::Load(std::string_view filename) {
    std::ifstream is(filename.data(), std::ios_base::binary);
    if (!is.is_open()) return false;

    if (io::Read<uint64_t>(is) != SerMagic) {
        return false;
    }

    uint32_t numGroups = io::Read<uint32_t>(is);

    for (uint32_t i = 0; i < numGroups; i++) {
        std::string groupName = io::ReadStr(is);
        uint32_t numSettings = io::Read<uint32_t>(is);

        for (uint32_t i = 0; i < numSettings; i++) {
            std::string name = io::ReadStr(is);
            std::string value = io::ReadStr(is);

            if (auto setting = FindSetting(groupName, name)) {
                setting->ValueStorage = value;
                
                if (setting->OnChange) {
                    setting->OnChange(*setting);
                }
            }
        }
    }

    return true;
}
void SettingManager::Save(std::string_view filename) {
    std::ofstream os(filename.data(), std::ios::binary | std::ios::trunc);

    io::Write<uint64_t>(os, SerMagic);
    io::Write<uint32_t>(os, Groups.size());

    for (auto& group : Groups) {
        io::WriteStr(os, group->Name);

        io::Write<uint32_t>(os, group->Settings.size());
        for (auto& setting : group->Settings) {
            io::WriteStr(os, setting->Name);
            io::WriteStr(os, setting->ValueStorage);
        }
    }
}

};  // namespace glim::ui
