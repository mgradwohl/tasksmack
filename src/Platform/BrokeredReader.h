#pragma once

// Process Details' reads that an administrator can finish (#2309), one template for every kind (#3782): environment,
// modules, open files, connections, threads and the memory map. BrokeredReader<Traits> does what
// BrokeredProcessActions does for the actions: read in-process first; a PermissionDenied read goes through the helper
// at once while it runs, and otherwise is returned for the UI to offer "Authenticate to read" (readAuthorized()). The
// helper reads with its capabilities, with the same identity check (PID and start time). A new read kind is a traits
// struct below and a decorator in BrokeredProcessActions.h that forwards its interface's four overrides.

#include "Platform/BrokerProtocol.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"

#include <concepts>
#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Platform
{

/// What a read through the helper says when the helper couldn't be asked: this, then why.
inline constexpr std::string_view HELPER_COULDNT_BE_ASKED = "The administrator helper couldn't be asked: ";
/// What a read through the helper says when the helper answered with another kind of reply.
inline constexpr std::string_view HELPER_UNEXPECTED_ANSWER = "The administrator helper gave an unexpected answer";

/// Process Details' environment (#2309). The helper's reply carries no detail of its own (BrokerProtocol), so a
/// failure's detail is the reason the helper couldn't be asked.
struct EnvironmentReadTraits
{
    using Reader = IProcessEnvironmentReader;
    using Result = EnvironmentReadResult;
    using Request = Broker::EnvironmentRequest;
    using Reply = Broker::EnvironmentReply;

    static constexpr EnvironmentReadStatus PERMISSION_DENIED = EnvironmentReadStatus::PermissionDenied;

    [[nodiscard]] static bool has(const Reader& reader)
    {
        return reader.hasEnvironment();
    }
    [[nodiscard]] static Result readInProcess(Reader& reader, const ProcessTarget& target)
    {
        return reader.readEnvironment(target);
    }
    /// The refusal that stands when the prompt is dismissed or the helper couldn't start.
    [[nodiscard]] static Result refused()
    {
        return {.status = EnvironmentReadStatus::PermissionDenied, .variables = {}};
    }
    /// A read the helper didn't answer: @p detail says why, and the view draws it after the status line (#4423).
    [[nodiscard]] static Result failed(std::string detail)
    {
        return {.status = EnvironmentReadStatus::Failed, .variables = {}, .truncated = false, .detail = std::move(detail)};
    }
};

/// Process Details' modules (#2309).
struct ModulesReadTraits
{
    using Reader = IProcessModulesReader;
    using Result = ModulesReadResult;
    using Request = Broker::ModulesRequest;
    using Reply = Broker::ModulesReply;

    static constexpr ModulesReadStatus PERMISSION_DENIED = ModulesReadStatus::PermissionDenied;

    [[nodiscard]] static bool has(const Reader& reader)
    {
        return reader.hasModules();
    }
    [[nodiscard]] static Result readInProcess(Reader& reader, const ProcessTarget& target)
    {
        return reader.readModules(target);
    }
    [[nodiscard]] static Result refused()
    {
        return {.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}};
    }
    [[nodiscard]] static Result failed(std::string detail)
    {
        return {.status = ModulesReadStatus::Failed, .modules = {}, .detail = std::move(detail)};
    }
};

/// Process Details' and Find handles' open files (#2309, #3578). BrokeredOpenFilesReader adds the batch.
struct OpenFilesReadTraits
{
    using Reader = IProcessOpenFilesReader;
    using Result = OpenFilesReadResult;
    using Request = Broker::OpenFilesRequest;
    using Reply = Broker::OpenFilesReply;

    static constexpr OpenFilesReadStatus PERMISSION_DENIED = OpenFilesReadStatus::PermissionDenied;

    [[nodiscard]] static bool has(const Reader& reader)
    {
        return reader.hasOpenFiles();
    }
    [[nodiscard]] static Result readInProcess(Reader& reader, const ProcessTarget& target)
    {
        return reader.readOpenFiles(target);
    }
    [[nodiscard]] static Result refused()
    {
        Result refused;
        refused.status = OpenFilesReadStatus::PermissionDenied;
        return refused;
    }
    [[nodiscard]] static Result failed(std::string detail)
    {
        Result failed;
        failed.status = OpenFilesReadStatus::Failed;
        failed.detail = std::move(detail);
        return failed;
    }
};

/// Process Details' connections (#2309).
struct ConnectionsReadTraits
{
    using Reader = IProcessConnectionsReader;
    using Result = ConnectionsReadResult;
    using Request = Broker::ConnectionsRequest;
    using Reply = Broker::ConnectionsReply;

    static constexpr ConnectionsReadStatus PERMISSION_DENIED = ConnectionsReadStatus::PermissionDenied;

    [[nodiscard]] static bool has(const Reader& reader)
    {
        return reader.hasConnections();
    }
    [[nodiscard]] static Result readInProcess(Reader& reader, const ProcessTarget& target)
    {
        return reader.readConnections(target);
    }
    [[nodiscard]] static Result refused()
    {
        return {.status = ConnectionsReadStatus::PermissionDenied, .connections = {}, .detail = {}};
    }
    [[nodiscard]] static Result failed(std::string detail)
    {
        return {.status = ConnectionsReadStatus::Failed, .connections = {}, .detail = std::move(detail)};
    }
};

/// Process Details' threads (#3069).
struct ThreadsReadTraits
{
    using Reader = IProcessThreadsReader;
    using Result = ThreadsReadResult;
    using Request = Broker::ThreadsRequest;
    using Reply = Broker::ThreadsReply;

