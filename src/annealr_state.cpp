// SPDX-License-Identifier: GPL-3.0-or-later

#include "annealr_state.h"
#include "static_subject_registry.h"
#include "ui_update_queue.h"

#include "helix-xml/helix_xml.h"

#include <spdlog/spdlog.h>
#include <hv/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

using json = nlohmann::json;

namespace anneal {

// ── AnnealrProfile ──────────────────────────────────────────────────────

float AnnealrProfile::estimate_duration_s(float start_temp) const {
    float total = 0.0f;
    float current = start_temp;
    for (const auto& seg : segments) {
        if (seg.kind == "ramp") {
            if (seg.duration_s > 0) {
                total += seg.duration_s;
            } else if (seg.rate > 0) {
                float delta = std::fabs(seg.target - current);
                total += (delta / seg.rate) * 60.0f;
            }
            current = seg.target;
        } else if (seg.kind == "soak") {
            total += seg.duration_s;
        }
    }
    return total;
}

std::vector<std::pair<float, float>> AnnealrProfile::build_planned_curve(
        float start_temp) const {
    std::vector<std::pair<float, float>> points;
    points.emplace_back(0.0f, start_temp);
    float t = 0.0f;
    float temp = start_temp;
    for (const auto& seg : segments) {
        if (seg.kind == "ramp") {
            float dur = 0.0f;
            if (seg.duration_s > 0) {
                dur = seg.duration_s;
            } else if (seg.rate > 0) {
                float delta = std::fabs(seg.target - temp);
                dur = (delta / seg.rate) * 60.0f;
            }
            t += dur;
            points.emplace_back(t, seg.target);
            temp = seg.target;
        } else if (seg.kind == "soak") {
            if (seg.duration_s > 0) {
                t += seg.duration_s;
                points.emplace_back(t, temp);
            }
        }
    }
    return points;
}

// ── Singleton ───────────────────────────────────────────────────────────

AnnealrState& AnnealrState::instance() {
    static AnnealrState inst;
    return inst;
}

// ── Lifecycle ───────────────────────────────────────────────────────────

void AnnealrState::init_subjects() {
    if (initialized_) return;
    spdlog::info("[AnnealrState] Initializing subjects");

    std::strncpy(state_buf_, "idle", sizeof(state_buf_));
    std::memset(profile_name_buf_, 0, sizeof(profile_name_buf_));
    std::memset(stage_label_buf_, 0, sizeof(stage_label_buf_));
    std::strncpy(status_text_buf_, "Idle", sizeof(status_text_buf_));

    lv_subject_init_string(&state_, state_buf_, nullptr,
                           sizeof(state_buf_), "idle");
    lv_subject_init_string(&profile_name_, profile_name_buf_, nullptr,
                           sizeof(profile_name_buf_), "");
    lv_subject_init_string(&stage_label_, stage_label_buf_, nullptr,
                           sizeof(stage_label_buf_), "");
    lv_subject_init_string(&status_text_, status_text_buf_, nullptr,
                           sizeof(status_text_buf_), "Idle");

    lv_subject_init_int(&stage_index_, 0);
    lv_subject_init_int(&stage_count_, 0);
    lv_subject_init_int(&stage_target_, 0);
    lv_subject_init_int(&stage_rate_, 0);
    lv_subject_init_int(&progress_, 0);
    lv_subject_init_int(&elapsed_s_, 0);
    lv_subject_init_int(&remaining_s_, 0);
    lv_subject_init_int(&run_elapsed_s_, 0);
    lv_subject_init_int(&profiles_version_, 0);
    lv_subject_init_int(&chamber_temp_, 220);   // 22.0°C default
    lv_subject_init_int(&chamber_target_, 0);

    std::strncpy(chamber_temp_text_buf_, "22.0\xC2\xB0""C", sizeof(chamber_temp_text_buf_));
    lv_subject_init_string(&chamber_temp_text_, chamber_temp_text_buf_, nullptr,
                           sizeof(chamber_temp_text_buf_), "22.0\xC2\xB0""C");

    std::memset(chamber_target_text_buf_, 0, sizeof(chamber_target_text_buf_));
    lv_subject_init_string(&chamber_target_text_, chamber_target_text_buf_, nullptr,
                           sizeof(chamber_target_text_buf_), "");

    std::memset(stage_rate_text_buf_, 0, sizeof(stage_rate_text_buf_));
    lv_subject_init_string(&stage_rate_text_, stage_rate_text_buf_, nullptr,
                           sizeof(stage_rate_text_buf_), "");

    std::memset(stage_kind_buf_, 0, sizeof(stage_kind_buf_));
    lv_subject_init_string(&stage_kind_, stage_kind_buf_, nullptr,
                           sizeof(stage_kind_buf_), "");

    // Register globally so XML bind_text/bind_value can find them
    lv_xml_register_subject(nullptr, "annealr_state", &state_);
    lv_xml_register_subject(nullptr, "annealr_profile_name", &profile_name_);
    lv_xml_register_subject(nullptr, "annealr_stage_label", &stage_label_);
    lv_xml_register_subject(nullptr, "annealr_stage_index", &stage_index_);
    lv_xml_register_subject(nullptr, "annealr_stage_count", &stage_count_);
    lv_xml_register_subject(nullptr, "annealr_stage_target", &stage_target_);
    lv_xml_register_subject(nullptr, "annealr_stage_rate", &stage_rate_);
    lv_xml_register_subject(nullptr, "annealr_stage_kind", &stage_kind_);
    lv_xml_register_subject(nullptr, "annealr_progress", &progress_);
    lv_xml_register_subject(nullptr, "annealr_elapsed_s", &elapsed_s_);
    lv_xml_register_subject(nullptr, "annealr_remaining_s", &remaining_s_);
    lv_xml_register_subject(nullptr, "annealr_run_elapsed_s", &run_elapsed_s_);
    lv_xml_register_subject(nullptr, "annealr_status_text", &status_text_);
    lv_xml_register_subject(nullptr, "annealr_profiles_version", &profiles_version_);
    lv_xml_register_subject(nullptr, "annealr_chamber_temp", &chamber_temp_);
    lv_xml_register_subject(nullptr, "annealr_chamber_target", &chamber_target_);
    lv_xml_register_subject(nullptr, "annealr_chamber_temp_text", &chamber_temp_text_);
    lv_xml_register_subject(nullptr, "annealr_chamber_target_text", &chamber_target_text_);
    lv_xml_register_subject(nullptr, "annealr_stage_rate_text", &stage_rate_text_);

    initialized_ = true;

    // Self-register cleanup (co-located with init per HelixScreen pattern)
    StaticSubjectRegistry::instance().register_deinit(
        "AnnealrState", []() { AnnealrState::instance().deinit_subjects(); });
}

void AnnealrState::deinit_subjects() {
    if (!initialized_) return;
    spdlog::debug("[AnnealrState] Deinitializing subjects");

    lv_subject_deinit(&state_);
    lv_subject_deinit(&profile_name_);
    lv_subject_deinit(&stage_label_);
    lv_subject_deinit(&stage_index_);
    lv_subject_deinit(&stage_count_);
    lv_subject_deinit(&stage_target_);
    lv_subject_deinit(&progress_);
    lv_subject_deinit(&elapsed_s_);
    lv_subject_deinit(&remaining_s_);
    lv_subject_deinit(&run_elapsed_s_);
    lv_subject_deinit(&status_text_);
    lv_subject_deinit(&profiles_version_);
    lv_subject_deinit(&chamber_temp_);
    lv_subject_deinit(&chamber_target_);
    lv_subject_deinit(&chamber_temp_text_);
    lv_subject_deinit(&chamber_target_text_);
    lv_subject_deinit(&stage_rate_);
    lv_subject_deinit(&stage_kind_);
    lv_subject_deinit(&stage_rate_text_);

    initialized_ = false;
}

// ── Profile loading ─────────────────────────────────────────────────────

AnnealrSegment AnnealrState::parse_segment_line(const std::string& line,
                                                 float prev_target) {
    AnnealrSegment seg{};
    std::vector<std::string> parts;
    std::istringstream iss(line);
    std::string token;
    while (std::getline(iss, token, ',')) {
        auto start = token.find_first_not_of(" \t");
        auto end   = token.find_last_not_of(" \t");
        if (start != std::string::npos)
            parts.push_back(token.substr(start, end - start + 1));
    }
    if (parts.size() < 2) return seg;

    seg.kind = parts[0];
    std::transform(seg.kind.begin(), seg.kind.end(), seg.kind.begin(), ::tolower);

    float value = 0;
    try { value = std::stof(parts[1]); } catch (...) { return seg; }

    seg.rate = 0;
    seg.duration_s = 0;

    for (size_t i = 2; i < parts.size(); ++i) {
        const auto& p = parts[i];
        if (p.rfind("rate=", 0) == 0) {
            try { seg.rate = std::stof(p.substr(5)); } catch (...) {}
        } else if (p.rfind("duration=", 0) == 0) {
            try { seg.duration_s = std::stof(p.substr(9)) * 60.0f; } catch (...) {}
        }
    }

    if (seg.kind == "ramp") {
        seg.target = value;
    } else if (seg.kind == "soak") {
        seg.target = prev_target;
        seg.duration_s = value * 60.0f;
    }
    return seg;
}

void AnnealrState::load_profiles_from_config(const std::string& config_json) {
    json config;
    try {
        config = json::parse(config_json);
    } catch (const json::exception& e) {
        spdlog::error("[AnnealrState] Config parse error: {}", e.what());
        return;
    }

    std::lock_guard<std::mutex> lock(profiles_mutex_);
    profiles_.clear();

    // The config comes from Moonraker's configfile.config response
    // which is a dict of section_name → section_data
    for (auto& [section_name, section_data] : config.items()) {
        if (section_name.rfind("annealr_profile ", 0) != 0) continue;
        std::string name = section_name.substr(16);

        AnnealrProfile profile;
        profile.name = name;
        profile.description = section_data.value("description", "");

        std::string segments_raw = section_data.value("segments", "");
        float prev_target = 0;

        std::istringstream stream(segments_raw);
        std::string line;
        while (std::getline(stream, line)) {
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            auto end = line.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) line.resize(end + 1);
            if (line.empty()) continue;

            AnnealrSegment seg = parse_segment_line(line, prev_target);
            if (seg.kind.empty()) continue;
            if (seg.kind == "ramp") prev_target = seg.target;
            profile.segments.push_back(std::move(seg));
        }

        if (!profile.segments.empty()) {
            spdlog::info("[AnnealrState] Loaded profile '{}' ({} segments)",
                         profile.name, profile.segments.size());
            profiles_.push_back(std::move(profile));
        }
    }

