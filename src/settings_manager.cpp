// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings_manager.h"

#include <spdlog/spdlog.h>
#include <hv/json.hpp>

#include <fstream>

using json = nlohmann::json;

namespace anneal {

SettingsManager& SettingsManager::instance() {
    static SettingsManager inst;
    return inst;
}

bool SettingsManager::load(const std::string& path) {
    path_ = path;
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::info("[Settings] No settings file at {}, using defaults", path);
        return false;
    }

    try {
        json j = json::parse(f);
        settings_.moonraker_host = j.value("moonraker_host", "localhost");
        settings_.moonraker_port = j.value("moonraker_port", 7125);
        settings_.heater_name    = j.value("heater_name", "annealer");
        settings_.dark_mode      = j.value("dark_mode", true);
        spdlog::info("[Settings] Loaded from {}: {}:{} heater={}",
                     path, settings_.moonraker_host,
                     settings_.moonraker_port, settings_.heater_name);
        return true;
    } catch (const json::exception& e) {
        spdlog::error("[Settings] Parse error: {}", e.what());
        return false;
    }
}

bool SettingsManager::save(const std::string& path) const {
    json j = {
        {"moonraker_host", settings_.moonraker_host},
        {"moonraker_port", settings_.moonraker_port},
        {"heater_name",    settings_.heater_name},
        {"dark_mode",      settings_.dark_mode},
    };

    std::ofstream f(path);
    if (!f.is_open()) {
        spdlog::error("[Settings] Cannot write to {}", path);
        return false;
    }
    f << j.dump(2) << "\n";
    spdlog::info("[Settings] Saved to {}", path);
    return true;
}

bool SettingsManager::save() const {
    if (path_.empty()) return false;
    return save(path_);
}

bool SettingsManager::has_valid_config() const {
    return !settings_.moonraker_host.empty() &&
           !settings_.heater_name.empty();
}

} // namespace anneal
