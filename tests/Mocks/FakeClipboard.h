/// @file FakeClipboard.h
/// @brief An in-memory clipboard for headless ImGui tests (#1890).
///
/// A new ImGui context on Windows points its clipboard at the real system clipboard, which every
/// process on the machine shares: a parallel test run, a clipboard manager or the user copying can
/// overwrite it between a test's copy and its read. Install this in the fixture, right after
/// ImGui::CreateContext(), and ImGui::SetClipboardText()/GetClipboardText() use this object instead.

#pragma once

#include <imgui.h>

#include <string>

namespace TestMocks
{

class FakeClipboard
{
  public:
    FakeClipboard() = default;
    ~FakeClipboard() = default;
    // The context holds a pointer to this object, so it stays where it is.
    FakeClipboard(const FakeClipboard&) = delete;
    FakeClipboard& operator=(const FakeClipboard&) = delete;
    FakeClipboard(FakeClipboard&&) = delete;
    FakeClipboard& operator=(FakeClipboard&&) = delete;

    /// Points the current ImGui context's clipboard at this object, which must outlive the context.
    void install()
    {
        ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
        platformIO.Platform_ClipboardUserData = this;
        platformIO.Platform_SetClipboardTextFn = &FakeClipboard::setText;
        platformIO.Platform_GetClipboardTextFn = &FakeClipboard::getText;
    }

    [[nodiscard]] const std::string& text() const
    {
        return m_Text;
    }

  private:
    // ImGui calls these for the current context, the one install() was called on.
    static FakeClipboard& self()
    {
        return *static_cast<FakeClipboard*>(ImGui::GetPlatformIO().Platform_ClipboardUserData);
    }

    static void setText(ImGuiContext* /*ctx*/, const char* text)
    {
        self().m_Text = (text == nullptr) ? std::string{} : std::string(text);
    }

    static const char* getText(ImGuiContext* /*ctx*/)
    {
        return self().m_Text.c_str();
    }

    std::string m_Text;
};

} // namespace TestMocks
