// SPDX-License-Identifier: GPL-3.0-or-later
// AnnealScreen — LVGL touchscreen UI for the annealr Klipper plugin.
//
// Entry point: initializes LVGL, display backend, XML engine, subjects,
// theme, connects to Moonraker, and runs the event loop.

#include "annealr_state.h"
#include "crash_handler.h"
#include "home_panel.h"
#include "moonraker_client.h"
#include "settings_manager.h"
#include "static_panel_registry.h"
#include "static_subject_registry.h"
#include "theme_manager.h"
#include "ui_update_queue.h"

#include "helix-xml/helix_xml.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <lvgl.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifdef ANNEAL_DISPLAY_FBDEV
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/kd.h>
#endif

namespace fs = std::filesystem;

#ifndef ANNEAL_VERSION
#define ANNEAL_VERSION "0.1.0-dev"
#endif

// ── Globals ─────────────────────────────────────────────────────────────

static volatile sig_atomic_t g_should_quit = 0;
static anneal::MoonrakerClient* g_client = nullptr;

static void signal_handler(int sig) {
    (void)sig;
    g_should_quit = 1;
}

// ── Setup overlay logic ─────────────────────────────────────────────────

static lv_obj_t* g_setup_overlay = nullptr;
static lv_obj_t* g_keyboard = nullptr;

static void start_connection(const std::string& host, int port,
                              const std::string& heater);

static void on_setup_save(lv_event_t*) {
    if (!g_setup_overlay) return;

    lv_obj_t* host_ta   = lv_obj_find_by_name(g_setup_overlay, "setup_host");
    lv_obj_t* port_ta   = lv_obj_find_by_name(g_setup_overlay, "setup_port");
    lv_obj_t* heater_ta = lv_obj_find_by_name(g_setup_overlay, "setup_heater");

    std::string host   = host_ta   ? lv_textarea_get_text(host_ta)   : "localhost";
    std::string port_s = port_ta   ? lv_textarea_get_text(port_ta)   : "7125";
    std::string heater = heater_ta ? lv_textarea_get_text(heater_ta) : "annealer";

    if (host.empty()) host = "localhost";
    if (heater.empty()) heater = "annealer";
    int port = 7125;
    if (!port_s.empty()) {
        try { port = std::stoi(port_s); } catch (...) {}
    }

    auto& sm = anneal::SettingsManager::instance();
    sm.settings().moonraker_host = host;
    sm.settings().moonraker_port = port;
    sm.settings().heater_name    = heater;
    sm.save();

    // Remove setup overlay
    if (g_keyboard) {
        lv_obj_delete(g_keyboard);
        g_keyboard = nullptr;
    }
    lv_obj_delete(g_setup_overlay);
    g_setup_overlay = nullptr;

    start_connection(host, port, heater);
}

