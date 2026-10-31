/// @file FakeIfeoRegistry.h
/// @brief taskmgr.exe's IFEO key in memory (#3072), for Replace Task Manager's logic without the registry:
/// no test writes the real key, HKLM's or HKCU's.

#pragma once

#include "Platform/TaskManagerReplacement.h"

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Mocks
{

class FakeIfeoRegistry final : public Platform::TaskManagerReplacement::IIfeoRegistry
{
  public:
    /// Whether the key is there, and its values by name (the Debugger, the mark, another tool's).
    bool keyExists = false;
    std::map<std::string, std::string, std::less<>> values;
    /// A call by this name ("readDebugger", "createKey", ...) fails with this reason.
    std::map<std::string, std::string, std::less<>> failures;
    /// Every call made, in order.
    std::vector<std::string> calls;

    /// A key holding @p debugger, as a tool other than TaskSmack would leave it: no mark.
    static void seed(FakeIfeoRegistry& registry, std::string debugger)
    {
        registry.keyExists = true;
        registry.values[std::string(Platform::TaskManagerReplacement::DEBUGGER_VALUE)] = std::move(debugger);
    }

    [[nodiscard]] std::optional<std::string> debugger() const
    {
        const auto it = values.find(Platform::TaskManagerReplacement::DEBUGGER_VALUE);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    [[nodiscard]] bool marked() const
    {
        return values.contains(Platform::TaskManagerReplacement::CREATED_MARKER_VALUE);
    }

    [[nodiscard]] std::expected<std::optional<std::string>, std::string> readDebugger() override
    {
        if (const auto failure = failed("readDebugger"))
        {
            return std::unexpected(*failure);
        }
        return keyExists ? debugger() : std::nullopt;
    }
    [[nodiscard]] std::expected<bool, std::string> createKey() override
    {
        if (const auto failure = failed("createKey"))
        {
            return std::unexpected(*failure);
        }
        const bool created = !keyExists;
        keyExists = true;
        return created;
    }
    [[nodiscard]] std::expected<void, std::string> writeDebugger(std::string_view value) override
    {
        return write("writeDebugger", Platform::TaskManagerReplacement::DEBUGGER_VALUE, std::string(value));
    }
    [[nodiscard]] std::expected<void, std::string> deleteDebugger() override
    {
        return erase("deleteDebugger", Platform::TaskManagerReplacement::DEBUGGER_VALUE);
    }
    [[nodiscard]] std::expected<bool, std::string> readCreatedMarker() override
    {
        if (const auto failure = failed("readCreatedMarker"))
        {
            return std::unexpected(*failure);
        }
        return keyExists && marked();
    }
    [[nodiscard]] std::expected<void, std::string> writeCreatedMarker() override
    {
        return write("writeCreatedMarker", Platform::TaskManagerReplacement::CREATED_MARKER_VALUE, "1");
    }
    [[nodiscard]] std::expected<void, std::string> deleteCreatedMarker() override
    {
        return erase("deleteCreatedMarker", Platform::TaskManagerReplacement::CREATED_MARKER_VALUE);
    }
    [[nodiscard]] std::expected<bool, std::string> deleteKeyIfEmpty() override
    {
        if (const auto failure = failed("deleteKeyIfEmpty"))
        {
            return std::unexpected(*failure);
        }
        if (!keyExists || !values.empty())
        {
            return false;
        }
        keyExists = false;
        return true;
    }

  private:
    [[nodiscard]] std::optional<std::string> failed(std::string_view call)
    {
        calls.emplace_back(call);
        const auto it = failures.find(call);
        return it == failures.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    [[nodiscard]] std::expected<void, std::string> write(std::string_view call, std::string_view name, std::string value)
    {
        if (const auto failure = failed(call))
        {
            return std::unexpected(*failure);
        }
        if (!keyExists)
        {
            return std::unexpected(std::string("the key isn't there"));
        }
        values[std::string(name)] = std::move(value);
        return {};
    }
    [[nodiscard]] std::expected<void, std::string> erase(std::string_view call, std::string_view name)
    {
        if (const auto failure = failed(call))
        {
            return std::unexpected(*failure);
        }
        if (const auto it = values.find(name); it != values.end())
        {
            values.erase(it);
        }
        return {};
    }
};

} // namespace Mocks
