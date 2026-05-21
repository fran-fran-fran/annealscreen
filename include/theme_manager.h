// SPDX-License-Identifier: GPL-3.0-or-later
// Simplified theme manager for AnnealScreen.
//
// Loads color palettes from JSON files and registers them as LVGL XML
// constants so XML layout files can reference #card_bg, #primary, etc.
// No responsive breakpoint system (fixed 800×480 reference for now).

#pragma once

#include <lvgl.h>

#include <string>
#include <unordered_map>

namespace anneal {

class ThemeManager {
  public:
    static ThemeManager& instance();

    // Load a theme JSON file (config/themes/dark.json or light.json)
    bool load(const std::string& path);

    // Apply the loaded theme: sets screen bg, registers XML color consts
    void apply();

    // Get a color by token name
    lv_color_t get_color(const std::string& name) const;
    bool has_color(const std::string& name) const;

  private:
    ThemeManager() = default;

    void register_spacing_tokens();

    std::unordered_map<std::string, lv_color_t> colors_;
    bool loaded_ = false;
};

// Convenience function matching HelixScreen's ui_theme_get_color()
inline lv_color_t ui_theme_get_color(const std::string& name) {
    return ThemeManager::instance().get_color(name);
}

} // namespace anneal
