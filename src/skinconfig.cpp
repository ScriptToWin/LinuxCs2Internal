module;
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

export module SkinConfig;

import Logger;
import Json;
import Config;

// config.json "skins" bölümünden silah bazlı skin tablosu (Kernel-Cheat formatı):
// "both" / "ct" / "t" altında silah adı -> { paint_kit, seed, wear }.
// Arama önceliği: takım (ct=3 / t=2), yoksa "both".

export namespace skinconfig {

struct SkinEntry {
    std::int32_t paint_kit = 38;
    std::int32_t seed = 0;
    float wear = 0.0001f;
};

class SkinTable {
public:
    using Map = std::unordered_map<std::string, SkinEntry>;

    bool Load(const json::Value& skins);
    bool Get(std::string_view weapon, std::uint8_t team, SkinEntry& out) const;
    bool GetExact(std::string_view section, std::string_view weapon, SkinEntry& out) const;
    bool Set(std::string_view section, std::string_view weapon, const SkinEntry& entry);
    bool SaveOverrides() const;
    std::size_t size() const noexcept {
        std::lock_guard lock(mutex_);
        return both_.size() + ct_.size() + t_.size();
    }
    static SkinTable& Global();

private:
    Map both_;
    Map ct_;
    Map t_;
    mutable std::mutex mutex_;
};

}  // namespace skinconfig

namespace {

void ParseSection(const json::Value& section, skinconfig::SkinTable::Map& dst) {
    if (section.type != json::Type::Object)
        return;
    for (const auto& kv : section.obj) {
        const json::Value& entry = kv.second;
        if (entry.type != json::Type::Object)
            continue;
        skinconfig::SkinEntry se;
        se.paint_kit = static_cast<std::int32_t>(json::GetLong(entry, "paint_kit", se.paint_kit));
        se.seed = static_cast<std::int32_t>(json::GetLong(entry, "seed", se.seed));
        se.wear = static_cast<float>(json::GetDouble(entry, "wear", se.wear));
        dst.emplace(kv.first, se);
    }
}

}  // namespace

bool skinconfig::SkinTable::Load(const json::Value& skins) {
    std::lock_guard lock(mutex_);
    both_.clear();
    ct_.clear();
    t_.clear();
    if (skins.type != json::Type::Object)
        return false;
    for (const auto& kv : skins.obj) {
        if (kv.first == "both")
            ParseSection(kv.second, both_);
        else if (kv.first == "ct")
            ParseSection(kv.second, ct_);
        else if (kv.first == "t")
            ParseSection(kv.second, t_);
    }
    return true;
}

bool skinconfig::SkinTable::Get(std::string_view weapon, std::uint8_t team, SkinEntry& out) const {
    std::lock_guard lock(mutex_);
    const std::string key(weapon);
    if (team == 3) {
        const auto it = ct_.find(key);
        if (it != ct_.end()) {
            out = it->second;
            return true;
        }
    } else if (team == 2) {
        const auto it = t_.find(key);
        if (it != t_.end()) {
            out = it->second;
            return true;
        }
    }
    const auto it = both_.find(key);
    if (it != both_.end()) {
        out = it->second;
        return true;
    }
    return false;
}

bool skinconfig::SkinTable::GetExact(std::string_view section, std::string_view weapon,
                                     SkinEntry& out) const {
    std::lock_guard lock(mutex_);
    const Map* map = nullptr;
    if (section == "both") map = &both_;
    else if (section == "ct") map = &ct_;
    else if (section == "t") map = &t_;
    if (!map)
        return false;
    const auto it = map->find(std::string(weapon));
    if (it == map->end())
        return false;
    out = it->second;
    return true;
}

namespace {

std::string OverridePath() {
    const char* home = std::getenv("HOME");
    const std::string base = (home && *home) ? home : ".";
    return base + "/.config/hamzex/skins.json";
}

skinconfig::SkinTable::Map* SectionMap(skinconfig::SkinTable& table, std::string_view section) {
    // This helper is intentionally unused outside the class methods; section
    // validation stays centralized in Set/GetExact.
    (void)table;
    (void)section;
    return nullptr;
}

void WriteSection(std::ostream& out, const char* name,
                  const skinconfig::SkinTable::Map& map, bool& firstSection) {
    if (!firstSection)
        out << ",\n";
    firstSection = false;
    out << "    \"" << name << "\": {\n";
    bool first = true;
    for (const auto& [weapon, entry] : map) {
        if (!first)
            out << ",\n";
        first = false;
        out << "      \"" << weapon << "\": {\"paint_kit\": " << entry.paint_kit
            << ", \"seed\": " << entry.seed << ", \"wear\": " << entry.wear << "}";
    }
    out << "\n    }";
}

}  // namespace

bool skinconfig::SkinTable::Set(std::string_view section, std::string_view weapon,
                                const SkinEntry& entry) {
    std::lock_guard lock(mutex_);
    Map* map = nullptr;
    if (section == "both") map = &both_;
    else if (section == "ct") map = &ct_;
    else if (section == "t") map = &t_;
    if (!map || weapon.empty())
        return false;
    (*map)[std::string(weapon)] = entry;
    return SaveOverrides();
}

bool skinconfig::SkinTable::SaveOverrides() const {
    const std::filesystem::path path(OverridePath());
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return false;
    out << "{\n";
    bool firstSection = true;
    WriteSection(out, "both", both_, firstSection);
    WriteSection(out, "ct", ct_, firstSection);
    WriteSection(out, "t", t_, firstSection);
    out << "\n}\n";
    return static_cast<bool>(out);
}

skinconfig::SkinTable& skinconfig::SkinTable::Global() {
    static SkinTable table;
    static const bool initialized = [&] {
        const json::Value* skins = config::ConfigStore::Instance().Root().Find("skins");
        if (skins && table.Load(*skins)) {
            static Logger log;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SkinTable: loaded %zu weapons from config.json",
                          table.size());
            log.info(buf);
        } else {
            static Logger log;
            log.warn("SkinTable: no 'skins' section (no skins applied)");
        }
        std::ifstream overrides(OverridePath());
        if (overrides) {
            const std::string text((std::istreambuf_iterator<char>(overrides)),
                                   std::istreambuf_iterator<char>());
            json::Value root;
            if (json::Parse(text.data(), text.size(), root) && root.type == json::Type::Object) {
                // Overlay editor values on top of config.json without touching
                // the user's original skin file.
                for (const auto& section : root.obj) {
                    if (section.second.type != json::Type::Object)
                        continue;
                    for (const auto& item : section.second.obj) {
                        if (item.second.type != json::Type::Object)
                            continue;
                        SkinEntry entry;
                        entry.paint_kit = static_cast<std::int32_t>(
                            json::GetLong(item.second, "paint_kit", entry.paint_kit));
                        entry.seed = static_cast<std::int32_t>(
                            json::GetLong(item.second, "seed", entry.seed));
                        entry.wear = static_cast<float>(
                            json::GetDouble(item.second, "wear", entry.wear));
                        table.Set(section.first, item.first, entry);
                    }
                }
            }
        }
        return true;
    }();
    (void)initialized;
    return table;
}
