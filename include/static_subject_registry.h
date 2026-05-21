// SPDX-License-Identifier: GPL-3.0-or-later
// Registry for static singleton subject cleanup.
//
// Ensures subjects are deinitialized BEFORE lv_deinit() so LVGL doesn't
// try to remove observers from already-freed subjects.
//
// Destruction order in shutdown:
//   1. StaticSubjectRegistry::deinit_all()  — deinit subjects
//   2. lv_deinit()                          — LVGL cleanup (safe now)

#pragma once

#include <functional>
#include <string>
#include <vector>

class StaticSubjectRegistry {
  public:
    static StaticSubjectRegistry& instance() {
        static StaticSubjectRegistry inst;
        return inst;
    }

    void register_deinit(const char* name, std::function<void()> deinit_fn) {
        deinitializers_.push_back({name, std::move(deinit_fn)});
    }

    void deinit_all();
    void clear() { deinitializers_.clear(); }
    size_t count() const { return deinitializers_.size(); }

  private:
    StaticSubjectRegistry() = default;
    ~StaticSubjectRegistry() = default;

    struct DeinitEntry {
        std::string name;
        std::function<void()> deinit_fn;
    };
    std::vector<DeinitEntry> deinitializers_;
};
