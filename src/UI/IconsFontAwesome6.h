// Font Awesome 6 Icons for TaskSmack
// Icon font: Font Awesome 6 Free Solid (fa-solid-900.ttf)
// Generated from IconFontCppHeaders: https://github.com/juliettef/IconFontCppHeaders
//
// Usage: Include this header and use ICON_FA_* constants in ImGui text strings
// Example: ImGui::Text(ICON_FA_BOLT " Power");
//
// Note: Icon string macros remain as #define because they need to support
// compile-time string literal concatenation (e.g., ICON_FA_BOLT " Power").
// C++ does not support constexpr string literal concatenation.

#pragma once

#include <cstdint>

// Font file name (relative to assets/fonts/)
inline constexpr const char* FONT_ICON_FILE_NAME_FAS = "fa-solid-900.ttf";

// Icon font Unicode range
inline constexpr uint32_t ICON_MIN_FA = 0xe005;
inline constexpr uint32_t ICON_MAX_FA = 0xf8ff;

// NOLINTBEGIN(cppcoreguidelines-macro-usage) - Macros required for string literal concatenation
// ============================================================================
// Battery & Power Icons
// ============================================================================
#define ICON_FA_BATTERY_FULL "\xef\x89\x80"           // U+f240 - Full battery
#define ICON_FA_BATTERY_THREE_QUARTERS "\xef\x89\x81" // U+f241 - 75% battery
#define ICON_FA_BATTERY_HALF "\xef\x89\x82"           // U+f242 - 50% battery
#define ICON_FA_BATTERY_QUARTER "\xef\x89\x83"        // U+f243 - 25% battery
#define ICON_FA_BATTERY_EMPTY "\xef\x89\x84"          // U+f244 - Empty battery
#define ICON_FA_BOLT "\xef\x83\xa7"                   // U+f0e7 - Lightning bolt (charging)
#define ICON_FA_PLUG "\xef\x87\xa6"                   // U+f1e6 - Power plug (AC)
#define ICON_FA_CHARGING_STATION "\xef\x97\xa7"       // U+f5e7 - Charging station

// ============================================================================
// Status & Info Icons
// ============================================================================
#define ICON_FA_CIRCLE_CHECK "\xef\x81\x98"         // U+f058 - Check in circle
#define ICON_FA_CIRCLE_EXCLAMATION "\xef\x81\xaa"   // U+f06a - Exclamation in circle
#define ICON_FA_CIRCLE_INFO "\xef\x81\x9a"          // U+f05a - Info in circle
#define ICON_FA_TRIANGLE_EXCLAMATION "\xef\x81\xb1" // U+f071 - Warning triangle
#define ICON_FA_CIRCLE_QUESTION "\xef\x81\x99"      // U+f059 - Question in circle

// ============================================================================
// System & Hardware Icons
// ============================================================================
#define ICON_FA_MICROCHIP "\xef\x8b\x9b"        // U+f2db - CPU/Microchip
#define ICON_FA_MEMORY "\xef\x94\xb8"           // U+f538 - RAM/Memory
#define ICON_FA_HARD_DRIVE "\xef\x82\xa0"       // U+f0a0 - Hard drive
#define ICON_FA_SERVER "\xef\x88\xb3"           // U+f233 - Server
#define ICON_FA_NETWORK_WIRED "\xef\x9b\xbf"    // U+f6ff - Network
#define ICON_FA_GAUGE_HIGH "\xef\x98\xa5"       // U+f625 - Performance gauge
#define ICON_FA_TEMPERATURE_HALF "\xef\x8b\x89" // U+f2c9 - Temperature

// ============================================================================
// Process & Task Icons
// ============================================================================
#define ICON_FA_LIST "\xef\x80\xba"  // U+f03a - List
#define ICON_FA_BARS "\xef\x83\x89"  // U+f0c9 - Menu bars
#define ICON_FA_GEAR "\xef\x80\x93"  // U+f013 - Settings gear
#define ICON_FA_GEARS "\xef\x82\x85" // U+f085 - Multiple gears
#define ICON_FA_SKULL "\xef\x95\x8c" // U+f54c - Kill process
#define ICON_FA_STOP "\xef\x81\x8d"  // U+f04d - Stop
#define ICON_FA_PLAY "\xef\x81\x8b"  // U+f04b - Play/Resume
#define ICON_FA_PAUSE "\xef\x81\x8c" // U+f04c - Pause/Suspend
#define ICON_FA_XMARK "\xef\x80\x8d" // U+f00d - Close/Terminate

