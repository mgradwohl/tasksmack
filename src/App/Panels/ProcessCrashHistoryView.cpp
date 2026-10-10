#include "ProcessCrashHistoryView.h"

#include "Domain/CrashHistory.h"
#include "Domain/ProcessModules.h"
#include "Domain/SamplingConfig.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ThreadName.h"
#include "SystemInfoSections.h"
#include "UI/Format.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace App
{

namespace Detail
{

namespace
{

/// "2026-10-02 14:44": a crash's local time to the minute; empty when unknown.
[[nodiscard]] std::string crashTime(std::uint64_t unixSeconds)
{
    std::string text = UI::Format::formatEpochDateTime(unixSeconds);
    constexpr std::size_t MINUTES = 16; // "YYYY-MM-DD HH:MM"
    if (text.size() > MINUTES)
    {
        text.resize(MINUTES);
    }
    return text;
}

[[nodiscard]] std::string plural(std::size_t count, std::string_view one, std::string_view many)
{
    return std::format("{} {}", count, count == 1 ? one : many);
}

/// How @p executable was matched, for the tooltip.
[[nodiscard]] std::string matchNote(const Platform::CrashesInfo& crashes, std::string_view name, bool truncatedName)
{
    if (crashes.family == Platform::OsFamily::Windows)
    {
        return std::format("Matched by file name, ignoring case: {}", name);
    }
    if (crashes.family == Platform::OsFamily::Linux && truncatedName)
    {
        return std::format("Matched on the first {} characters of {}: core dumps are named by the process's comm, which the "
                           "kernel cuts to {}, so another program with the same start would match too",
                           Domain::LINUX_COMM_MAX_BYTES,
                           name,
                           Domain::LINUX_COMM_MAX_BYTES);
    }
    return std::format("Matched by name: {}", name);
}

} // namespace

CrashLine crashLineFor(const Domain::CrashHistorySnapshot& history, std::string_view executable)
{
    CrashLine line;
    if (history.version == 0)
    {
        line.visible = true;
        line.value = "Reading...";
        return line;
    }
    const Platform::CrashesInfo& crashes = history.crashes;
    if (!crashes.available)
    {
        return line; // no crash history on this platform: no line at all
    }
    line.visible = true;
    const std::string readAt = history.readAtUnixSeconds != 0 ? crashTime(history.readAtUnixSeconds) : std::string{};
    const std::string readNote = readAt.empty() ? std::string{} : std::format("\nRead at {}.", readAt);
    if (!crashes.listed)
    {
        line.value = crashes.accessDenied ? "Not readable without more permission" : "Unavailable";
        line.tooltip =
            (crashes.unavailableReason.empty() ? std::string("The crash history couldn't be read.") : crashes.unavailableReason) + readNote;
        return line;
    }

    const std::string_view name = Domain::Modules::fileName(executable);
    const Domain::ExecutableCrashCounts counts = Domain::crashCountsFor(crashes, name);
    std::string tooltip;
    if (counts.total() == 0)
    {
        line.value = std::format("None in the last {} days", Platform::CRASH_HISTORY_DAYS);
    }
    else
    {
        line.tone = CrashLineTone::Warning;
        std::string what = plural(counts.crashes, "crash", "crashes");
        if (counts.hangs > 0)
        {
            what = counts.crashes > 0 ? what + ", " + plural(counts.hangs, "hang", "hangs") : plural(counts.hangs, "hang", "hangs");
        }
        line.value = std::format("{} in the last {} days", what, Platform::CRASH_HISTORY_DAYS);
        if (const std::string last = crashTime(counts.lastUnixSeconds); !last.empty())
        {
            line.value += std::format(" (last {})", last);
        }
        for (const Platform::CrashEvent& event : counts.recent)
        {
            const std::string when = crashTime(event.unixSeconds);
            tooltip += std::format(
                "{}  {}\n", when.empty() ? std::string_view("Unknown time") : std::string_view(when), SystemInfo::formatCrashValue(event));
        }
        if (counts.total() > counts.recent.size())
        {
            tooltip += std::format("...and {} more\n", counts.total() - counts.recent.size());
        }
        tooltip += '\n';
    }
    tooltip += matchNote(crashes, name, counts.truncatedName);
    if (crashes.capped)
    {
        tooltip +=
            std::format("\nOnly the system's {} newest crashes and hangs were read; older ones aren't counted.", Platform::CRASH_LIST_MAX);
    }
    tooltip += "\nThe System tab's Recent crashes section lists every program's." + readNote;
    line.tooltip = std::move(tooltip);
    return line;
}

} // namespace Detail

bool ProcessCrashHistoryView::update(float deltaSeconds)
{
    if (m_Pending.valid() && m_Pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        takePending();
    }
    const bool drawn = std::exchange(m_Drawn, false);
    if (!m_History)
    {
        return false;
    }
    m_SecondsSinceFresh += deltaSeconds;
    if (const std::uint64_t version = m_History->version(); version != m_SeenVersion)
    {
        // A newer read, Process Details' own or the System tab's: fresh from now.
        m_SeenVersion = version;
        m_SecondsSinceFresh = 0.0F;
    }
    if (!drawn || m_Pending.valid())
    {
        return false;
    }
    const bool firstRead = m_SeenVersion == 0 && !m_Attempted;
    const bool stale = (m_SecondsSinceFresh * 1000.0F) >= static_cast<float>(Domain::Sampling::PROCESS_CRASH_HISTORY_REFRESH_MS);
    if (!firstRead && !stale)
    {
        return false;
    }
    m_Attempted = true;
    m_SecondsSinceFresh = 0.0F; // a read that fails to start is retried at the next interval
    try
    {
        m_Pending = std::async(std::launch::async,
                               [history = m_History]
                               {
                                   // Best effort, as for the System Information read: a pooled thread may keep the name.
                                   static_cast<void>(Platform::setCurrentThreadName(Platform::CRASH_HISTORY_READ_THREAD_NAME));
                                   history->read();
                               });
    }
    catch (const std::system_error& e)
    {
        spdlog::warn("Process Details: couldn't start a crash history read: {}", e.what());
        return false;
    }
    return true;
}

void ProcessCrashHistoryView::finishPendingRead()
{
    if (m_Pending.valid())
    {
        takePending();
    }
}

void ProcessCrashHistoryView::takePending()
{
    try
    {
        m_Pending.get();
    }
    catch (...)
    {
        // CrashHistory::read() publishes a failed read itself, so only publishing it (bad_alloc) lands
        // here. The exception object is not read: it was thrown on the worker (#1685).
        spdlog::warn("Process Details: a crash history read failed");
    }
}

const Detail::CrashLine& ProcessCrashHistoryView::line(std::string_view executable)
{
    if (!m_History)
    {
        m_Line = {};
        m_LineSnapshot.reset();
        return m_Line;
    }
    // The version first, without locking, so a frame with nothing new takes no snapshot.
    const bool sameRead = m_LineSnapshot && m_LineSnapshot->version == m_History->version();
    if (!sameRead || executable != m_LineExecutable)
    {
        std::shared_ptr<const Domain::CrashHistorySnapshot> snapshot = m_History->snapshot();
        m_Line = Detail::crashLineFor(*snapshot, executable);
        m_LineExecutable.assign(executable);
        m_LineSnapshot = std::move(snapshot);
    }
    return m_Line;
}

void ProcessCrashHistoryView::render(std::string_view executable)
{
    if (!m_History)
    {
        return;
    }
    const Detail::CrashLine& crashLine = line(executable);
    if (!crashLine.visible)
    {
        return;
    }
    markDrawn();

    const auto& scheme = UI::Theme::get().scheme();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Recent crashes:");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, crashLine.tone == Detail::CrashLineTone::Warning ? scheme.textWarning : scheme.textMuted);
    ImGui::TextUnformatted(crashLine.value.c_str());
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    if (!crashLine.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip())
    {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0F);
        ImGui::TextUnformatted(crashLine.tooltip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace App
