#include "UdpByteCounter.h"

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
#include <utility>
#include <vector>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

namespace Platform
{

namespace
{

constexpr std::string_view OBJECT_NAME = "udp_bytes";
constexpr std::string_view MAP_NAME = "udp_bytes_by_tgid";
/// The map's max_entries in bpf/udp_bytes.bpf.c, and Broker::MAX_UDP_BYTE_PROCESSES.
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

// libbpf's own messages go to the debug log rather than stderr: a failed fexit attempt is expected on older
// kernels, and the kretprobe fallback then loads.
[[gnu::format(printf, 2, 0)]] int libbpfPrint(libbpf_print_level level, const char* format, va_list args)
{
    if (level == LIBBPF_DEBUG)
    {
        return 0;
    }
    std::array<char, 512> line{};
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
    const std::string_view prefix = attempt.mode == UdpAttachMode::Fexit ? "fexit/" : "kretprobe/";
    if (!section.starts_with(prefix))
    {
        return false;
    }
    std::string_view function = section;
    function.remove_prefix(prefix.size()); // starts_with above: no throwing substr in a noexcept function
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
    libbpf_set_print(libbpfPrint);

    std::string lastError = "no attach mode was tried";
    for (const UdpAttachAttempt& attempt : ATTEMPTS)
    {
        const char* modeName = attempt.mode == UdpAttachMode::Fexit ? "fexit" : "kretprobe";
        const std::string label = std::format("{}{}", modeName, attempt.ipv6 ? "" : " (IPv4 only)");

        bpf_object_open_opts options{};
        options.sz = sizeof(options);
        const std::string objectName(OBJECT_NAME);
        options.object_name = objectName.c_str();
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
            lastError = std::format("{}: loading failed: {}", label, errnoText(-error));
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
            bpf_link* link = bpf_program__attach(program);
            if (link == nullptr)
            {
                lastError = std::format("{}: attaching {} failed: {}", label, bpf_program__section_name(program), errnoText(errno));
                spdlog::debug("UDP byte counter: {}", lastError);
                attached = false;
                break;
            }
            state->links.emplace_back(link);
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

std::expected<std::vector<UdpByteCounts>, std::string> UdpByteCounter::read() const
{
    std::vector<UdpByteCounts> counts;
    if (!m_State || m_State->mapFd < 0)
    {
        return std::unexpected("the UDP byte counter isn't loaded");
    }
    std::uint32_t key = 0;
    std::uint32_t next = 0;
    const void* previous = nullptr; // nullptr asks for the first key
    // A key that aged out of the LRU map between two calls makes the next call start over from the first, so
    // the walk is bounded: at most the map's size in entries, and twice that in steps.
    for (std::size_t step = 0; step < 2 * MAP_ENTRIES && counts.size() < MAP_ENTRIES; ++step)
    {
        if (const int error = bpf_map_get_next_key(m_State->mapFd, previous, &next); error != 0)
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
        if (bpf_map_lookup_elem(m_State->mapFd, &next, &value) == 0)
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
