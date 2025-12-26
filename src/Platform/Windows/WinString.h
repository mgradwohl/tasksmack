#pragma once

#include <string>
#include <string_view>

namespace Platform::WinString
{

[[nodiscard]] std::string wideToUtf8(const wchar_t* wide);
[[nodiscard]] std::string wideToUtf8(const std::wstring& wide);
[[nodiscard]] std::wstring utf8ToWide(std::string_view utf8);

} // namespace Platform::WinString
