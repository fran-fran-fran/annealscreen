// SPDX-License-Identifier: GPL-3.0-or-later

#include "static_subject_registry.h"
#include <spdlog/spdlog.h>

void StaticSubjectRegistry::deinit_all() {
    spdlog::info("[SubjectRegistry] Deinitializing {} registered singletons",
                 deinitializers_.size());
    // Reverse order: last registered = first deinitialized
    for (auto it = deinitializers_.rbegin(); it != deinitializers_.rend(); ++it) {
        spdlog::debug("[SubjectRegistry] Deinit: {}", it->name);
        it->deinit_fn();
    }
    deinitializers_.clear();
}