static void textarea_focused_cb(lv_event_t* e) {
    lv_obj_t* ta = lv_event_get_target_obj(e);
    if (!g_keyboard) {
        g_keyboard = lv_keyboard_create(lv_screen_active());
    }
    lv_keyboard_set_textarea(g_keyboard, ta);
    lv_obj_remove_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void textarea_defocused_cb(lv_event_t*) {
    if (g_keyboard) {
        lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void show_setup_overlay() {
    lv_xml_register_event_cb(nullptr, "on_setup_save", on_setup_save);

    g_setup_overlay = static_cast<lv_obj_t*>(
        lv_xml_create(lv_screen_active(), "setup_overlay", nullptr));
    if (!g_setup_overlay) {
        spdlog::error("[Main] Failed to create setup overlay");
        return;
    }

    // Wire keyboard to textareas
    for (const char* name : {"setup_host", "setup_port", "setup_heater"}) {
        lv_obj_t* ta = lv_obj_find_by_name(g_setup_overlay, name);
        if (ta) {
            lv_obj_add_event_cb(ta, textarea_focused_cb,
                                LV_EVENT_FOCUSED, nullptr);
            lv_obj_add_event_cb(ta, textarea_defocused_cb,
                                LV_EVENT_DEFOCUSED, nullptr);
        }
    }

    // Pre-fill with current settings
    auto& s = anneal::SettingsManager::instance().settings();
    lv_obj_t* host_ta = lv_obj_find_by_name(g_setup_overlay, "setup_host");
    if (host_ta && !s.moonraker_host.empty())
        lv_textarea_set_text(host_ta, s.moonraker_host.c_str());

    lv_obj_t* port_ta = lv_obj_find_by_name(g_setup_overlay, "setup_port");
    if (port_ta) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", s.moonraker_port);
        lv_textarea_set_text(port_ta, buf);
    }

    lv_obj_t* heater_ta = lv_obj_find_by_name(g_setup_overlay, "setup_heater");
    if (heater_ta && !s.heater_name.empty())
        lv_textarea_set_text(heater_ta, s.heater_name.c_str());
}

// ── Moonraker connection ────────────────────────────────────────────────

static void start_connection(const std::string& host, int port,
                              const std::string& heater) {
    auto& state = anneal::AnnealrState::instance();
    auto& home  = anneal::HomePanel::instance();

    if (!g_client) {
        g_client = new anneal::MoonrakerClient();
        home.set_client(g_client);
    }

    g_client->on_connected = [&state, heater]() {
        spdlog::info("[Main] Moonraker connected, subscribing...");

        // Subscribe to annealr status + heater temperature
        // null = subscribe to all fields for that object
        std::string heater_key = "heater_generic " + heater;
        nlohmann::json objects = {
            {"annealr", nullptr},
            {heater_key, nullptr}
        };
        g_client->subscribe(objects);

        // Query config for profiles
        g_client->query_config([&state](const nlohmann::json& config) {
            state.load_profiles_from_config(config.dump());
        });
    };

    g_client->on_disconnected = []() {
        spdlog::warn("[Main] Moonraker disconnected");
    };

    g_client->on_status_update = [&state, heater](const nlohmann::json& data) {
        // Route annealr status
        if (data.contains("annealr")) {
            state.update_from_status(data["annealr"].dump());
        }

        // Route heater temperature
        std::string heater_key = "heater_generic " + heater;
        if (data.contains(heater_key)) {
            const auto& h = data[heater_key];
            if (h.contains("temperature")) {
                float temp = h["temperature"].get<float>();
                float target = h.value("target", 0.0f);
                float elapsed = static_cast<float>(
                    lv_subject_get_int(state.run_elapsed_s_subject()));

                anneal::ui::queue_update([&state, temp, target, elapsed]() {
                    // Update chamber temp subject + formatted text
                    lv_subject_set_int(state.chamber_temp_subject(),
                                       static_cast<int>(temp * 10));
                    lv_subject_set_int(state.chamber_target_subject(),
                                       static_cast<int>(target * 10));
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""C", temp);
                    lv_subject_copy_string(state.chamber_temp_text_subject(), buf);

                    // Push to chart if run active
                    auto& home = anneal::HomePanel::instance();
                    if (state.is_run_active()) {
                        home.push_temperature(temp, elapsed);
                    }
                    home.update_chart_target(target);
                });

                // Record for temp history (thread-safe)
                if (state.is_run_active()) {
                    state.record_temperature(temp, elapsed);
                }
            }
        }
    };

    g_client->connect(host, port);
}

// ── Resolve XML base path ───────────────────────────────────────────────

static std::string find_xml_base() {
    // Search order: alongside binary, in source tree, in install location
    std::vector<std::string> candidates = {
        "ui_xml",                              // dev: cwd
        "../ui_xml",                           // dev: build/ subdir
        fs::path(getenv("HOME") ? getenv("HOME") : ".")
            .append("annealscreen/ui_xml").string(),
    };

    // Also check relative to the binary itself
    char exe_path[1024] = {0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        fs::path exe_dir = fs::path(exe_path).parent_path();
        candidates.insert(candidates.begin(),
                          (exe_dir / "../ui_xml").string());
    }

    for (const auto& p : candidates) {
        if (fs::exists(p) && fs::is_directory(p)) {
            return fs::canonical(p).string();
        }
    }
    return "ui_xml"; // fallback
}

static std::string find_config_dir() {
    std::vector<std::string> candidates = {
        "config",
        "../config",
        fs::path(getenv("HOME") ? getenv("HOME") : ".")
            .append("annealscreen/config").string(),
    };
    for (const auto& p : candidates) {
        if (fs::exists(p) && fs::is_directory(p))
            return fs::canonical(p).string();
    }
    return "config";
}

// ── Main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Parse CLI arguments
    bool test_mode = false;
    int verbosity = 0;
    std::string cli_host;
    int cli_port = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test" || arg == "-t") {
            test_mode = true;
        } else if (arg == "-v") {
            verbosity = 1;
        } else if (arg == "-vv") {
            verbosity = 2;
        } else if (arg == "-vvv") {
            verbosity = 3;
        } else if (arg == "--host" && i + 1 < argc) {
            cli_host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            try { cli_port = std::stoi(argv[++i]); } catch (...) {}
        } else if (arg == "--help" || arg == "-h") {
            printf("AnnealScreen — Touchscreen UI for annealr\n\n");
            printf("Usage: annealscreen [options]\n\n");
            printf("Options:\n");
            printf("  --test, -t       Mock mode (no Moonraker connection)\n");
            printf("  --host HOST      Moonraker hostname/IP\n");
            printf("  --port PORT      Moonraker port (default: 7125)\n");
            printf("  -v/-vv/-vvv      Verbosity (info/debug/trace)\n");
            printf("  --help, -h       Show this help\n");
            return 0;
        }
    }

    // Init logging
    auto logger = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(logger);
    switch (verbosity) {
        case 0:  spdlog::set_level(spdlog::level::warn);  break;
        case 1:  spdlog::set_level(spdlog::level::info);  break;
        case 2:  spdlog::set_level(spdlog::level::debug); break;
        default: spdlog::set_level(spdlog::level::trace); break;
    }

    spdlog::info("AnnealScreen starting (verbosity={})", verbosity);

    // Install crash handler (before anything that could crash)
    std::string crash_path = std::string(getenv("HOME") ? getenv("HOME") : ".") +
                             "/annealscreen/crash.txt";
    crash_handler::install(crash_path, ANNEAL_VERSION);

    // Check for crash from previous run
    if (crash_handler::has_crash_file(crash_path)) {
        crash_handler::report_and_remove(crash_path);
    }

    // Signal handling (SIGINT/SIGTERM for clean shutdown)
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Resolve paths
    std::string xml_base   = find_xml_base();
    std::string config_dir = find_config_dir();
    spdlog::info("XML base: {}", xml_base);
    spdlog::info("Config: {}", config_dir);

    // Init LVGL
    lv_init();

    // Create display