    std::sort(profiles_.begin(), profiles_.end(),
              [](const AnnealrProfile& a, const AnnealrProfile& b) {
                  return a.name < b.name;
              });

    if (initialized_) {
        int v = lv_subject_get_int(&profiles_version_);
        lv_subject_set_int(&profiles_version_, v + 1);
    }
}

// ── Status updates ──────────────────────────────────────────────────────

void AnnealrState::update_from_status(const std::string& status_json) {
    json data;
    try {
        data = json::parse(status_json);
    } catch (const json::exception& e) {
        spdlog::warn("[AnnealrState] Status parse error: {}", e.what());
        return;
    }

    // Moonraker sends PARTIAL updates — only changed fields are present.
    // Use current values as defaults so a partial update (e.g. only
    // run_elapsed_s changed) doesn't reset state to "idle".
    std::string state_str    = data.value("state", std::string(state_buf_));
    std::string profile_name = data.value("profile", std::string(profile_name_buf_));
    int         stage_idx    = data.value("stage_index",
                                   static_cast<int>(lv_subject_get_int(&stage_index_)));
    int         stage_cnt    = data.value("stage_count",
                                   static_cast<int>(lv_subject_get_int(&stage_count_)));
    float       progress_f   = data.value("progress",
                                   lv_subject_get_int(&progress_) / 100.0f);
    float       run_elapsed  = data.value("run_elapsed_s",
                                   static_cast<float>(lv_subject_get_int(&run_elapsed_s_)));

    std::string stage_lbl    = std::string(stage_label_buf_);
    float       stage_tgt    = lv_subject_get_int(&stage_target_) / 10.0f;
    float       seg_elapsed  = static_cast<float>(lv_subject_get_int(&elapsed_s_));
    float       seg_remaining = static_cast<float>(lv_subject_get_int(&remaining_s_));

    if (data.contains("stage") && !data["stage"].is_null()) {
        const auto& stage = data["stage"];
        stage_lbl     = stage.value("label", stage_lbl);
        stage_tgt     = stage.value("target", stage_tgt);
        seg_elapsed   = stage.value("elapsed_s", seg_elapsed);
        seg_remaining = stage.value("remaining_s", seg_remaining);
        if (stage.contains("progress"))
            progress_f = stage.value("progress", progress_f);
    }

    // Parse rate and kind (may be absent in partial updates)
    std::string stage_kind_str = std::string(stage_kind_buf_);
    float stage_rate_val = lv_subject_get_int(&stage_rate_) / 100.0f;
    bool rate_is_null = (lv_subject_get_int(&stage_rate_) == 0 &&
                         stage_kind_str == "ramp");

    if (data.contains("stage") && !data["stage"].is_null()) {
        const auto& stage = data["stage"];
        if (stage.contains("kind"))
            stage_kind_str = stage["kind"].get<std::string>();
        if (stage.contains("rate")) {
            if (stage["rate"].is_null()) {
                stage_rate_val = 0;
                rate_is_null = true;
            } else {
                stage_rate_val = stage["rate"].get<float>();
                rate_is_null = false;
            }
        }
    }

    // Defer to LVGL thread
    anneal::ui::queue_update(
        [this, state_str, profile_name, stage_idx, stage_cnt,
         stage_lbl, stage_tgt, progress_f, seg_elapsed,
         seg_remaining, run_elapsed, stage_kind_str, stage_rate_val,
         rate_is_null]() {
            apply_status(state_str, profile_name, stage_idx, stage_cnt,
                         stage_lbl, stage_tgt, progress_f,
                         seg_elapsed, seg_remaining, run_elapsed);

            // Update kind
            std::strncpy(stage_kind_buf_, stage_kind_str.c_str(),
                         sizeof(stage_kind_buf_) - 1);
            stage_kind_buf_[sizeof(stage_kind_buf_) - 1] = '\0';
            lv_subject_copy_string(&stage_kind_, stage_kind_buf_);

            // Update rate (stored as centi-degrees/min for int subject)
            lv_subject_set_int(&stage_rate_,
                               static_cast<int>(stage_rate_val * 100));

            // Format rate text
            if (stage_kind_str == "soak") {
                std::strncpy(stage_rate_text_buf_, "",
                             sizeof(stage_rate_text_buf_));
            } else if (rate_is_null) {
                std::strncpy(stage_rate_text_buf_, "Rate: unbound",
                             sizeof(stage_rate_text_buf_));
            } else {
                std::snprintf(stage_rate_text_buf_, sizeof(stage_rate_text_buf_),
                              "Rate: %.1f \xC2\xB0""C/min", stage_rate_val);
            }
            lv_subject_copy_string(&stage_rate_text_, stage_rate_text_buf_);

            // Format chamber target text
            float target_c = lv_subject_get_int(&chamber_target_) / 10.0f;
            if (target_c > 0) {
                std::snprintf(chamber_target_text_buf_,
                              sizeof(chamber_target_text_buf_),
                              "%.1f\xC2\xB0""C", target_c);
            } else {
                std::strncpy(chamber_target_text_buf_, "",
                             sizeof(chamber_target_text_buf_));
            }
            lv_subject_copy_string(&chamber_target_text_,
                                   chamber_target_text_buf_);
        });
}