    static constexpr ThreadsReadStatus PERMISSION_DENIED = ThreadsReadStatus::PermissionDenied;

    [[nodiscard]] static bool has(const Reader& reader)
    {
        return reader.hasThreads();
    }
    [[nodiscard]] static Result readInProcess(Reader& reader, const ProcessTarget& target)
    {
        return reader.readThreads(target);
    }
    [[nodiscard]] static Result refused()
    {
        return {.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}};
    }
    [[nodiscard]] static Result failed(std::string detail)
    {
        return {.status = ThreadsReadStatus::Failed, .threads = {}, .detail = std::move(detail)};
    }
};

/// Process Details' memory map (#3665): the helper reads the region list only, never the process's memory.
struct MemoryMapReadTraits
{
    using Reader = IProcessMemoryMapReader;
    using Result = MemoryMapReadResult;
    using Request = Broker::MemoryMapRequest;
    using Reply = Broker::MemoryMapReply;

    static constexpr MemoryMapReadStatus PERMISSION_DENIED = MemoryMapReadStatus::PermissionDenied;

    [[nodiscard]] static bool has(const Reader& reader)
    {
        return reader.hasMemoryMap();
    }
    [[nodiscard]] static Result readInProcess(Reader& reader, const ProcessTarget& target)
    {
        return reader.readMemoryMap(target);
    }
    [[nodiscard]] static Result refused()
    {
        return {.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}, .truncated = false};
    }
    [[nodiscard]] static Result failed(std::string detail)
    {
        return {.status = MemoryMapReadStatus::Failed, .mappings = {}, .detail = std::move(detail), .truncated = false};
    }
};

/// What BrokeredReader needs of a read kind's traits.
template<typename Traits>
concept BrokeredReadTraits = requires(const Traits::Reader& reader, Traits::Reader& mutableReader, const ProcessTarget& target) {
    typename Traits::Request;
    typename Traits::Reply;
    { Traits::has(reader) } -> std::same_as<bool>;
    { Traits::readInProcess(mutableReader, target) } -> std::same_as<typename Traits::Result>;
    { Traits::refused() } -> std::same_as<typename Traits::Result>;
    { Traits::failed(std::string{}) } -> std::same_as<typename Traits::Result>;
};

/// One read kind's read that an administrator can finish (#2309, #3782), over the in-process reader and the broker.
/// It has no virtual members: each kind's decorator (BrokeredProcessActions.h) is a plain class that implements its
/// interface's overrides by forwarding to one of these, so no virtual member of a class template is left for a
/// compiler to instantiate or not.
template<BrokeredReadTraits Traits> class BrokeredReader
{
  public:
    using Reader = Traits::Reader;
    using Result = Traits::Result;

    BrokeredReader(std::unique_ptr<Reader> inner, std::shared_ptr<IPrivilegedBroker> broker)
        : m_Inner(std::move(inner)), m_Broker(std::move(broker))
    {}

    [[nodiscard]] bool has() const
    {
        return Traits::has(*m_Inner);
    }

    /// The in-process read; a PermissionDenied one goes through the helper at once while it runs.
    [[nodiscard]] Result read(const ProcessTarget& target)
    {
        Result result = Traits::readInProcess(*m_Inner, target);
        if (result.status != Traits::PERMISSION_DENIED || m_Broker == nullptr || !m_Broker->connected())
        {
            return result;
        }
        return viaBroker(target, std::stop_token{});
    }

    [[nodiscard]] bool canReadAuthorized() const
    {
        return m_Broker != nullptr && m_Broker->readsOtherUsers();
    }

    [[nodiscard]] Result readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
    {
        if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
        {
            return Traits::readInProcess(*m_Inner, target);
        }
        if (!helperConnected(stopToken))
        {
            return Traits::refused(); // the prompt was dismissed or the helper couldn't start: the refusal stands
        }
        return viaBroker(target, stopToken);
    }

    /// Starts the helper if it isn't running, after the password prompt, for a run of readAuthorized() calls (#4420): one
    /// prompt for them all. Nothing to start when no helper could read (canReadAuthorized() false): readAuthorized()
    /// reads in-process then. The error is connect()'s: why there is no helper.
    [[nodiscard]] std::expected<void, std::string> authorize(const std::stop_token& stopToken)
    {
        if (!canReadAuthorized() || m_Broker->connected())
        {
            return {};
        }
        return m_Broker->connect(stopToken);
    }

    /// Whether the helper is connected, connecting it if need be; false when the prompt was dismissed or the helper
    /// couldn't start, and the refusal stands.
    [[nodiscard]] bool helperConnected(const std::stop_token& stopToken)
    {
        return m_Broker->connected() || m_Broker->connect(stopToken).has_value();
    }

    /// The read through the helper; a failure to ask it is a Failed read.
    [[nodiscard]] Result viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
    {
        const std::expected<Broker::Response, std::string> response = m_Broker->call(typename Traits::Request{.target = target}, stopToken);
        if (!response.has_value())
        {
            return Traits::failed(std::string(HELPER_COULDNT_BE_ASKED) + response.error());
        }
        if (const auto* reply = std::get_if<typename Traits::Reply>(&*response))
        {
            return reply->result;
        }
        return Traits::failed(std::string(HELPER_UNEXPECTED_ANSWER));
    }

    [[nodiscard]] Reader& inner() noexcept
    {
        return *m_Inner;
    }
    [[nodiscard]] const Reader& inner() const noexcept
    {
        return *m_Inner;
    }
    [[nodiscard]] IPrivilegedBroker& privilegedBroker() noexcept
    {
        return *m_Broker;
    }

  private:
    std::unique_ptr<Reader> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

} // namespace Platform
