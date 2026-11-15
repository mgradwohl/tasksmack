#pragma once

// What a refusal offers beside its line (#3812): "Retry as administrator" (#2308), where the helper can redo it
// as an administrator, or "Restart as administrator..." (#3658, #3669), where there is no helper but TaskSmack can
// restart elevated. One rule (decideRefusalOffer()), one value type that holds the offer and the line's countdown
// (RefusalOffer), and one draw for the pair (drawRefusalOffer()), so every view that offers them follows the same
// #3658/#3669 rules, and a change to them is made here. Everything here runs on the UI thread.

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/RestartAsAdministratorButton.h"

#include <imgui.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <utility>

namespace App
{

/// Which of the pair a refusal offers: neither, "Retry as administrator", or "Restart as administrator...".
enum class RefusalOfferKind : std::uint8_t
{
    None,
    Retry,
    Restart,
};

/// The rule (#2308, #3658, #3669): a refusal for want of permission (@p refusedForPermission) offers "Retry as
/// administrator" when the helper can redo it (@p helperCanRedo) and there is something to redo (@p retryable),
/// and otherwise, with no helper, "Restart as administrator..." where TaskSmack can restart elevated
/// (@p restartOffered, RestartAsAdministrator::offered()). Nothing for any other failure: an administrator
/// couldn't do better.
[[nodiscard]] constexpr RefusalOfferKind
decideRefusalOffer(bool refusedForPermission, bool retryable, bool helperCanRedo, bool restartOffered) noexcept
{
    if (refusedForPermission && retryable && helperCanRedo)
    {
        return RefusalOfferKind::Retry;
    }
    if (RestartAsAdministrator::offersRestart(refusedForPermission, helperCanRedo, restartOffered))
    {
        return RefusalOfferKind::Restart;
    }
    return RefusalOfferKind::None;
}

/// One view's offer after a refused action, with what "Retry as administrator" redoes (@p Retry: the action and
/// what the view needs to run it again), and the countdown of the result line it stands beside. A line that
/// offers either button stays HOLD_SECONDS, time to read it and decide; when the countdown runs out the offer
/// goes with the line. A view whose line has no timeout never calls tick().
template<typename Retry> class RefusalOffer
{
  public:
    /// How long a line that offers either button stays up (Detail::AUTHORIZE_RESULT_SECONDS).
    static constexpr float HOLD_SECONDS = Detail::AUTHORIZE_RESULT_SECONDS;

    /// A new result line, up for @p seconds: the last line's offer goes.
    void showLine(float seconds) noexcept
    {
        clear();
        m_SecondsLeft = seconds;
    }

    /// Keeps the line up for @p seconds from now, with no offer of its own (Open file location's Copy, #3669).
    void holdLine(float seconds) noexcept
    {
        m_SecondsLeft = seconds;
    }

    /// After a result: decides what it offers (decideRefusalOffer(), retryable when @p retry has a value) and
    /// holds it, with @p retry for "Retry as administrator". A line offering either stays HOLD_SECONDS.
    RefusalOfferKind offer(bool refusedForPermission,
                           std::optional<Retry> retry,
                           bool helperCanRedo,
                           bool restartOffered = RestartAsAdministrator::offered())
    {
        m_Kind = decideRefusalOffer(refusedForPermission, retry.has_value(), helperCanRedo, restartOffered);
        if (m_Kind == RefusalOfferKind::Retry)
        {
            m_Retry = std::move(retry);
        }
        else
        {
            m_Retry.reset();
        }
        if (m_Kind != RefusalOfferKind::None)
        {
            m_SecondsLeft = HOLD_SECONDS;
        }
        return m_Kind;
    }

    /// Drops the offer (a new edit, another process selected); the line's countdown stays.
    void clear() noexcept
    {
        m_Retry.reset();
        m_Kind = RefusalOfferKind::None;
    }

    /// Takes the retry "Retry as administrator" redoes, dropping the offer: a refusal to the administrator isn't
    /// offered again. Nullopt, with the offer left as it is, when "Retry as administrator" isn't offered.
    [[nodiscard]] std::optional<Retry> takeRetry()
    {
        if (m_Kind != RefusalOfferKind::Retry)
        {
            return std::nullopt;
        }
        m_Kind = RefusalOfferKind::None;
        return std::exchange(m_Retry, std::nullopt);
    }

    /// Counts the line down by @p deltaSeconds. True on the frame it runs out: the offer has gone, and the view
    /// drops its line.
    bool tick(float deltaSeconds) noexcept
    {
        if (m_SecondsLeft <= 0.0F)
        {
            return false;
        }
        m_SecondsLeft -= std::max(0.0F, deltaSeconds);
        if (m_SecondsLeft > 0.0F)
        {
            return false;
        }
        m_SecondsLeft = 0.0F;
        clear();
        return true;
    }

    /// Whether the line's countdown is still running.
    [[nodiscard]] bool lineShown() const noexcept
    {
        return m_SecondsLeft > 0.0F;
    }

    /// What is offered, whatever the view's state.
    [[nodiscard]] RefusalOfferKind kind() const noexcept
    {
        return m_Kind;
    }

    /// What the line shows: nothing while @p busy (the retry, or another action, runs), and no "Retry as
    /// administrator" while @p retryHeld (the view says why it can't run now, #3429, #3467).
    [[nodiscard]] RefusalOfferKind shown(bool busy, bool retryHeld = false) const noexcept
    {
        if (busy || (m_Kind == RefusalOfferKind::Retry && retryHeld))
        {
            return RefusalOfferKind::None;
        }
        return m_Kind;
    }

    /// The retry "Retry as administrator" would redo, or null when none is offered.
    [[nodiscard]] const Retry* retry() const noexcept
    {
        return m_Retry.has_value() ? &*m_Retry : nullptr;
    }

  private:
    std::optional<Retry> m_Retry;
    float m_SecondsLeft = 0.0F;
    RefusalOfferKind m_Kind = RefusalOfferKind::None;
};

/// How drawRefusalOffer() draws "Retry as administrator": its label and tooltip (a read's "Read as administrator",
/// an I/O priority's own tooltip), and whether the button follows the last item on its line.
struct RefusalOfferButton
{
    const char* retryLabel = AuthorizeWording::RETRY_LABEL;
    const char* retryTooltip = AuthorizeWording::RETRY_TOOLTIP;
    bool sameLine = false;
};

/// The label of the button @p kind offers, drawn as @p button says: for a line that sizes its room before it draws.
/// Null for None.
[[nodiscard]] constexpr const char* refusalOfferLabel(RefusalOfferKind kind, const RefusalOfferButton& button = {}) noexcept
{
    switch (kind)
    {
    case RefusalOfferKind::Retry:
        return button.retryLabel;
    case RefusalOfferKind::Restart:
        return RestartAsAdministrator::LABEL;
    case RefusalOfferKind::None:
        break;
    }
    return nullptr;
}

/// Draws the button @p kind offers: "Retry as administrator", whose press calls @p onRetry, or "Restart as
/// administrator..." (RestartAsAdministrator::drawButton(), which raises the notice). Nothing for None. True when
/// it was pressed this frame.
template<std::invocable OnRetry> bool drawRefusalOffer(RefusalOfferKind kind, OnRetry onRetry, const RefusalOfferButton& button = {})
{
    if (kind == RefusalOfferKind::None)
    {
        return false;
    }
    if (button.sameLine)
    {
        ImGui::SameLine();
    }
    if (kind == RefusalOfferKind::Restart)
    {
        return RestartAsAdministrator::drawButton(); // no helper here: the restart instead (#3658, #3669)
    }
    // The lock and the words, never colour alone (#2308).
    const bool pressed = ImGui::SmallButton(button.retryLabel);
    if (pressed)
    {
        onRetry();
    }
    ImGui::SetItemTooltip("%s", button.retryTooltip);
    return pressed;
}

} // namespace App
