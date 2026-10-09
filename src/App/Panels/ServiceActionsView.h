#pragma once

// The Services tab's actions (#1577): Start / Stop / Restart / Startup type in each row's context
// menu and in an action bar for the selected row, the confirm for destructive ones (the shared
// ProcessActionConfirm dialog, centred), and the result line. An action runs on a worker thread of
// its own (std::async), never on the UI or the sampler thread: the platform call waits up to 10 s
// per state change. The pure helpers (which actions apply, the critical list, the texts) make no
// ImGui calls.

#include "Platform/IServiceActions.h"
#include "Platform/IServiceProbe.h"

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace App
{

enum class ServiceActionKind : std::uint8_t
{
    Start,
    Stop,
    Restart,
    SetStartType,
};

/// One action on one service, captured when it is asked for: the confirm and the worker act on this
/// copy, never on whatever row is selected by then.
struct ServiceActionRequest
{
    ServiceActionKind kind = ServiceActionKind::Start;
    std::string name;
    std::string displayName;
    Platform::ServiceStartType startType = Platform::ServiceStartType::Unknown; ///< SetStartType's target.
    bool critical = false;                                                      ///< isCriticalService() when asked.
};

/// What the result line shows.
struct ServiceActionMessage
{
    bool ok = false;
    std::string text; ///< Empty when there is nothing to show.
};

namespace ServiceActionsDetail
{

/// How long a finished action's result line stays up, in seconds.
inline constexpr float RESULT_SECONDS = 8.0F;

/// Whether stopping @p name (a short name, case-insensitive) or a service of @p serviceType ("Driver":
/// a kernel or file-system driver) can destabilise Windows: RpcSs, EventLog, Winmgmt and the like.
[[nodiscard]] bool isCriticalService(std::string_view name, std::string_view serviceType);

/// Whether @p kind applies to a service in @p state, and the platform can run it at all: Start only a
/// stopped service, Stop a running or paused one, Restart a running one; the start type always.
[[nodiscard]] bool isApplicable(ServiceActionKind kind, Platform::ServiceState state, const Platform::ServiceActionCapabilities& caps);

/// Whether @p request asks for a confirm first: Stop, Restart and setting Disabled.
[[nodiscard]] bool needsConfirm(const ServiceActionRequest& request);

[[nodiscard]] ServiceActionRequest makeRequest(ServiceActionKind kind,
                                               const Platform::ServiceInfo& service,
                                               Platform::ServiceStartType startType = Platform::ServiceStartType::Unknown);

/// The confirm's title, question (with the critical warning first for a critical service) and button.
[[nodiscard]] std::string confirmTitle(const ServiceActionRequest& request);
[[nodiscard]] std::string confirmQuestion(const ServiceActionRequest& request);
[[nodiscard]] const char* confirmLabel(const ServiceActionRequest& request);

/// "Stopping Spooler..." while it runs.
[[nodiscard]] std::string progressText(const ServiceActionRequest& request);

/// "Stopped Spooler", or "Could not stop Spooler: Requires administrator".
[[nodiscard]] ServiceActionMessage resultMessage(const ServiceActionRequest& request, const Platform::ServiceActionResult& result);

} // namespace ServiceActionsDetail

/// The actions' state between frames, owned by ServicesPanel; drawn from ServicesView. Used on the UI
/// thread only: the worker gets its own copy of the request and a shared_ptr to the actions, and hands
/// back only its result, through the future.
class ServiceActionsView
{
  public:
    explicit ServiceActionsView(std::shared_ptr<Platform::IServiceActions> actions);
    /// Waits for an action still running (bounded by the platform's wait, at most 20 s for a restart).
    ~ServiceActionsView();

    ServiceActionsView(const ServiceActionsView&) = delete;
    ServiceActionsView& operator=(const ServiceActionsView&) = delete;
    ServiceActionsView(ServiceActionsView&&) = delete;
    ServiceActionsView& operator=(ServiceActionsView&&) = delete;

    [[nodiscard]] const Platform::ServiceActionCapabilities& capabilities() const noexcept
    {
        return m_Capabilities;
    }

    /// Whether the platform has any action: false hides the bar and the menu (Linux for now).
    [[nodiscard]] bool supported() const noexcept;

    /// Asks to confirm @p request, or runs it straight away when it needs no confirm. Ignored while
    /// another action runs.
    void request(ServiceActionRequest request);

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
    void renderActionBar(const Platform::ServiceInfo* selected);
    /// The row's right-click menu; call right after the row's item.
    void renderContextMenu(const Platform::ServiceInfo& service);
    /// The progress or result line, when there is one.
    void renderResultLine() const;
    /// The confirm dialog while one is pending; call every frame outside the table.
    void renderConfirmation();

    [[nodiscard]] bool confirmRequested() const noexcept
    {
        return m_ShowConfirm;
    }
    [[nodiscard]] const std::optional<ServiceActionRequest>& pendingConfirm() const noexcept
    {
        return m_Confirm;
    }
    [[nodiscard]] const ServiceActionMessage& lastResult() const noexcept
    {
        return m_Result;
    }

  private:
    void run(ServiceActionRequest request);
    /// The Start / Stop / Restart items or buttons and the start-type choices, for @p service.
    void renderStartTypeItems(const Platform::ServiceInfo& service);

    std::shared_ptr<Platform::IServiceActions> m_Actions;
    Platform::ServiceActionCapabilities m_Capabilities;
    std::optional<ServiceActionRequest> m_Confirm;
    bool m_ShowConfirm = false;
    std::string m_ConfirmTitle;
    std::string m_ConfirmQuestion;
    std::future<Platform::ServiceActionResult> m_Worker;
    ServiceActionRequest m_Running;
    ServiceActionMessage m_Result;
    float m_ResultSecondsLeft = 0.0F;
};

} // namespace App