void AnnealrState::apply_status(
        const std::string& state_str,
        const std::string& profile_name,
        int stage_index, int stage_count,
        const std::string& stage_label,
        float stage_target, float progress,
        float elapsed_s, float remaining_s,
        float run_elapsed_s) {

    std::strncpy(state_buf_, state_str.c_str(), sizeof(state_buf_) - 1);
    state_buf_[sizeof(state_buf_) - 1] = '\0';
    lv_subject_copy_string(&state_, state_buf_);

    std::strncpy(profile_name_buf_, profile_name.c_str(),
                 sizeof(profile_name_buf_) - 1);
    profile_name_buf_[sizeof(profile_name_buf_) - 1] = '\0';
    lv_subject_copy_string(&profile_name_, profile_name_buf_);

    std::strncpy(stage_label_buf_, stage_label.c_str(),
                 sizeof(stage_label_buf_) - 1);
    stage_label_buf_[sizeof(stage_label_buf_) - 1] = '\0';
    lv_subject_copy_string(&stage_label_, stage_label_buf_);

    lv_subject_set_int(&stage_index_, stage_index);
    lv_subject_set_int(&stage_count_, stage_count);
    lv_subject_set_int(&stage_target_,
                       static_cast<int>(stage_target * 10));
    lv_subject_set_int(&progress_,
                       static_cast<int>(progress * 100));
    lv_subject_set_int(&elapsed_s_, static_cast<int>(elapsed_s));
    lv_subject_set_int(&remaining_s_, static_cast<int>(remaining_s));
    lv_subject_set_int(&run_elapsed_s_, static_cast<int>(run_elapsed_s));

    update_status_text(state_str, profile_name, stage_label,
                       progress, remaining_s, run_elapsed_s);
}

