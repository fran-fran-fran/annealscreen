// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal settings persistence for AnnealScreen.
// Stores Moonraker connection info and heater name in config/settings.json.

#pragma once

#include <string>

namespace anneal {

struct Settings {
    std::string moonraker_host = "localhost";
    int         moonraker_port = 7125;
    std::string heater_name    = "annealer";
    bool        dark_mode      = true;
};

class SettingsManager {
  public:
    static SettingsManager& instance();

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    bool save() const;   // saves to last loaded path

    Settings& settings() { return settings_; }
    const Settings& settings() const { return settings_; }

    bool has_valid_config() const;
    const std::string& config_path() const { return path_; }

  private:
    SettingsManager() = default;
    Settings settings_;
    std::string path_;
};

} // namespace anneal
