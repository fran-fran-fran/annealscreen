# CLAUDE.md

## Quick Start

**AnnealScreen**: LVGL 9.5 touchscreen UI for the [annealr](https://github.com/fran-fran-fran/annealr) Klipper plugin. XML engine in `lib/helix-xml/` (extracted from LVGL). Pattern: XML → Subjects → C++. Single-panel application — no navigation system, overlays are one-off imperative creations.

```bash
make -j                              # Build native (SDL) binary
./build/bin/annealscreen --test -vv  # Mock mode + DEBUG logs
# ALWAYS use verbosity: -v=INFO, -vv=DEBUG, -vvv=TRACE (default=WARN)

make pi-docker                       # Cross-compile for Raspberry Pi via Docker (debian:12)
make clean                           # Remove build artifacts

# First-time setup (init submodules)
make deps
```

**XML runtime loading:** XML files in `ui_xml/` are loaded at runtime — edit, restart the binary, no rebuild needed. No hot-reload (no `HELIX_HOT_RELOAD` support).

---

## Code Standards

| Rule | ❌ WRONG | ✅ CORRECT |
|------|----------|-----------|
| **spdlog only** | `printf()`, `cout`, `LV_LOG_*` | `spdlog::info("temp: {}", t)` |
| **SPDX headers** | 20-line GPL boilerplate | `// SPDX-License-Identifier: GPL-3.0-or-later` |
| **Class-based** | Free functions for state | Class singletons: `HomePanel`, `AnnealrState` |
| **JSON include** | `#include <nlohmann/json.hpp>` | `#include "hv/json.hpp"` (libhv's bundled version) |
| **Build system** | `cmake`, `ninja` | `make -j` (pure Makefile) |
| **Namespace** | Global scope | `namespace anneal` for classes, `namespace anneal::ui` for UI utils |

**ALWAYS:** Search the same file you're editing for similar patterns before implementing.

---

## CRITICAL RULES - Declarative UI

**DATA in C++, APPEARANCE in XML, Subjects connect them.**

| # | Rule | ❌ NEVER | ✅ ALWAYS |
|---|------|----------|----------|
| 1 | **NO lv_obj_add_event_cb()** | `lv_obj_add_event_cb(btn, cb)` | XML `<event_cb trigger="clicked" callback="name"/>` + `lv_xml_register_event_cb()` |
| 2 | **NO imperative visibility** | `lv_obj_add_flag(obj, HIDDEN)` | XML `<bind_flag_if_eq subject="state" flag="hidden" ref_value="0"/>` |
| 3 | **NO lv_label_set_text** | `lv_label_set_text(lbl, val)` | XML `<lv_label bind_text="annealr_status_text"/>` |
| 4 | **NO C++ styling** | `lv_obj_set_style_bg_color()` | XML: `style_bg_color="#card_bg"` |
| 5 | **NO manual LVGL cleanup** | `lv_display_delete()`, `lv_group_delete()` | Just `lv_deinit()` - handles everything |

**Known exceptions to declarative rules:**
- **Textarea + keyboard** (`main.cpp:122-130`): `lv_obj_add_event_cb()` for FOCUSED/DEFOCUSED is required by LVGL to wire a keyboard to a textarea. This is correct.
- **Dynamic profile list** (`home_panel.cpp:165-237`): Programmatic `lv_obj_clean()` + button creation with `lv_obj_add_event_cb()` + `lv_obj_set_user_data()` for data-driven lists. No XML template for per-item rendering.
- **Chart widget** (`ui_temp_graph.h`): C-style struct API with draw callbacks. LVGL charts require manual data push and draw event callbacks — no XML equivalent.
- **Settings icon** (`home_panel.cpp:106-110`): `lv_obj_set_style_text_font()` + `lv_label_set_text()` to set MDI icon font and unicode glyph — one-time setup, not dynamic binding.

---

## Design Tokens (MANDATORY)

| Category | ❌ WRONG | ✅ CORRECT |
|----------|----------|-----------|
| **Colors** | `lv_color_hex(0xE0E0E0)` | `ui_theme_get_color("card_bg")` |
| **Spacing** | `style_pad_all="12"` | `style_pad_all="#space_md"` |
| **Typography** | `<lv_label style_text_font="...">` | Tokens defined in `globals.xml` |

Theme colors are registered as XML constants by `ThemeManager::apply()` — use `#token_name` in XML and `ui_theme_get_color("token")` in C++. Defaults (dark theme) are hardcoded in `theme_manager.cpp:62-77` as fallback when no JSON theme is loaded.

---

## Threading & Lifecycle

WebSocket/libhv callbacks = background thread. **NEVER** call `lv_subject_set_*()` directly.
Use `anneal::ui::queue_update()` from `ui_update_queue.h`. See `moonraker_client.cpp` and `annealr_state.cpp` for the pattern.

Use `ObserverGuard` for RAII cleanup. See `observer_factory.h` for `observe_int_sync<HomePanel>()` and `observe_string<HomePanel>()` — both **defer callbacks** via `ui_queue_update()` to prevent re-entrant observer destruction crashes.

**No `SubjectLifetime` needed:** All subjects in this project are static singletons with fixed buffers (no per-fan/per-sensor dynamic subjects). The `ObserverGuard` is simplified — no lifetime token parameter.

**`ObserverGuard::reset()` is the default — `release()` is NOT.** Use `reset()` for cleanup. `release()` is only for `StaticSubjectRegistry::register_deinit()` callbacks where the subject is already destroyed.

**libhv handles WebSocket reconnect:** `moonraker_client.cpp:31-34` configures libhv's `reconn_setting_t` (min_delay=1000ms, max_delay=30000ms, exponential). No custom reconnect thread.

**Shutdown order (from `main.cpp:476-498`):**
1. `disconnect()` WebSocket client + delete
2. `update_queue_shutdown()` — drain pending callbacks
3. `StaticPanelRegistry::destroy_all()` — destroy panels (reverse order)
4. `StaticSubjectRegistry::deinit_all()` — deinit subjects before LVGL teardown
5. `ObserverGuard::invalidate_all()` — invalidate remaining observer guards
6. `lv_deinit()` — LVGL cleanup
7. `crash_handler::uninstall()`

**Subject init order:** Register XML components → `init_subjects()` → create XML objects.
Each class registers its own cleanup via `StaticSubjectRegistry::register_deinit()` inside `init_subjects()` (co-located pattern).

---

## Project-Specific Patterns

### 1. Single-panel architecture — no NavigationManager
No panel navigation system, overlay stack, or modals. `HomePanel` is the only panel. The setup overlay is a one-off `lv_xml_create("setup_overlay")` created/destroyed imperatively in `main.cpp`. When adding new UI: either extend the existing panel, or create one-off overlays like the setup screen — do not introduce a navigation framework.

### 2. Profile list is built imperatively
`populate_profile_list()` (`home_panel.cpp:165-237`) does `lv_obj_clean()` + recreates all buttons programmatically. Uses `lv_obj_set_user_data()` to store `const AnnealrProfile*` pointers, and `lv_obj_add_event_cb()` for click handlers. This is correct for dynamic lists — no XML template for list items.

### 3. C-style chart wrapper
`ui_temp_graph.h` exposes a struct-based C API (`anneal_temp_graph_t*`, `anneal_temp_graph_create()`, draw callbacks). LVGL's `lv_chart` requires manual data push via `lv_chart_set_next_value()` / `lv_chart_set_all_values()`, and axis labels/grid lines are rendered via `LV_EVENT_DRAW_PART_BEGIN` / `LV_EVENT_DRAW_POST` callbacks. No XML equivalent for chart series or draw customization. Managed manually in `HomePanel` via member variables (`graph_`, `planned_series_`, `actual_series_`).

### 4. Fixed-buffer string subjects
All string subjects use pre-allocated `char buf[N]` arrays with `std::strncpy` + explicit null termination in set positions. No dynamic heap strings. Pattern:
```cpp
std::strncpy(state_buf_, value.c_str(), sizeof(state_buf_) - 1);
state_buf_[sizeof(state_buf_) - 1] = '\0';
lv_subject_copy_string(&state_, state_buf_);
```
Buffers live as member variables of the owning class. Sizes: `state_buf_[32]`, `profile_name_buf_[64]`, `stage_label_buf_[128]`, `status_text_buf_[512]`.

### 5. Partial status updates with fallback-to-current
`AnnealrState::update_from_status()` (`annealr_state.cpp:285-390`) expects Moonraker partial updates (only changed fields). It reads current values as defaults via `data.value("field", current_value_from_buffer_or_subject)`. This prevents partial updates from resetting unrelated state. Pattern:
```cpp
std::string state_str = data.value("state", std::string(state_buf_));
```

### 6. Deci-degree storage for chart subjects
Temperature subjects store centidegrees (x10) as `lv_subject_t` int — e.g., 140.5°C = 1405. This gives 0.1°C precision in LVGL's integer chart API. Display text is separately formatted into string subjects with UTF-8 `°C` (`\xC2\xB0`). Chart pushing also uses raw float values (`push_temperature(temp_c, elapsed_s)`).

### 7. First-run setup flow entirely in main.cpp
When `settings.json` doesn't exist: copy template → show setup overlay → on save, destroy overlay + connect to Moonraker. All one-off imperative logic in `main.cpp:54-148` and `start_connection()` lambda. No dedicated SetupPanel class.

### 8. Font loading via LV_FONT_DECLARE + external C
MDI icons font (`mdi_icons_24`) is compiled from a C file in `assets/fonts/` with C linkage. Used via `extern "C" { LV_FONT_DECLARE(mdi_icons_24); }` in `home_panel.cpp:21`. Font C files are compiled separately and linked as objects (`FONT_SRCS` in Makefile). No `codepoints.h` or `make regen-fonts`.

### 9. Settings location
Settings file: `~/printer_data/config/annealscreen/settings.json` (Klipper ecosystem convention, matches `main.cpp:408`). Theme files: `<install_dir>/config/themes/dark.json` and `light.json`. Config template: `<install_dir>/config/settings.json.template`.

### 10. Klipper GCode commands
The app sends `ANNEALR_START PROFILE=<name>`, `ANNEALR_PAUSE`, `ANNEALR_RESUME`, `ANNEALR_CANCEL` via Moonraker's `printer.gcode.script` JSON-RPC method. Profile name from `selected_profile_buf_` is URL-unsafe (may contain spaces/hyphens) but sent raw in the GCode — annealr's GCode parser handles this.

### 11. CI vs Dockerfile
`.github/workflows/release.yml` uses `container: debian:12` + inline `apt-get install` for the same dependencies as `docker/Dockerfile.pi`. The Dockerfile exists for local cross-compilation (`make pi-docker`). CI does NOT build from the Dockerfile — it duplicates the dependency list inline. Keep both synchronized when changing build deps.

---

## Where Things Live

**Singletons** (all `::instance()`):
`AnnealrState` (all annealr data/subjects), `SettingsManager` (persistent settings), `ThemeManager` (color palette), `UpdateQueue` (thread-safe UI updates), `StaticPanelRegistry`, `StaticSubjectRegistry`

**Entry flow**: `main.cpp` → init LVGL → init XML → load theme → init subjects → create HomePanel → connect to Moonraker → event loop

**Key directories**:
| Path | Contents |
|------|----------|
| `include/` | All headers — flat dir |
| `src/` | Main entry + singletons: `main.cpp`, `annealr_state.cpp`, `theme_manager.cpp`, `settings_manager.cpp` |
| `src/ui/` | Panel and widget implementations: `home_panel.cpp`, `ui_temp_graph.cpp` |
| `src/moonraker/` | WebSocket JSON-RPC client: `moonraker_client.cpp` |
| `src/system/` | Crash handler: `crash_handler.cpp` |
| `ui_xml/` | XML layouts (loaded at runtime): `globals.xml`, `home_panel.xml`, `setup_overlay.xml` |
| `assets/fonts/` | Compiled font `.c` files |
| `config/` | Default config files, theme JSON |
| `config/themes/` | Dark and light color palette JSON files |
| `docker/` | Cross-compilation Dockerfile |
| `scripts/` | Installer (`install.sh`), release packager (`package.sh`) |

**Runtime config** (on device): `~/printer_data/config/annealscreen/settings.json`

**XML files registerable set** (from `main.cpp:437-443`):
- `A:<xml_base>/globals.xml` → components + consts
- `A:<xml_base>/home_panel.xml` → `home_panel` component
- `A:<xml_base>/setup_overlay.xml` → `setup_overlay` component

---

## Debugging

**NEVER debug without flags!** Use `-vv` minimum.
Trust debug output. Impossible values = bug is UPSTREAM. Ask "what ELSE?" not "did first fix work?"

**Crash files**: Written to `~/annealscreen/crash.txt` — signal name, fault address, registers (PC/SP/LR/FP), backtrace (glibc `backtrace()` + frame-pointer walk fallback), UpdateQueue callback tag ring, version, uptime. Reported via spdlog on next startup.

---

## Critical Paths (always MAJOR work)

AnnealrState (WebSocket status → subject updates), MoonrakerClient (connection lifecycle), UpdateQueue (thread-safe UI updates), XML layout changes (coordinate with C++ subject registrations)
