#include "UdpByteCounter.h"

#include "Platform/Linux/PosixGuards.h"
#include "Platform/UdpByteCounts.h"
#include "UdpBytesBpfObject.h" // generated: bpf/udp_bytes.bpf.c, compiled and embedded (cmake/BpfObject.cmake)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/libbpf_version.h>
#include <fcntl.h>

// libbpf 1.0 made a failed open or attach return null (and set errno), which the checks below rely on; before it,
// they returned error pointers that aren't null (CMakeLists.txt requires 1.0 too).
static_assert(LIBBPF_MAJOR_VERSION >= 1, "UdpByteCounter needs libbpf 1.0 or newer");

namespace Platform
{

namespace
{

constexpr std::string_view OBJECT_NAME = "udp_bytes";
constexpr std::string_view MAP_NAME = "udp_bytes_by_tgid";
/// The map's max_entries in bpf/udp_bytes.bpf.c: a read returns at most this many processes.
constexpr std::size_t MAP_ENTRIES = 16384;

constexpr std::array<UdpAttachAttempt, 4> ATTEMPTS{
    UdpAttachAttempt{.mode = UdpAttachMode::Fexit, .ipv6 = true},
    UdpAttachAttempt{.mode = UdpAttachMode::Fexit, .ipv6 = false},
    UdpAttachAttempt{.mode = UdpAttachMode::Kretprobe, .ipv6 = true},
    UdpAttachAttempt{.mode = UdpAttachMode::Kretprobe, .ipv6 = false},
};

// The map's value, as the BPF program lays it out.
struct MapValue
{
    std::uint64_t tx = 0;
    std::uint64_t rx = 0;
};
static_assert(sizeof(MapValue) == 16);

std::string errnoText(int error)
{
    return std::error_code(error, std::generic_category()).message();
}

/// This thread's libbpf warnings while a UdpLibbpfLog::Capture is open (#3634): owned here, so no caller's address
/// is stored.
thread_local std::string t_CaptureText; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): load()'s own
thread_local bool t_Capturing = false;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the same

// libbpf's own messages go to the debug log rather than stderr: a failed fexit attempt is expected on older
// kernels, and the kretprobe fallback then loads. During an attempt its warnings are also kept, so a failure can
// say what the verifier refused.
[[gnu::format(printf, 2, 0)]] int libbpfPrint(libbpf_print_level level, const char* format, va_list args)
{
    if (level == LIBBPF_DEBUG)
    {
        return 0;
    }
    // Formatted whole, however long: the verifier's log comes as one message, and its last line says why.
    std::va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, copy); // NOLINT(cert-err33-c): the length is checked
    va_end(copy);
    if (length <= 0)
    {
        return 0;
    }
    try
    {
        std::string line(static_cast<std::size_t>(length) + 1, '\0');
        static_cast<void>(std::vsnprintf(line.data(), line.size(), format, args));
        line.resize(static_cast<std::size_t>(length));
        std::string_view text(line);
        while (text.ends_with('\n'))
        {
            text.remove_suffix(1);
        }
        spdlog::debug("libbpf: {}", text);
        UdpLibbpfLog::note(text);
    }
    catch (...) // NOLINT(bugprone-empty-catch): the log is a courtesy; an allocation failure only loses it
    {}
    return 0;
}

struct ObjectCloser
{
    void operator()(bpf_object* object) const noexcept
    {
        bpf_object__close(object);
    }
};
using ObjectPtr = std::unique_ptr<bpf_object, ObjectCloser>;

struct LinkDestroyer
{
    void operator()(bpf_link* link) const noexcept
    {
        bpf_link__destroy(link);
    }
};
using LinkPtr = std::unique_ptr<bpf_link, LinkDestroyer>;

/// Whether the program in @p section runs when its function returns (fexit, kretprobe) rather than on entry.
[[nodiscard]] bool isReturnProgram(std::string_view section) noexcept
{
    return section.starts_with("fexit/") || section.starts_with("kretprobe/");
}

} // namespace

void UdpLibbpfLog::keep(std::string& capture, std::string_view line)
{
    capture.append(line).push_back('\n');
    if (capture.size() > CAPTURE_BYTES)
    {
        capture.erase(0, capture.size() - CAPTURE_BYTES);
    }
}

