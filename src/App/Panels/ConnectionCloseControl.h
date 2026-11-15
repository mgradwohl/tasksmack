#pragma once

// Close connection (#3066): the Connections tab's row menu item, its confirm and its result line. As
// Process Details' actions do, the close runs on a worker of its own (ProcessActionRun, #2305), so the
// helper's round trip or its UAC prompt never holds the frame, and a permission refusal offers "Retry as
// administrator" (#2308): the helper closes it as an administrator. The retry is Windows': Linux closes it in TaskSmack
// itself (pidfd_getfd() and shutdown(), #3066), with no helper to retry a refusal (#3580).
//
// The control owns only its UI state; the IProcessActions it closes through stays the panel's (from the
// composition root) and is passed in, and the panel declares it before this control, so the control (and
// its worker, waited for) is destroyed first. Free of ImGui: the panel draws the confirm and the line.
// Tested in tests/App/test_Connections.cpp.

#include "App/Panels/ConnectionsView.h"
#include "App/Panels/ProcessActionRun.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/RefusalOffer.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"

#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>

namespace App
{

/// The confirm's question: which process's connection, between which endpoints, and what closing it does.
[[nodiscard]] inline std::string closeConnectionQuestion(const ConnectionRow& row)
{
    return std::format("Close the connection {} (PID {}) has from {} to {}?\n\n"
                       "Both directions are shut down: {} sees the connection end, as if the network had dropped it, "
                       "and the other end is told it closed. Data not yet sent is lost.",
                       row.process,
                       row.connection.pid,
                       row.local,
                       row.remote,
                       row.process);
}

class ConnectionCloseControl
{
  public:
    /// The row menu's "Close connection..." on @p row: the confirm opens, naming it. Ignored while a close
    /// runs, or for a socket that can't be closed (closeConnectionRefusal(), with @p key, #3358).
    void request(const ConnectionRow& row, Platform::ConnectionCloseKey key = Platform::ConnectionCloseKey::SocketInode)
    {
        if (busy() || !closeConnectionRefusal(row.connection, key).empty())
        {
            return;
        }
        m_Pending = row;
        m_Question = closeConnectionQuestion(row);
        m_ShowConfirm = true;
    }

    /// The row the confirm is up for, if any.
    [[nodiscard]] const std::optional<ConnectionRow>& pending() const noexcept
    {
        return m_Pending;
    }

    [[nodiscard]] const std::string& question() const noexcept
    {
        return m_Question;
    }

    /// The flag the confirm modal opens on, and clears when it closes (ProcessActionConfirm::renderLabelled()).
    [[nodiscard]] bool& showConfirm() noexcept
    {
        return m_ShowConfirm;
    }

    /// The confirm was cancelled: nothing is closed.
    void cancel()
    {
        m_Pending.reset();
        m_ShowConfirm = false;
    }

    /// The confirm was accepted: the pending connection is closed through @p actions, on the worker.
    void confirm(Platform::IProcessActions* actions)
    {
        if (!m_Pending.has_value())
        {
            return;
        }
        const ConnectionRow row = std::move(*m_Pending);
        m_Pending.reset();
        m_ShowConfirm = false;
        const Platform::ProcessActionCapabilities capabilities =
            actions != nullptr ? actions->actionCapabilities() : Platform::ProcessActionCapabilities{};
        if (!capabilities.canCloseConnections)
        {
            finish(row, Platform::ProcessActionResult::error("Closing connections isn't available"));
            return;
        }
        // By inode (Linux), or by the IPv4 TCP endpoints where Windows names a connection by them (#3358).
        const bool byEndpoints = capabilities.connectionCloseKey == Platform::ConnectionCloseKey::Tcp4Endpoints;
        Platform::AuthorizedAction retry;
        retry.kind = Platform::AuthorizedAction::Kind::CloseConnection;
        retry.target = {.pid = row.connection.pid, .startTimeTicks = row.connection.startTimeTicks};
        if (byEndpoints)
        {
            retry.tcpConnection = Platform::tcpConnectionToClose(row.connection.connection);
        }
        else
        {
            retry.socketInode = row.connection.socketInode;
        }
        m_RunRow = row;
        static_cast<void>(m_Run.start(
            std::format("Closing {}'s connection to {}...", row.process, row.remote),
            [actions, retry, byEndpoints]
            {
                return byEndpoints ? actions->closeTcpConnection(retry.target, retry.tcpConnection)
                                   : actions->closeConnection(retry.target, retry.socketInode);
            },
            [this, actions, retry](const Platform::ProcessActionResult& result)
            {
                finish(m_RunRow, result);
                // Retry as administrator through the helper, or with none, Restart as administrator... (#3669).
                static_cast<void>(m_Offer.offer(result.permissionDenied,
                                                Retry{.actions = actions, .action = retry},
                                                actions->actionCapabilities().canAuthorizeInteractively));
            }));
    }

