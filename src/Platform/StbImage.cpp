// stb_image's implementation, compiled once for every user of <stb_image.h>: UI/IconLoader.cpp (the
// app's own icons) and Platform/Linux/LinuxProcessIconSource.cpp (applications' PNG icons, #1800).
// And stb_image_write's, for Core/FramebufferCapture.cpp (the TASKSMACK_SCREENSHOT PNGs, #2827).
// In its own translation unit so neither layer holds the other's third-party code.

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define STB_IMAGE_IMPLEMENTATION
#ifdef _WIN32
// stbi_load() takes a UTF-8 file name and opens it with _wfopen, never through the code page (#1648).
#define STBI_WINDOWS_UTF8
#endif
// NOLINTNEXTLINE(misc-include-cleaner) - stb_image.h macros generate scanner errors
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef _WIN32
// stbi_write_png() likewise takes a UTF-8 file name and opens it with _wfopen (#2827).
#define STBIW_WINDOWS_UTF8
#endif
// NOLINTNEXTLINE(misc-include-cleaner) - stb_image_write.h macros generate scanner errors
#include <stb_image_write.h>

#ifdef __clang__
#pragma clang diagnostic pop
#endif