#ifdef ANNEAL_DISPLAY_SDL
    lv_display_t* disp = lv_sdl_window_create(800, 480);
    lv_indev_t* mouse = lv_sdl_mouse_create();
    (void)mouse;
    spdlog::info("Display: SDL 800x480");
#elif defined(ANNEAL_DISPLAY_FBDEV)
    lv_display_t* disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");
    // Touch input via evdev
    lv_indev_t* touch = lv_evdev_create(LV_INDEV_TYPE_POINTER,
                                         "/dev/input/event0");
    (void)touch;
    spdlog::info("Display: fbdev /dev/fb0");

    // Suppress Linux console cursor/text on framebuffer (HelixScreen pattern).
    // Switch VT to KD_GRAPHICS mode so kernel stops rendering console text.
    {
        static const char* tty_paths[] = {"/dev/tty0", "/dev/tty1", "/dev/tty", nullptr};
        int tty_fd = -1;
        for (int i = 0; tty_paths[i] != nullptr; ++i) {
            tty_fd = open(tty_paths[i], O_WRONLY | O_CLOEXEC);
            if (tty_fd >= 0) {
                if (ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) == 0) {
                    spdlog::info("Console suppressed via KD_GRAPHICS on {}", tty_paths[i]);
                    // Keep fd open for the lifetime of the process
                    break;
                }
                spdlog::debug("KDSETMODE failed on {}: {}", tty_paths[i], strerror(errno));
                close(tty_fd);
                tty_fd = -1;
            }
        }
        if (tty_fd < 0) {
            spdlog::warn("Could not suppress console cursor");
        }
    }
