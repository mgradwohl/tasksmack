#pragma once

// The Startup tab's actions (#801, phase 2), shaped like the Services tab's (ServiceActionsView):
// Enable / Disable in each row's context menu and in an action bar for the selected row, a confirm for
// Disable (the shared ProcessActionConfirm dialog, centred), and the result line. An action runs on a
// worker thread of its own (std::async), never on the UI or the sampler thread. The pure helpers make
// no ImGui calls.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace App
{

/// One action on one entry, captured when it is asked for: the confirm and the worker act on this
/// copy, never on whatever row is selected by then.
struct StartupActionRequest
{
    bool enable = false;
    Platform::StartupEntry entry;
};

/// What the result line shows.
struct StartupActionMessage
{
    bool ok = false;
    std::string text; ///< Empty when there is nothing to show.
};

namespace StartupActionsDetail
{

/// How long a finished action's result line stays up, in seconds (as on the Services tab).
inline constexpr float RESULT_SECONDS = 8.0F;

/// Why no action applies to @p entry at all: "" when one may, else the reason the menu and the bar
/// show ("Requires administrator" for an all-users entry when not elevated; RunOnce has no state).
[[nodiscard]] std::string_view blockedReason(const Platform::StartupEntry& entry, const Platform::StartupActionCapabilities& caps);

/// Whether enabling (@p enable) or disabling @p entry applies: it isn't blocked and isn't already so.
[[nodiscard]] bool isApplicable(bool enable, const Platform::StartupEntry& entry, const Platform::StartupActionCapabilities& caps);

/// Disable asks first; Enable runs straight away.
[[nodiscard]] constexpr bool needsConfirm(const StartupActionRequest& request) noexcept
{
    return !request.enable;
}

[[nodiscard]] std::string confirmTitle(const StartupActionRequest& request);
[[nodiscard]] std::string confirmQuestion(const StartupActionRequest& request);

/// "Disabling OneDrive..." while it runs.
[[nodiscard]] std::string progressText(const StartupActionRequest& request);

/// "Disabled OneDrive", or "Could not disable OneDrive: Requires administrator".
[[nodiscard]] StartupActionMessage resultMessage(const StartupActionRequest& request, const Platform::StartupActionResult& result);

} // namespace StartupActionsDetail

/// The actions' state between frames, owned by StartupPanel; drawn from StartupView. Used on the UI
/// thread only: the worker gets its own copy of the request and a shared_ptr to the actions, and hands
/// back only its result, through the future.
class StartupActionsView
{
  public:
    explicit StartupActionsView(std::shared_ptr<Platform::IStartupActions> actions);
    /// Waits for an action still running (one registry write).
    ~StartupActionsView();

    StartupActionsView(const StartupActionsView&) = delete;
    StartupActionsView& operator=(const StartupActionsView&) = delete;
    StartupActionsView(StartupActionsView&&) = delete;
    StartupActionsView& operator=(StartupActionsView&&) = delete;

    /// Whether the platform has the actions: false hides the bar and the menu (Linux for now).
    [[nodiscard]] bool supported() const noexcept
    {
        return m_Capabilities.canSetEnabled;
    }

    /// Asks to confirm @p request, or runs it straight away when it needs no confirm. Ignored while
    /// another action runs or a confirm is up.
    void request(StartupActionRequest request);

    /// Takes in a finished action's result, without waiting. True on the frame one finished, so the
    /// caller can have the list re-read.
    [[nodiscard]] bool takeFinished();

    [[nodiscard]] bool busy() const noexcept
    {
        return m_Worker.valid();
    }

    /// Counts the result line down by @p deltaSeconds.
    void tick(float deltaSeconds) noexcept;

    /// The bar above the table: the buttons for @p selected (null: none selected; all disabled).
    void renderActionBar(const Platform::StartupEntry* selected);
    /// The row's right-click menu; call right after the row's item.
    void renderContextMenu(const Platform::StartupEntry& entry);
    /// The progress or result line, when there is one.
    void renderResultLine() const;
    /// The confirm dialog while one is pending; call every frame outside the table.
    void renderConfirmation();

    [[nodiscard]] bool confirmRequested() const noexcept
    {
        return m_ShowConfirm;
    }
    [[nodiscard]] const std::optional<StartupActionRequest>& pendingConfirm() const noexcept
    {
        return m_Confirm;
    }
    [[nodiscard]] const StartupActionMessage& lastResult() const noexcept
    {
        return m_Result;
    }

  private:
    void run(StartupActionRequest request);

    std::shared_ptr<Platform::IStartupActions> m_Actions;
    Platform::StartupActionCapabilities m_Capabilities;
    std::optional<StartupActionRequest> m_Confirm;
    bool m_ShowConfirm = false;
    std::string m_ConfirmTitle;
    std::string m_ConfirmQuestion;
    std::future<Platform::StartupActionResult> m_Worker;
    StartupActionRequest m_Running;
    StartupActionMessage m_Result;
    float m_ResultSecondsLeft = 0.0F;
};

} // namespace App
