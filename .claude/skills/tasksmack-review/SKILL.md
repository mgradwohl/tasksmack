---
name: tasksmack-review
description: TaskSmack-specific review checklist for C++23 compliance, memory/exception safety, thread safety, layered architecture boundaries, naming/include conventions, and clang-tidy pitfalls. Use alongside /code-review when reviewing any diff or pull request in this repository. Ported from .github/skills/code-review/SKILL.md (written for GitHub Copilot) so the same checks apply here.
---

# TaskSmack Code Review

Apply these checks when reviewing pull requests or diffs in this repository. Report only genuine issues; skip style nits already enforced by clang-format.

## 1. Architecture boundaries (highest priority)

TaskSmack has a strict layered architecture: App → UI → Core → Domain → Platform.

- Platform probes (`src/Platform/`) must return **raw counters** only (CPU ticks, bytes). Flag any computation of rates, deltas, or percentages in a probe.
- Domain (`src/Domain/`) must not depend on UI, Core, ImGui/ImPlot, or graphics libraries.
- UI code must never call Platform directly, include `Platform/Factory.h`, or call `Platform::make*()`. Path access goes through `Core::Application::get().paths()`.
- App panels may call `Platform::make*Probe()` / `Platform::makeProcessActions()` only at construction/`onAttach` time — never in render loops.
- OpenGL calls belong in `Core/` and `UI/` only.

## 2. C++23 compliance

- Suggest modern replacements for legacy patterns **only when the replacement preserves behavior and improves clarity**: `find()` used purely as a containment/prefix check → `contains()`/`starts_with()` (not when the position is needed, e.g., for `substr` parsing); raw loops → `std::ranges`/`std::views` when the loop has no early exit or iterator dependence (not when iterators are needed, e.g., map erase patterns); `fmt::format` → `std::format`; raw threads → `std::jthread`; C-style arrays → `std::array`/`std::span`.
- Flag C-style casts; require the narrowest named cast with a comment explaining why it is safe.
- Flag `using namespace` in headers.
- Prefer `enum class` over plain `enum`, with an explicit underlying type (e.g., `: std::uint8_t`) when size matters. Do not flag C-style enums that intentionally mirror an external ABI (e.g., `src/Platform/NVMLTypes.h`), especially when marked with NOLINT and a rationale.

## 3. Memory and exception safety

- Flag raw `new`/`delete`; require smart pointers and RAII.
- Rule of 5: if any of destructor/copy/move ops is user-defined or deleted, all five must be handled.
- Destructors, move constructors, and swap should be `noexcept`.
- Check `std::optional` access is guarded (`has_value()`).

## 4. Thread safety

- Verify mutex usage and `std::atomic` correctness in `BackgroundSampler` and snapshot publication paths.
- Snapshots published to UI must be immutable and versioned; flag shared mutable state crossed between sampler threads and render code.

## 5. Conventions

- Naming: `PascalCase` classes, `camelCase` functions, `m_camelCase` members, `UPPER_SNAKE_CASE` constants.
- Include order: matching header → project headers → third-party → stdlib, blank line between groups; `#pragma once` in headers.
- GLAD before SDL: `#include <glad/gl.h>` then `#include <SDL3/SDL.h>`.
- Reuse constants from `src/Domain/SamplingConfig.h`; flag re-declared sampling literals.
- Windows code must call wide-character (W) APIs and convert to UTF-8 at the boundary.

## 6. Testing

- New Domain logic should have Google Test coverage in `tests/` (mirroring `src/`) using mocks from `tests/Mocks/MockProbes.h`.
- Flag `EXPECT_EQ` on floating-point values (use `EXPECT_DOUBLE_EQ`).
- Flag mocks defined inside anonymous namespaces when used with `std::make_unique`.

## 7. clang-tidy pitfalls

Flag: uninitialized members, missing `override`, `NULL`/`0` for pointers, `const` return by value, unnecessary copies (use `const&`), missing parentheses in math expressions, and exceptions escaping `noexcept` functions.
