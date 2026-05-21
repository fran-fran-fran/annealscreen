// SPDX-License-Identifier: GPL-3.0-or-later

#include "static_panel_registry.h"
#include <spdlog/spdlog.h>

std::atomic<bool> StaticPanelRegistry::s_destroying_all_{false};

void StaticPanelRegistry::destroy_all() {
    spdlog::info("[PanelRegistry] Destroying {} registered panels",
                 destroyers_.size());
    s_destroying_all_.store(true, std::memory_order_release);
    for (auto it = destroyers_.rbegin(); it != destroyers_.rend(); ++it) {
        spdlog::debug("[PanelRegistry] Destroying: {}", it->name);
        it->destroy_fn();
    }
    s_destroying_all_.store(false, std::memory_order_release);
    destroyers_.clear();
}