void AnnealrState::update_status_text(
        const std::string& state_str,
        const std::string& profile_name,
        const std::string& stage_label,
        float progress, float remaining_s,
        float run_elapsed_s) {

    std::string state_cap = state_str;
    if (!state_cap.empty())
        state_cap[0] = static_cast<char>(std::toupper(state_cap[0]));

    std::string text = state_cap;

    if (!profile_name.empty()) {
        text += "\n";
        text += profile_name;
    }
    if (!stage_label.empty()) {
        text += "\n";
        text += stage_label;
    }
    if (progress > 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\n%.0f%%", progress * 100.0f);
        text += buf;
    }
    if (remaining_s > 0) {
        int rm = static_cast<int>(remaining_s / 60.0f);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "\n%dm remaining", rm);
        text += buf;
    }
    if (run_elapsed_s > 0) {
        int em = static_cast<int>(run_elapsed_s / 60.0f);
        int eh = em / 60;
        char buf[64];
        if (eh > 0)
            std::snprintf(buf, sizeof(buf), "\nElapsed: %dh %dm", eh, em % 60);
        else
            std::snprintf(buf, sizeof(buf), "\nElapsed: %dm", em);
        text += buf;
    }

    std::strncpy(status_text_buf_, text.c_str(), sizeof(status_text_buf_) - 1);
    status_text_buf_[sizeof(status_text_buf_) - 1] = '\0';
    lv_subject_copy_string(&status_text_, status_text_buf_);
}

