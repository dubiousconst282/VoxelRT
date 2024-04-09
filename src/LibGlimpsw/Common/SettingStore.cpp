#include <fstream>
#include <chrono>

#include "SettingStore.h"
#include "BinaryIO.h"

namespace glim {

void TimeStat::Begin() {
    auto ts = std::chrono::high_resolution_clock::now();
    _measureStart = ts.time_since_epoch().count();
}
void TimeStat::End() {
    auto ts = std::chrono::high_resolution_clock::now();
    int64_t elapsedNs = ts.time_since_epoch().count() - _measureStart;

    AddSample(elapsedNs / 1000000.0);
}
void TimeStat::AddSample(double elapsedMs) {
    _samples[_sampleIdx++ % std::size(_samples)] = elapsedMs;
}

void TimeStat::GetElapsedMs(double& mean, double& stdDev) const {
    uint32_t n = std::min((uint32_t)std::size(_samples), _sampleIdx);

    double sum1 = 0.0;
    double sum2 = 0.0;

    for (uint32_t i = 0; i < n; i++) {
        sum1 += _samples[i];
        sum2 += _samples[i] * _samples[i];
    }
    mean = sum1 / n;
    stdDev = sqrt((sum2 / n) - (mean * mean));
}

static const uint64_t SerMagic = 0x02'74'65'73'6d'69'6c'67;  // glimset\2

bool SettingStore::Load(std::string_view filename, bool autoSave) {
    _autoSavePath = autoSave ? filename : "";

    std::ifstream is(filename.data(), std::ios_base::binary);
    if (!is.is_open()) return false;

    if (io::Read<uint64_t>(is) != SerMagic) {
        return false;
    }

    uint32_t numEntries = io::Read<uint32_t>(is);

    for (uint32_t i = 0; i < numEntries; i++) {
        ImGuiID hash = io::Read<ImGuiID>(is);
        std::string name = io::ReadStr(is);
        std::string value = io::ReadStr(is);

        KnownValues.insert_or_assign(std::pair(name, hash), value);
    }
    _loadSyncId = (uint32_t)ImGui::GetFrameCount() + 1;

    return true;
}
void SettingStore::Save(std::string_view filename) {
    std::ofstream os(filename.data(), std::ios::binary | std::ios::trunc);

    io::Write<uint64_t>(os, SerMagic);
    io::Write<uint32_t>(os, KnownValues.size());

    for (auto& [key, value] : KnownValues) {
        io::Write<uint32_t>(os, key.second);
        io::WriteStr(os, key.first);
        io::WriteStr(os, value);
    }
}

};  // namespace glim