// ============================================================================
// Arrow & Direction Icons
// ============================================================================
#define ICON_FA_ARROW_UP "\xef\x81\xa2"    // U+f062 - Arrow up
#define ICON_FA_ARROW_DOWN "\xef\x81\xa3"  // U+f063 - Arrow down
#define ICON_FA_ARROW_RIGHT "\xef\x81\xa1" // U+f061 - Arrow right
#define ICON_FA_ARROW_LEFT "\xef\x81\xa0"  // U+f060 - Arrow left
#define ICON_FA_SORT "\xef\x83\x9c"        // U+f0dc - Sort (up/down arrows)
#define ICON_FA_SORT_UP "\xef\x83\x9e"     // U+f0de - Sort ascending
#define ICON_FA_SORT_DOWN "\xef\x83\x9d"   // U+f0dd - Sort descending

// ============================================================================
// UI & Display Icons
// ============================================================================
#define ICON_FA_EXPAND "\xef\x81\xa5"           // U+f065 - Expand/fullscreen
#define ICON_FA_COMPRESS "\xef\x81\xa6"         // U+f066 - Compress
#define ICON_FA_MAGNIFYING_GLASS "\xef\x80\x82" // U+f002 - Search
#define ICON_FA_FILTER "\xef\x82\xb0"           // U+f0b0 - Filter
#define ICON_FA_EYE "\xef\x81\xae"              // U+f06e - View/visible
#define ICON_FA_EYE_SLASH "\xef\x81\xb0"        // U+f070 - Hidden
#define ICON_FA_CHART_LINE "\xef\x88\x81"       // U+f201 - Line chart
#define ICON_FA_CHART_BAR "\xef\x82\x80"        // U+f080 - Bar chart
#define ICON_FA_CHART_PIE "\xef\x88\x80"        // U+f200 - Pie chart

// ============================================================================
// Misc Icons
// ============================================================================
#define ICON_FA_CLOCK "\xef\x80\x97"          // U+f017 - Clock/time
#define ICON_FA_HOURGLASS_HALF "\xef\x89\x92" // U+f252 - Loading/waiting
#define ICON_FA_SPINNER "\xef\x84\x90"        // U+f110 - Spinner
#define ICON_FA_REFRESH "\xef\x80\xa1"        // U+f021 - Refresh (arrows rotate)
#define ICON_FA_ARROWS_ROTATE "\xef\x80\xa1"  // U+f021 - Refresh (same as above)
#define ICON_FA_COPY "\xef\x83\x85"           // U+f0c5 - Copy
#define ICON_FA_TRASH "\xef\x87\xb8"          // U+f1f8 - Delete/trash
#define ICON_FA_POWER_OFF "\xef\x80\x91"      // U+f011 - Power off

// ============================================================================
// File & Document Icons
// ============================================================================
#define ICON_FA_FILE "\xef\x85\x9b"        // U+f15b - File
#define ICON_FA_FILE_PEN "\xef\x8c\x9c"    // U+f31c - File with pen (edit)
#define ICON_FA_FOLDER "\xef\x81\xbb"      // U+f07b - Folder
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc" // U+f07c - Open folder

// ============================================================================
// Menu & Navigation Icons
// ============================================================================
#define ICON_FA_DOOR_OPEN "\xef\x94\xbb" // U+f52b - Exit/door
#define ICON_FA_WRENCH "\xef\x82\xad"    // U+f0ad - Tools/wrench
#define ICON_FA_PALETTE "\xef\x94\xbf"   // U+f53f - Theme/palette
#define ICON_FA_FONT "\xef\x80\xb1"      // U+f031 - Font
#define ICON_FA_PLUS "\x2b"              // U+002b - Plus sign
#define ICON_FA_MINUS "\x2d"             // U+002d - Minus sign

// ============================================================================
// Device Icons
// ============================================================================
#define ICON_FA_COMPUTER "\xef\x84\x88" // U+f108 - Desktop computer
#define ICON_FA_DESKTOP "\xef\x84\x88"  // U+f108 - Desktop (alias)
#define ICON_FA_ID_CARD "\xef\x8b\x82"  // U+f2c2 - ID card
#define ICON_FA_FAN "\xef\xa1\xa3"      // U+f863 - Fan
#define ICON_FA_VIDEO "\xef\x80\xbd"    // U+f03d - Video camera
// NOLINTEND(cppcoreguidelines-macro-usage)
