# Precompiled headers for TaskSmack
# This file is included from the main CMakeLists.txt when TASKSMACK_ENABLE_PCH is ON

set(TASKSMACK_PCH_HEADERS
    # GL headers first to prevent conflicts with system headers
    <glad/gl.h>
    # Standard library headers
    <string>
    <vector>
    <memory>
    <algorithm>
    <functional>
    <optional>
    <variant>
    <span>
    <format>
    # Third-party headers
    <spdlog/spdlog.h>
)
