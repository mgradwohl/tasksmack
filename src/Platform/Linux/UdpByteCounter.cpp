#include "UdpByteCounter.h"

#include "UdpBytesBpfObject.h" // generated: bpf/udp_bytes.bpf.c, compiled and embedded (cmake/BpfObject.cmake)

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
#include <sys/utsname.h>

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

/// Whether this kernel's udp_recvmsg takes (sk, msg, len, flags, addr_len), as since 5.19, rather than the older
/// six arguments with noblock before flags. Only the kprobe mode needs it; fexit counts the arguments itself.
[[nodiscard]] bool recvmsgHasFiveArguments() noexcept
{
    utsname name{};
    if (::uname(&name) != 0)
    {
        return true;
    }
    // The release string's leading "M.m"; anything else counts as a current kernel.
    const std::span<const char> release(name.release);
    const char* const begin = release.data();
    const char* const end = std::to_address(std::ranges::find(release, '\0')); // the terminator, or the array's end
    unsigned major = 0;
    unsigned minor = 0;
    const auto [afterMajor, majorError] = std::from_chars(begin, end, major);
    if (majorError != std::errc{} || afterMajor == end || *afterMajor != '.' ||
        std::from_chars(afterMajor + 1, end, minor).ec != std::errc{}) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    {
        return true;
    }
    return major > 5 || (major == 5 && minor >= 19);
}

/// Sets the program's RECV_FLAGS_ARG, its only read-only global, before the object loads.
[[nodiscard]] bool setRecvFlagsArg(const bpf_object* object, int position) noexcept
{
    const bpf_map* map = nullptr;
    bpf_object__for_each_map(map, object)
    {
        const std::string_view name(bpf_map__name(map));
        if (!bpf_map__is_internal(map) || !name.ends_with(".rodata"))
        {
            continue;
        }
        std::size_t size = 0;
        void* data = bpf_map__initial_value(map, &size);
        if (data == nullptr || size != sizeof(int))
        {
            return false;
        }
        std::memcpy(data, &position, sizeof(int));
        return true;
    }
    return false;
}

/// The most of libbpf's warnings one attempt keeps, and the most of their end a failure's reason quotes.
constexpr std::size_t CAPTURE_BYTES = 16384;
constexpr std::size_t QUOTED_BYTES = 800;

/// libbpf's warnings during the current attempt on this thread (the verifier's log among them), or null.
thread_local std::string* t_Capture = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): load()'s own

/// Points t_Capture at a buffer for one attempt's warnings, and back at null when it ends.
class CaptureScope
{
  public:
    explicit CaptureScope(std::string& buffer) noexcept
    {
        buffer.clear();
        t_Capture = &buffer;
    }
    ~CaptureScope()
    {
        t_Capture = nullptr;
    }
    CaptureScope(const CaptureScope&) = delete;
    CaptureScope& operator=(const CaptureScope&) = delete;
    CaptureScope(CaptureScope&&) = delete;
    CaptureScope& operator=(CaptureScope&&) = delete;
};

/// @p reason, with the end of what libbpf said in @p captured (the verifier's last lines say why it refused).
[[nodiscard]] std::string withLibbpfLog(std::string reason, std::string_view captured)
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

// libbpf's own messages go to the debug log rather than stderr: a failed fexit attempt is expected on older
// kernels, and the kretprobe fallback then loads. During an attempt its warnings are also kept, so a failure can
// say what the verifier refused.
[[gnu::format(printf, 2, 0)]] int libbpfPrint(libbpf_print_level level, const char* format, va_list args)
{
    if (level == LIBBPF_DEBUG)
    {
        return 0;
    }
    std::array<char, 4096> line{};
    const int length = std::vsnprintf(line.data(), line.size(), format, args); // NOLINT(cert-err33-c): the length is checked
    if (length > 0)
    {
        std::string_view text(line.data(), std::min<std::size_t>(static_cast<std::size_t>(length), line.size() - 1));
        while (text.ends_with('\n'))
        {
            text.remove_suffix(1);
        }
        // NOLINTNEXTLINE(bugprone-exception-escape): spdlog catches its own formatting and sink errors
        spdlog::debug("libbpf: {}", text);
        if (t_Capture != nullptr && t_Capture->size() + text.size() + 1 <= CAPTURE_BYTES)
        {
            try
            {
                t_Capture->append(text).push_back('\n');
            }
            catch (...) // NOLINT(bugprone-empty-catch): the log is a courtesy; an allocation failure only loses it
            {}
        }
    }
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

} // namespace

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
    return function.starts_with("udp_");
}

// Links are destroyed before the object closes: they're declared after it.
struct UdpByteCounter::State
{
    ObjectPtr object;
    std::vector<LinkPtr> links;
    int mapFd = -1;
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
    std::string captured;
    for (const UdpAttachAttempt& attempt : attempts)
    {
        const char* modeName = attempt.mode == UdpAttachMode::Fexit ? "fexit" : "kretprobe";
        const std::string label = std::format("{}{}", modeName, attempt.ipv6 ? "" : " (IPv4 only)");

        bpf_object_open_opts options{};
        options.sz = sizeof(options);
        const std::string objectName(OBJECT_NAME);
        options.object_name = objectName.c_str();
        const CaptureScope capture(captured); // this attempt's libbpf warnings, for its failure's reason
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
        if (!setRecvFlagsArg(object.get(), recvmsgHasFiveArguments() ? 3 : 4))
        {
            return std::unexpected("the BPF object has no RECV_FLAGS_ARG to set");
        }

        if (const int error = bpf_object__load(object.get()); error != 0)
        {
            lastError = withLibbpfLog(std::format("{}: loading failed: {}", label, errnoText(-error)), captured);
            spdlog::debug("UDP byte counter: {}", lastError);
            continue;
        }

        auto state = std::make_unique<State>();
        bool attached = true;
        bpf_object__for_each_program(program, object.get())
        {
            if (!bpf_program__autoload(program))
            {
                continue;
            }
            // Owned at once, so a failed allocation below still detaches it.
            LinkPtr link(bpf_program__attach(program));
            if (!link)
            {
                const int attachError = errno;
                lastError = withLibbpfLog(
                    std::format("{}: attaching {} failed: {}", label, bpf_program__section_name(program), errnoText(attachError)),
                    captured);
                spdlog::debug("UDP byte counter: {}", lastError);
                attached = false;
                break;
            }
            state->links.push_back(std::move(link));
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
        state->mapFd = bpf_map__fd(map);
        state->attempt = attempt;
        state->object = std::move(object);
        spdlog::info("UDP byte counter attached with {}", label);
        return UdpByteCounter(std::move(state));
    }
    return std::unexpected(lastError);
}

std::vector<UdpByteCounts> UdpByteCounter::read() const
{
    std::vector<UdpByteCounts> counts;
    if (!m_State || m_State->mapFd < 0)
    {
        return counts;
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
        if (bpf_map_get_next_key(m_State->mapFd, previous, &next) != 0)
        {
            break; // the last key
        }
        MapValue value;
        // An entry can age out of the LRU map between the two calls; skip it.
        if (seen.insert(next).second && bpf_map_lookup_elem(m_State->mapFd, &next, &value) == 0)
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
    return m_State ? m_State->mapFd : -1;
}

} // namespace Platform