#else
    #error "Define ANNEAL_DISPLAY_SDL or ANNEAL_DISPLAY_FBDEV"
#endif
    (void)disp;

    // Init update queue (must be after lv_init, before UI creation)
    anneal::ui::update_queue_init();

    // Load theme
    auto& theme = anneal::ThemeManager::instance();
    std::string theme_path = config_dir + "/themes/dark.json";
    theme.load(theme_path);
    theme.apply();

    // Load settings from Klipper ecosystem config dir
    auto& settings = anneal::SettingsManager::instance();
    std::string home_dir = getenv("HOME") ? getenv("HOME") : ".";
    std::string settings_dir = home_dir + "/printer_data/config/annealscreen";
    std::string settings_path = settings_dir + "/settings.json";
    std::string template_path = config_dir + "/settings.json.template";

    // Track first run — settings file didn't exist before we created it
    bool first_run = !fs::exists(settings_path);

    // Create config dir and copy template on first run
    if (first_run) {
        try {
            fs::create_directories(settings_dir);
            if (fs::exists(template_path)) {
                fs::copy_file(template_path, settings_path);
                spdlog::info("First run: created {}", settings_path);
            }
        } catch (const fs::filesystem_error& e) {
            spdlog::warn("Could not create settings: {}", e.what());
        }
    }
    settings.load(settings_path);

    // Override settings from CLI
    if (!cli_host.empty()) settings.settings().moonraker_host = cli_host;
    if (cli_port > 0) settings.settings().moonraker_port = cli_port;

    // Register XML components and load globals
    // Must init XML subsystem first (LVGL 9.5 removed XML, helix-xml needs manual init)
    lv_xml_init();

    // LVGL's POSIX filesystem uses 'A:' drive prefix (LV_FS_POSIX_LETTER in lv_conf.h)
    std::string globals_path = "A:" + xml_base + "/globals.xml";
    lv_xml_register_component_from_file(globals_path.c_str());

    std::string home_panel_path = "A:" + xml_base + "/home_panel.xml";
    lv_xml_register_component_from_file(home_panel_path.c_str());

    std::string setup_path = "A:" + xml_base + "/setup_overlay.xml";
    lv_xml_register_component_from_file(setup_path.c_str());

    // Init domain state subjects (BEFORE XML creation)
    anneal::AnnealrState::instance().init_subjects();
    anneal::HomePanel::instance().init_subjects();

    // Create the home panel
    lv_obj_t* screen = lv_screen_active();
    anneal::HomePanel::instance().create(screen);

    // Connect to Moonraker or show setup
    if (test_mode) {
        spdlog::info("Running in test mode — no Moonraker connection");
    } else if (first_run) {
        spdlog::info("First run — showing setup overlay");
        show_setup_overlay();
    } else if (settings.has_valid_config()) {
        start_connection(settings.settings().moonraker_host,
                         settings.settings().moonraker_port,
                         settings.settings().heater_name);
    } else {
        show_setup_overlay();
    }

    // ── Event loop ──────────────────────────────────────────────────
    spdlog::info("Entering event loop");

    while (!g_should_quit) {
        uint32_t sleep_ms = lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::min(sleep_ms, 33u)));
    }

    // ── Shutdown ────────────────────────────────────────────────────
    spdlog::info("Shutting down");

    if (g_client) {
        g_client->disconnect();
        delete g_client;
        g_client = nullptr;
    }

    // Shutdown order (matches HelixScreen):
    // 1. Drain update queue (flush pending callbacks while panels live)
    // 2. Destroy panels (reverse registration order)
    // 3. Deinit subjects (disconnect observers before LVGL teardown)
    // 4. Invalidate all observer guards
    // 5. lv_deinit (LVGL cleanup)
    // 6. Uninstall crash handler
    anneal::ui::update_queue_shutdown();
    StaticPanelRegistry::instance().destroy_all();
    StaticSubjectRegistry::instance().deinit_all();
    ObserverGuard::invalidate_all();
    lv_deinit();
    crash_handler::uninstall();

    spdlog::info("Shutdown complete");
    return 0;
}