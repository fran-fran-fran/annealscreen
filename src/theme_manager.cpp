// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme_manager.h"

#include "helix-xml/helix_xml.h"

#include <spdlog/spdlog.h>
#include <hv/json.hpp>

#include <fstream>

using json = nlohmann::json;

namespace anneal {

ThemeManager& ThemeManager::instance() {
    static ThemeManager inst;
    return inst;
}

static lv_color_t parse_hex_color(const std::string& hex) {
    if (hex.empty() || hex[0] != '#' || hex.size() < 7) {
        return lv_color_black();
    }
    uint32_t val = std::strtoul(hex.c_str() + 1, nullptr, 16);
    return lv_color_hex(val);
}

bool ThemeManager::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        spdlog::error("[Theme] Cannot open {}", path);
        return false;
    }

    try {
        json j = json::parse(f);

        colors_.clear();

        if (j.contains("colors") && j["colors"].is_object()) {
            for (auto& [name, val] : j["colors"].items()) {
                if (val.is_string()) {
                    colors_[name] = parse_hex_color(val.get<std::string>());
                }
            }
        }

        loaded_ = true;
        spdlog::info("[Theme] Loaded {} colors from {}", colors_.size(), path);
        return true;
    } catch (const json::exception& e) {
        spdlog::error("[Theme] Parse error: {}", e.what());
        return false;
    }
}

void ThemeManager::apply() {
    if (!loaded_) {
        spdlog::warn("[Theme] No theme loaded, applying defaults");
        // Register minimal dark defaults
        colors_["screen_bg"]   = lv_color_hex(0x1A1A2E);
        colors_["card_bg"]     = lv_color_hex(0x25253E);
        colors_["elevated_bg"] = lv_color_hex(0x2D2D4A);
        colors_["overlay_bg"]  = lv_color_hex(0x0F0F1A);
        colors_["border"]      = lv_color_hex(0x3A3A52);
        colors_["text"]        = lv_color_hex(0xE8E8F0);
        colors_["text_muted"]  = lv_color_hex(0xA8A8B8);
        colors_["text_subtle"] = lv_color_hex(0x707088);
        colors_["primary"]     = lv_color_hex(0x6C63FF);
        colors_["secondary"]   = lv_color_hex(0x2EC4B6);
        colors_["tertiary"]    = lv_color_hex(0xFF6B6B);
        colors_["info"]        = lv_color_hex(0x4DA6FF);
        colors_["success"]     = lv_color_hex(0x4CAF50);
        colors_["warning"]     = lv_color_hex(0xFFA726);
        colors_["danger"]      = lv_color_hex(0xEF5350);
        colors_["focus"]       = lv_color_hex(0x6C63FF);
    }

    // Register color tokens as XML consts so #token_name works in XML
    for (const auto& [name, color] : colors_) {
        // lv_xml_register_const takes string values; we register as hex
        char hex[10];
        lv_snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                     color.red, color.green, color.blue);
        lv_xml_register_const(nullptr, name.c_str(), hex);
    }

    register_spacing_tokens();

    // Set screen background
    lv_obj_t* scr = lv_screen_active();
    if (scr && has_color("screen_bg")) {
        lv_obj_set_style_bg_color(scr, get_color("screen_bg"), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    }

    spdlog::info("[Theme] Applied {} color tokens", colors_.size());
}

void ThemeManager::register_spacing_tokens() {
    // Fixed spacing tokens for 800×480 (MEDIUM breakpoint in HelixScreen terms)
    // These match HelixScreen's globals.xml MEDIUM-suffix values
    struct { const char* name; const char* value; } tokens[] = {
        {"space_xxs", "2"},
        {"space_xs",  "4"},
        {"space_sm",  "6"},
        {"space_md",  "10"},
        {"space_lg",  "14"},
        {"space_xl",  "18"},
        {"space_2xl", "24"},
        {"border_radius", "8"},
        {"button_height", "42"},
        {"header_height", "44"},
    };

    for (const auto& t : tokens) {
        lv_xml_register_const(nullptr, t.name, t.value);
    }
}

lv_color_t ThemeManager::get_color(const std::string& name) const {
    auto it = colors_.find(name);
    if (it != colors_.end()) return it->second;
    spdlog::warn("[Theme] Unknown color token: {}", name);
    return lv_color_hex(0xFF00FF); // Magenta = missing token
}

bool ThemeManager::has_color(const std::string& name) const {
    return colors_.count(name) > 0;
}

} // namespace anneal