UdpLibbpfLog::Capture::Capture() noexcept
{
    t_CaptureText.clear();
    t_Capturing = true;
}

UdpLibbpfLog::Capture::~Capture()
{
    t_Capturing = false;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): read through the open capture on purpose
const std::string& UdpLibbpfLog::Capture::text() const noexcept
{
    return t_CaptureText;
}

void UdpLibbpfLog::note(std::string_view line)
{
    if (t_Capturing)
    {
        keep(t_CaptureText, line);
    }
}

std::string UdpLibbpfLog::withLog(std::string reason, std::string_view captured)
{
    while (captured.ends_with('\n'))
    {
        captured.remove_suffix(1);
    }
    if (captured.empty())
    {
        return reason;
    }
    if (captured.size() > QUOTED_BYTES)
    {
        captured.remove_prefix(captured.size() - QUOTED_BYTES);
    }
    return std::format("{}; libbpf: ...{}", reason, captured);
}

std::span<const UdpAttachAttempt> udpAttachAttempts() noexcept
{
    return ATTEMPTS;
}

bool udpProgramWanted(std::string_view section, const UdpAttachAttempt& attempt) noexcept
{
    // Each mode's entry and return programs.
    const std::array<std::string_view, 2> prefixes = attempt.mode == UdpAttachMode::Fexit
                                                       ? std::array<std::string_view, 2>{"fentry/", "fexit/"}
                                                       : std::array<std::string_view, 2>{"kprobe/", "kretprobe/"};
    const auto* const prefix = std::ranges::find_if(prefixes, [section](std::string_view p) { return section.starts_with(p); });
    if (prefix == prefixes.end())
    {
        return false;
    }
    std::string_view function = section;
    function.remove_prefix(prefix->size()); // starts_with above: no throwing substr in a noexcept function
    if (function.starts_with("udpv6_"))
    {
        return attempt.ipv6;
    }
    // skb_consume_udp: the kprobe mode's receives, IPv4's and IPv6's alike.
    return function.starts_with("udp_") || function == "skb_consume_udp";
}

// Links are destroyed before the object closes: they're declared after it.
// Only the links and the totals map's own descriptor outlive load(): the object, its programs' descriptors and its
// other maps are closed once the programs attach (the links keep them attached, and the kernel keeps what they
// use). in_udpv6_send holds kernel socket addresses, so a helper that still held it could read kernel pointers
// through the map reads its sandbox allows (#3554).
struct UdpByteCounter::State
{
    std::vector<LinkPtr> links;
    Posix::FdGuard map{-1};
    UdpAttachAttempt attempt;
};

UdpByteCounter::UdpByteCounter(std::unique_ptr<State> state) noexcept : m_State(std::move(state))
{}

UdpByteCounter::~UdpByteCounter() = default;
UdpByteCounter::UdpByteCounter(UdpByteCounter&& other) noexcept = default;
UdpByteCounter& UdpByteCounter::operator=(UdpByteCounter&& other) noexcept = default;

std::expected<UdpByteCounter, std::string> UdpByteCounter::load()
{
    return load(ATTEMPTS);
}