    /// Whether the result line offers "Retry as administrator" (#2308; AuthorizeWording::RETRY_TEXT).
    [[nodiscard]] bool offersAuthenticateAndRetry() const noexcept
    {
        return m_Offer.shown(busy()) == RefusalOfferKind::Retry;
    }

    /// Whether the result line offers "Restart as administrator..." in its place (#3669): the close was refused
    /// for want of permission, with no helper to retry it, where TaskSmack can restart elevated.
    [[nodiscard]] bool offersRestartAsAdministrator() const noexcept
    {
        return m_Offer.shown(busy()) == RefusalOfferKind::Restart;
    }

    /// What the result line offers beside it now: Retry or Restart as administrator, or neither (drawRefusalOffer()).
    [[nodiscard]] RefusalOfferKind refusalOffer() const noexcept
    {
        return m_Offer.shown(busy());
    }

    /// "Retry as administrator" was pressed: the refused close again, by the helper as an administrator,
    /// after its UAC prompt (IProcessActions::runAuthorized()).
    void retryAuthorized()
    {
        if (busy())
        {
            return;
        }
        const std::optional<Retry> taken = m_Offer.takeRetry();
        if (!taken.has_value())
        {
            return;
        }
        const Retry& retry = *taken;
        static_cast<void>(m_Run.start(
            std::format("Waiting for authorization to close {}'s connection...", m_RunRow.process),
            [actions = retry.actions, action = retry.action](const std::stop_token& stopToken)
            { return actions->runAuthorized(action, stopToken); },
            // A refusal to the administrator isn't offered again.
            [this](const Platform::ProcessActionResult& result) { finish(m_RunRow, result); }));
    }

    /// Each frame: a finished close's result goes to the line, and the line goes after a while. @p wait
    /// blocks until a running close finishes (the tests' way to take it in).
    void update(float deltaSeconds, bool wait = false)
    {
        if (m_Run.update(wait))
        {
            return;
        }
        if (m_Offer.tick(deltaSeconds))
        {
            m_Result = {};
        }
    }

    [[nodiscard]] bool busy() const noexcept
    {
        return m_Run.busy();
    }

    /// What the line says while busy(): "Closing firefox's connection to 142.250.80.46:443...".
    [[nodiscard]] const std::string& pendingText() const noexcept
    {
        return m_Run.pendingText();
    }

    /// The last close's result line; empty when there is none to show.
    [[nodiscard]] const Detail::ActionResultMessage& result() const noexcept
    {
        return m_Result;
    }

    /// Whether a connection was closed since the last call: the panel then reads the table again at once.
    [[nodiscard]] bool takeClosed() noexcept
    {
        return std::exchange(m_Closed, false);
    }

    /// How long the result line stays, in seconds.
    static constexpr float RESULT_SECONDS = 6.0F;

  private:
    struct Retry
    {
        Platform::IProcessActions* actions = nullptr;
        Platform::AuthorizedAction action;
    };

    void finish(const ConnectionRow& row, const Platform::ProcessActionResult& result)
    {
        if (result.success)
        {
            m_Result = {.ok = true, .text = std::format("Closed {}'s connection to {}", row.process, row.remote)};
        }
        else
        {
            m_Result = {
                .ok = false,
                .text = std::format("Couldn't close {}'s connection to {}: {}", row.process, row.remote, result.errorMessage),
            };
        }
        m_Offer.showLine(RESULT_SECONDS); // the last close's offer goes with its line
        m_Closed = m_Closed || result.success;
    }

    std::optional<ConnectionRow> m_Pending;
    std::string m_Question;
    bool m_ShowConfirm = false;
    RefusalOffer<Retry> m_Offer; ///< The result line's countdown, and its Retry or Restart as administrator (#2308, #3669)
    ConnectionRow m_RunRow;      ///< The connection the running (or last) close is for
    Detail::ActionResultMessage m_Result;
    bool m_Closed = false;
    // Last, so it is destroyed (stopping and waiting for the worker) before the members its finish uses.
    ProcessActionRun m_Run;
};

} // namespace App
