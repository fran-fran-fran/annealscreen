# AnnealScreen

Standalone LVGL touchscreen UI for the [annealr](https://github.com/YOUR_USERNAME/annealr) Klipper plugin. Controls PID-based annealing and drying runs from a dedicated touchscreen display.

Built from the ground up following [HelixScreen](https://github.com/prestonbrown/helixscreen) architecture patterns, reusing its XML engine and infrastructure while removing all printing-specific code.

## Features

- **Profile list** with descriptions and estimated durations, loaded from Klipper config
- **Reactive controls** (Start/Pause/Resume/Cancel) that show/hide based on run state via LVGL subject bindings
- **Temperature chart** with planned profile curve overlay and live actual temperature trace
- **Status display** with stage label, progress, and elapsed time
- **XML-driven layout** — change the UI by editing XML files, no rebuild needed
- **Theme support** — dark and light themes via JSON color palette files

## Screenshots

*Coming soon — run with `--test` to see the UI without a Moonraker connection.*

## Requirements

- **Hardware:** Raspberry Pi (3B+/4/5) with a touchscreen (800×480 reference, any resolution works)
- **Software:** Klipper + Moonraker + annealr plugin installed
- **Build tools:** GCC 10+, make, cmake (for libhv), SDL2 (for desktop development)

## Quick Start

### Desktop development (SDL)

```bash
# Clone with submodules
git clone --recursive https://github.com/YOUR_USERNAME/annealscreen.git
cd annealscreen

# Build (requires SDL2 dev packages)
# Ubuntu/Debian: sudo apt install libsdl2-dev
# macOS: brew install sdl2
make -j

# Run in test mode (no Moonraker needed)
./build/bin/annealscreen --test -vv
```

### Install on Raspberry Pi

```bash
# Clone on the Pi
git clone --recursive https://github.com/YOUR_USERNAME/annealscreen.git
cd annealscreen

# Build natively
make -j

# Install (creates systemd service, stops competing UIs)
./scripts/install.sh
```

### Cross-compile for Pi

```bash
# Build on your desktop, deploy to Pi
make pi-docker
scp build/bin/annealscreen pi@your-pi:~/annealscreen/bin/
```

## Configuration

### Settings

Edit `~/annealscreen/config/settings.json`:

```json
{
  "moonraker_host": "localhost",
  "moonraker_port": 7125,
  "heater_name": "annealer",
  "dark_mode": true
}
```

Or configure via the setup overlay on first launch, or via CLI:

```bash
annealscreen --host 192.168.1.100 --port 7125 -v
```

### Klipper config

AnnealScreen reads profiles from your `printer.cfg`:

```ini
[annealr]
heater: annealer

[annealr_profile PLA-Anneal]
description: Standard PLA annealing
segments:
    ramp, 60, rate=2.0
    soak, 30
    ramp, 90, rate=1.5
    soak, 120
    ramp, 50, rate=0.5
```

## Customizing the UI

All layout is defined in XML files under `ui_xml/`. Edit them and restart the service to see changes — no rebuild needed.

### Layout files

| File | Purpose |
|------|---------|
| `ui_xml/globals.xml` | Design tokens: fonts, styles, shared definitions |
| `ui_xml/home-panel.xml` | Main panel: profile list, controls, chart, info bar |
| `ui_xml/setup-overlay.xml` | First-run setup: Moonraker host/port, heater name |

### Themes

Color palettes are JSON files in `config/themes/`. Edit `dark.json` or `light.json` to change colors:

```json
{
  "colors": {
    "primary": "#6C63FF",
    "card_bg": "#25253E",
    "text": "#E8E8F0"
  }
}
```

### Layout principles

The layout uses LVGL's flex system with proportional sizing:

- **`width="30%"`** — sidebar takes 30% of screen width
- **`style_flex_grow="1"`** — chart and profile list expand to fill remaining space
- **No hardcoded pixel sizes** except minimum touch targets (42px button height)
- **Design tokens** — use `#space_md`, `#card_bg`, `font_body` instead of pixel values or hex colors

## Architecture

```
annealscreen/
├── include/           C++ headers
│   ├── annealr_state.h        Domain state singleton (LVGL subjects)
│   ├── moonraker_client.h     WebSocket JSON-RPC client
│   ├── home_panel.h           Main panel (list + controls + chart)
│   ├── settings_manager.h     JSON config persistence
│   ├── theme_manager.h        Color palette loading
│   ├── ui_update_queue.h      Thread-safe UI update queue
│   ├── ui_observer_guard.h    RAII observer wrapper
│   ├── observer_factory.h     Type-safe observer creation
│   └── static_subject_registry.h  Shutdown cleanup registry
├── src/               C++ source
├── ui_xml/            XML layouts (editable, no rebuild needed)
├── config/            Settings and theme JSON files
├── lib/               Dependencies (git submodules + helix-xml)
│   ├── lvgl/          LVGL 9.5
│   ├── helix-xml/     XML engine (MIT, extracted from LVGL 9.4)
│   ├── libhv/         WebSocket client
│   └── spdlog/        Logging
├── docker/            Cross-compilation toolchain
└── scripts/           Installer
```

### Key patterns (from HelixScreen)

- **DATA in C++, APPEARANCE in XML, Subjects connect them.** C++ owns state and registers LVGL subjects. XML binds widgets to subjects via `bind_text`, `bind_flag_if_eq`, etc.
- **Thread safety:** WebSocket callbacks run on a background thread. All LVGL mutations go through `anneal::ui::queue_update()` to execute on the main thread before rendering.
- **RAII observers:** `ObserverGuard` auto-removes observers on destruction. `observe_int_sync` and `observe_string` defer callbacks via the update queue.
- **Subject init order:** Register XML components → init subjects → create XML objects.
- **Self-registering cleanup:** Each singleton registers its own `deinit` callback in `StaticSubjectRegistry` inside `init_subjects()`.

## CLI Options

```
annealscreen [options]

  --test, -t       Run without Moonraker connection (mock mode)
  --host HOST      Override Moonraker hostname
  --port PORT      Override Moonraker port
  -v/-vv/-vvv      Log verbosity: info / debug / trace
  --help, -h       Show help
```

## License

GPL-3.0-or-later

Dependencies:
- LVGL: MIT
- helix-xml: MIT (extracted from LVGL by HelixScreen)
- libhv: BSD-3-Clause
- spdlog: MIT
