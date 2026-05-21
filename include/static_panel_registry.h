// SPDX-License-Identifier: GPL-3.0-or-later
// Registry for static panel instances — ensures proper destruction order.
//
// Static global panels are destroyed during exit() -> __cxa_finalize, which
// happens AFTER Application::shutdown(). By that time spdlog and LVGL may
// be dead. This registry lets panels self-register their destruction callback
// so shutdown() can tear them down in reverse order while infrastructure lives.
//
// Currently: HomePanel only.
// Future: ProfileEditorPanel (create/edit/delete profiles, graphical view).

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class StaticPanelRegistry {
  public:
    static StaticPanelRegistry& instance() {
        static StaticPanelRegistry inst;
        return inst;
    }

    static bool is_destroying_all() {
        return s_destroying_all_.load(std::memory_order_acquire);
    }

    void register_destroy(const char* name, std::function<void()> destroy_fn) {
        destroyers_.push_back({name, std::move(destroy_fn)});
    }

    void destroy_all();
    void clear() { destroyers_.clear(); }
    size_t count() const { return destroyers_.size(); }

  private:
    StaticPanelRegistry() = default;
    ~StaticPanelRegistry() = default;

    struct DestroyEntry {
        std::string name;
        std::function<void()> destroy_fn;
    };
    std::vector<DestroyEntry> destroyers_;
    static std::atomic<bool> s_destroying_all_;
};
