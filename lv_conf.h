// SPDX-License-Identifier: GPL-3.0-or-later
// AnnealScreen LVGL configuration
// Minimal feature set for annealing controller UI

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// ── Color ───────────────────────────────────────────────────────────────
#define LV_COLOR_DEPTH 32
#define LV_COLOR_MIX_ROUND_OFS 0

// ── Memory ──────────────────────────────────────────────────────────────
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (256 * 1024)
#define LV_MALLOC  lv_malloc_builtin
#define LV_REALLOC lv_realloc_builtin
#define LV_FREE    lv_free_builtin
#define LV_MEMSET  lv_memset_builtin
#define LV_MEMCPY  lv_memcpy_builtin
#define LV_STRLEN  lv_strlen_builtin
#define LV_STRNCPY lv_strncpy_builtin

// ── HAL ─────────────────────────────────────────────────────────────────
#define LV_TICK_CUSTOM 0
#define LV_DEF_REFR_PERIOD 33

// ── Display ─────────────────────────────────────────────────────────────
#define LV_USE_DRAW_SW 1
#if LV_USE_DRAW_SW == 1
    #define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
    #define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0
#endif
#define LV_USE_DRAW_VGLITE 0
#define LV_USE_DRAW_PXP 0
#define LV_USE_DRAW_DAVE2D 0
#define LV_USE_DRAW_SDL 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_DRAW_VG_LITE 0

// ── OS / Threading ──────────────────────────────────────────────────────
#define LV_USE_OS LV_OS_NONE

// ── Logging ─────────────────────────────────────────────────────────────
// We use spdlog, not LVGL's built-in logging
#define LV_USE_LOG 0

// ── Asserts ─────────────────────────────────────────────────────────────
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

// ── Core Features ───────────────────────────────────────────────────────
#define LV_USE_OBJ_NAME    1
#define LV_USE_OBSERVER    1
#define LV_USE_OBJ_PROPERTY 0

// XML is provided by lib/helix-xml, not LVGL core (removed in v9.5)
#define LV_USE_XML 0

// ── Layouts ─────────────────────────────────────────────────────────────
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

// ── Widgets ─────────────────────────────────────────────────────────────
#define LV_USE_LABEL       1
#define LV_USE_BUTTON      1
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_DROPDOWN    1
#define LV_USE_ROLLER      0
#define LV_USE_SLIDER      0
#define LV_USE_SWITCH      0
#define LV_USE_TEXTAREA    1
#define LV_USE_TABLE       0
#define LV_USE_CHECKBOX    0
#define LV_USE_BAR         1
#define LV_USE_LINE        1
#define LV_USE_ARC         1
#define LV_USE_IMAGE       1
#define LV_USE_SPINNER     1
#define LV_USE_CHART       0
#define LV_USE_CANVAS      0
#define LV_USE_SCALE       0
#define LV_USE_CALENDAR    0
#define LV_USE_KEYBOARD    1
#define LV_USE_LIST        0
#define LV_USE_MENU        0
#define LV_USE_MSGBOX      0
#define LV_USE_SPAN        0
#define LV_USE_TABVIEW     0
#define LV_USE_TILEVIEW    0
#define LV_USE_WIN         0
#define LV_USE_LED         0
#define LV_USE_ANIMIMG     0
#define LV_USE_SPINBOX     0
#define LV_USE_IMAGEBUTTON 0

// ── Text ────────────────────────────────────────────────────────────────
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_DEFAULT &lv_font_montserrat_16

// Disable unused built-in fonts
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0

// ── Themes ──────────────────────────────────────────────────────────────
#define LV_USE_THEME_DEFAULT    1
#define LV_USE_THEME_SIMPLE     0
#define LV_USE_THEME_MONO       0

// ── SDL display driver ──────────────────────────────────────────────────
#ifdef ANNEAL_DISPLAY_SDL
    #define LV_USE_SDL 1
    #if LV_USE_SDL
        #define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
        #define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_DIRECT
        #define LV_SDL_BUF_COUNT        1
        #define LV_SDL_FULLSCREEN       0
        #define LV_SDL_DEFAULT_DISPLAY_WIDTH  800
        #define LV_SDL_DEFAULT_DISPLAY_HEIGHT 480
    #endif
#else
    #define LV_USE_SDL 0
#endif

// ── Linux framebuffer driver ────────────────────────────────────────────
#ifdef ANNEAL_DISPLAY_FBDEV
    #define LV_USE_LINUX_FBDEV 1
    #if LV_USE_LINUX_FBDEV
        #define LV_LINUX_FBDEV_BSD           0
        #define LV_LINUX_FBDEV_RENDER_MODE   LV_DISPLAY_RENDER_MODE_PARTIAL
        #define LV_LINUX_FBDEV_BUFFER_COUNT  1
        #define LV_LINUX_FBDEV_BUFFER_SIZE   60
    #endif
#else
    #define LV_USE_LINUX_FBDEV 0
#endif

// ── Linux evdev (touch input) ───────────────────────────────────────────
#ifdef ANNEAL_DISPLAY_FBDEV
    #define LV_USE_EVDEV 1
#else
    #define LV_USE_EVDEV 0
#endif

// ── File system ─────────────────────────────────────────────────────────
// helix-xml uses 'A:' drive for XML file loading
#define LV_USE_FS_POSIX 1
#if LV_USE_FS_POSIX
    #define LV_FS_POSIX_LETTER 'A'
    #define LV_FS_POSIX_CACHE_SIZE 0
#endif

// ── Misc ────────────────────────────────────────────────────────────────
#define LV_USE_SNAPSHOT  0
#define LV_USE_SYSMON    0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_MONKEY    0
#define LV_USE_GRIDNAV   0
#define LV_USE_FRAGMENT  0
#define LV_USE_IMGFONT   0
#define LV_USE_IME_PINYIN 0
#define LV_USE_FILE_EXPLORER 0
#define LV_USE_BARCODE   0
#define LV_USE_QRCODE    0
#define LV_USE_GIF       0

// ── Build info ──────────────────────────────────────────────────────────
#define LV_USE_BUILTIN_MALLOC  1
#define LV_USE_BUILTIN_MEMCPY  1
#define LV_USE_BUILTIN_SNPRINTF 1
#define LV_USE_BUILTIN_STRLEN  1

#endif // LV_CONF_H