std::expected<UdpByteCounter, std::string> UdpByteCounter::load(std::span<const UdpAttachAttempt> attempts)
{
    libbpf_set_print(libbpfPrint);

    std::string lastError = "no attach mode was tried";
    for (const UdpAttachAttempt& attempt : attempts)
    {
        const char* modeName = attempt.mode == UdpAttachMode::Fexit ? "fexit" : "kretprobe";
        const std::string label = std::format("{}{}", modeName, attempt.ipv6 ? "" : " (IPv4 only)");

        bpf_object_open_opts options{};
        options.sz = sizeof(options);
        const std::string objectName(OBJECT_NAME);
        options.object_name = objectName.c_str();
        const UdpLibbpfLog::Capture capture; // this attempt's libbpf warnings, for its failure's reason
        ObjectPtr object(bpf_object__open_mem(UDP_BYTES_BPF_OBJECT.data(), UDP_BYTES_BPF_OBJECT.size(), &options));
        if (!object)
        {
            // The embedded object itself is bad: no other attempt can do better.
            return std::unexpected(std::format("couldn't open the BPF object: {}", errnoText(errno)));
        }

        bpf_program* program = nullptr;
        bpf_object__for_each_program(program, object.get())
        {
            bpf_program__set_autoload(program, udpProgramWanted(bpf_program__section_name(program), attempt));
        }

        if (const int error = bpf_object__load(object.get()); error != 0)
        {
            lastError = UdpLibbpfLog::withLog(std::format("{}: loading failed: {}", label, errnoText(-error)), capture.text());
            spdlog::debug("UDP byte counter: {}", lastError);
            continue;
        }

        auto state = std::make_unique<State>();
        bool attached = true;
        // Every return program before any entry program: an entry hook alone (udpv6_sendmsg's marks its thread)
        // could leave state that no return clears, for a call that began before the return hook was there.
        for (const bool returns : {true, false})
        {
            bpf_object__for_each_program(program, object.get())
            {
                if (!attached || !bpf_program__autoload(program) || isReturnProgram(bpf_program__section_name(program)) != returns)
                {
                    continue;
                }
                // Owned at once, so a failed allocation below still detaches it.
                LinkPtr link(bpf_program__attach(program));
                if (!link)
                {
                    const int attachError = errno;
                    lastError = UdpLibbpfLog::withLog(
                        std::format("{}: attaching {} failed: {}", label, bpf_program__section_name(program), errnoText(attachError)),
                        capture.text());
                    spdlog::debug("UDP byte counter: {}", lastError);
                    attached = false;
                    break;
                }
                state->links.push_back(std::move(link));
            }
        }
        if (!attached)
        {
            state->links.clear(); // detach what did attach before the object closes
            continue;
        }

        const std::string mapName(MAP_NAME);
        const bpf_map* map = bpf_object__find_map_by_name(object.get(), mapName.c_str());
        if (map == nullptr)
        {
            return std::unexpected(std::format("the BPF object has no {} map", MAP_NAME));
        }
        // A descriptor of its own, so closing the object leaves the totals map, and only it, open.
        state->map = Posix::FdGuard(::fcntl(bpf_map__fd(map), F_DUPFD_CLOEXEC, 0));
        if (state->map.get() < 0)
        {
            return std::unexpected(std::format("couldn't keep the {} map: {}", MAP_NAME, errnoText(errno)));
        }
        state->attempt = attempt;
        object.reset(); // the programs stay attached through their links
        spdlog::info("UDP byte counter attached with {}", label);
        return UdpByteCounter(std::move(state));
    }
    return std::unexpected(lastError);
}

std::expected<std::vector<UdpByteCounts>, std::string> UdpByteCounter::read() const
{
    std::vector<UdpByteCounts> counts;
    if (!m_State || m_State->map.get() < 0)
    {
        return std::unexpected("the UDP byte counter isn't loaded");
    }
    // A key that aged out of the LRU map between two calls makes the next call start over from the first, so
    // the walk is bounded (at most the map's size in processes, twice that in steps) and skips a tgid it already
    // has.
    std::unordered_set<std::uint32_t> seen;
    std::uint32_t key = 0;
    std::uint32_t next = 0;
    const void* previous = nullptr; // nullptr asks for the first key
    for (std::size_t step = 0; step < 2 * MAP_ENTRIES && counts.size() < MAP_ENTRIES; ++step)
    {
        if (const int error = bpf_map_get_next_key(m_State->map.get(), previous, &next); error != 0)
        {
            if (error == -ENOENT)
            {
                return counts; // the last key
            }
            // Before about 6.5, with kernel.unprivileged_bpf_disabled set, a helper that dropped root may not read
            // its own map (maintainer, 2026-11-07: report it; TCP still counts).
            return std::unexpected(std::format("the kernel refused reading the UDP byte counter: {}", errnoText(-error)));
        }
        MapValue value;
        // An entry can age out of the LRU map between the two calls; skip it.
        if (seen.insert(next).second && bpf_map_lookup_elem(m_State->map.get(), &next, &value) == 0)
        {
            counts.push_back({.tgid = next, .txBytes = value.tx, .rxBytes = value.rx});
        }
        key = next;
        previous = &key;
    }
    return counts;
}

UdpAttachAttempt UdpByteCounter::attempt() const noexcept
{
    return m_State ? m_State->attempt : UdpAttachAttempt{};
}

int UdpByteCounter::mapFd() const noexcept
{
    return m_State ? m_State->map.get() : -1;
}

} // namespace Platform