// ── Temperature recording ───────────────────────────────────────────────

void AnnealrState::record_temperature(float temp_c, float run_elapsed_s) {
    // Update chamber temp subject on LVGL thread
    int centi = static_cast<int>(temp_c * 10);
    anneal::ui::queue_update([this, centi, temp_c]() {
        lv_subject_set_int(&chamber_temp_, centi);

        // Format display string: "28.6°C"  (°= UTF-8 0xC2 0xB0)
        std::snprintf(chamber_temp_text_buf_, sizeof(chamber_temp_text_buf_),
                      "%.1f\xC2\xB0""C", temp_c);
        lv_subject_copy_string(&chamber_temp_text_, chamber_temp_text_buf_);
    });

    // Store history (thread-safe)
    std::lock_guard<std::mutex> lock(temp_mutex_);
    temp_history_.push_back({run_elapsed_s, temp_c});
    while (temp_history_.size() > MAX_TEMP_SAMPLES)
        temp_history_.pop_front();
}

void AnnealrState::clear_temp_history() {
    std::lock_guard<std::mutex> lock(temp_mutex_);
    temp_history_.clear();
}

// ── Convenience queries ─────────────────────────────────────────────────

bool AnnealrState::is_run_active() const {
    return (std::strcmp(state_buf_, "ramping") == 0 ||
            std::strcmp(state_buf_, "soaking") == 0 ||
            std::strcmp(state_buf_, "cooling") == 0 ||
            std::strcmp(state_buf_, "paused") == 0);
}

bool AnnealrState::is_paused() const {
    return std::strcmp(state_buf_, "paused") == 0;
}

bool AnnealrState::can_start() const {
    return (std::strcmp(state_buf_, "idle") == 0 ||
            std::strcmp(state_buf_, "complete") == 0 ||
            std::strcmp(state_buf_, "cancelled") == 0 ||
            std::strcmp(state_buf_, "error") == 0);
}

} // namespace anneal