# Modern C++23 in TaskSmack

This page says which C++23 features TaskSmack can use, which it can't, and how the project uses the language.
It was written from the C++23 audit of 2026-11-18 ([#4340](https://github.com/mgradwohl/tasksmack/issues/4340)).
The review checklist ([`.github/skills/code-review/SKILL.md`](https://github.com/mgradwohl/tasksmack/blob/main/.github/skills/code-review/SKILL.md))
applies it to every diff, and [`CONTRIBUTING.md`](https://github.com/mgradwohl/tasksmack/blob/main/CONTRIBUTING.md)
has the build, encoding and locale rules it refers to.

Modernize only where the new form keeps the behavior and removes a bug class, a duplicate, or an unclear owner.
A feature being new is not a reason to use it.

## The toolchain decides what "C++23" means

TaskSmack builds with clang only, as `-std=c++23` with no GNU extensions, against two standard libraries:

| | Linux | Windows |
|---|---|---|
| Compiler | clang 23 | clang 23, target `x86_64-pc-windows-msvc` (and ARM64) |
| Standard library | libc++ 23 | MSVC STL, Build Tools 14.51 or later (what CI's runner images have) |
| Library hardening | `_LIBCPP_HARDENING_MODE_FAST` in the app, tests and fuzzers | `_MSVC_STL_HARDENING=1` in the app, the elevated images and the tests ([#4341](https://github.com/mgradwohl/tasksmack/issues/4341)) |

The two libraries don't implement the same subset of C++23. Code in `Platform/Linux`, and all shared code
(`Domain`, `Core`, `UI`, `App`, and `Platform`'s top level), has to compile on both, so libc++ is the limit.

### Usable everywhere

Each of these has its feature-test macro defined on both libraries, and a snippet compiled on each.

| Feature | Use it for |
|---|---|
| `std::expected<T, E>` with `and_then`, `transform`, `or_else` | a result that can fail with a reason |
| `std::optional` with `transform`, `and_then`, `value_or` | a value that may be absent |
| `std::span`, `std::string_view`, `std::mdspan` | borrowed views of contiguous data |
| `std::format`, `std::print` (ranges and chrono too) | text that isn't shown to the user as a localized number |
| `std::from_chars`, `std::to_chars` (floating point too) | parsing and writing numbers |
| `std::ranges::to`, `views::zip`, `views::enumerate`, `views::join_with`, `views::chunk_by`, `views::stride`, `views::repeat` | pipelines with no early exit |
| `ranges::fold_left`, `ranges::contains`, `ranges::find_last`, `ranges::starts_with` | algorithms |
| `std::flat_map`, `std::flat_set` | small sorted lookups read far more than written (needs MSVC Build Tools 14.51) |
| `std::out_ptr`, `std::inout_ptr` | filling an owner from a C API's out-parameter |
| `std::to_underlying`, `std::byteswap`, `std::bit_cast`, `std::in_range`, `std::cmp_less` | enums, byte order, type punning and mixed-sign comparisons |
| `std::jthread`, `std::stop_token`, `condition_variable_any::wait` with a stop token | inside `Domain::Worker` and its relatives (see Concurrency) |
| `string::contains`, `string::resize_and_overwrite` | text |
| deducing `this`, `if consteval`, `static operator()`, `[[assume]]`, the `uz` suffix | language; use where it removes real duplication |

### Not usable in shared or Linux code

libc++ 23 doesn't have these. They compile on Windows, so a Windows-only build won't warn you.

| Feature | Use instead |
|---|---|
| `std::atomic<std::shared_ptr<T>>` | `Domain::PublicationSlot` (a short mutex and an atomic version) |
| `std::move_only_function` | `std::function` over a `std::shared_ptr`, or a template parameter |
| `std::generator` | a callback, or a function that returns a container |
| `std::stacktrace` | nothing: log what failed and where |
| `views::chunk`, `views::slide`, `views::adjacent`, `views::cartesian_product`, `views::as_const` | a loop |
| `<spanstream>`, the chrono time-zone database | `std::from_chars`; the OS's own time-zone calls |
| `constexpr` `<cmath>` | a hand-written `constexpr` helper |
| `std::start_lifetime_as` (in neither library) | `std::memcpy` or `std::bit_cast` |

C++26 features that clang accepts early (`= delete("reason")`, pack indexing, placeholder `_` variables, structured
bindings in a condition) warn under `-Wc++26-extensions`, which `-Werror` makes an error. The build enforces C++23.

### Re-checking after a toolchain bump

When LLVM or the MSVC toolset moves, the tables above can go stale in either direction. List what each library
defines, and compile a three-line use of anything you're about to rely on:

```bash
# Linux
printf '#include <version>\n' | clang++-23 -stdlib=libc++ -std=c++23 -x c++ -E -dM - | grep -E '__cpp_lib_|_LIBCPP_VERSION'
```

```powershell
# Windows (PowerShell 7)
'#include <version>' | clang++ -std=c++23 -x c++ -E -dM - | Select-String '__cpp_lib_|_MSVC_STL_UPDATE'
```

Update this page in the same PR as the bump.

## Errors and absent values

- **Return `std::expected<T, E>` when a call can fail with a reason, and `std::optional<T>` when a value may be
  absent.** Don't return `bool` with out-parameters, a string in which empty means success, or an optional whose
  value means failure: each lets a caller read the wrong half.
- **No sentinels in new types.** `-1`, `0` or an empty string meaning "unknown" has to be known by every reader; an
  optional can't be misread as a number. A status with three or more states is an `enum class`, not two bools.
- **An out-parameter is right for a buffer reused across samples** (a vector the sampler refills each pass). Say so
  at the declaration.
- **A read that stops early says so.** A timeout, an error or a cap is not "the end of the list": return the error,
  or mark the result truncated or incomplete.
- **Exceptions stay inside the thread that threw them.** `App::BackgroundTask` turns a throwing body into its Failed
  result; a sampler pass logs and carries on. `catch (...)` belongs only at a thread or C-callback boundary. Don't
  use an exception for a parse failure you expect.
- **`noexcept` on destructors, moves and swap.** spdlog inside a `noexcept` function takes
  `NOLINTNEXTLINE(bugprone-exception-escape)` with the reason the checklist gives.

## Ownership and lifetimes

- **Rule of zero.** A class that needs a destructor is an owner: write (or delete) all five special members.
- **Wrap every OS resource where it is acquired:** `Windows::UniqueHandle` and its relatives in `WindowsHandles.h`,
  `Posix::FdGuard` and `DirGuard`, `ComPtr`, a `std::unique_ptr` with a deleter for a C allocation. No allocation or
  other call that can throw may sit between an acquire and a hand-written release. `std::out_ptr` fills an owner
  straight from an out-parameter.
- **`std::string_view` and `std::span` are borrowed.** Don't store one that can outlive what it refers to, and don't
  return one into a local or a temporary. `string_view::data()` isn't NUL-terminated: a function that calls a C API
  takes `const std::string&`, or builds a string.
- **A `std::views` pipeline is lazy and borrows too.** Materialize it with `std::ranges::to` before its source goes.
- **A type whose stored callbacks capture `this` deletes its copy and move.**
- **Interfaces** have a virtual destructor and deleted (or protected) copy and move, so nothing can slice.
- **An identity carries its proof.** A process is a `ProcessTarget` (PID and start time), never a bare PID, from the
  click to the platform call. Never open by name a path that another process supplied.

## Numbers, text and bytes

- **Parse with `std::from_chars`.** Check both the error code and that the whole field was consumed. Never
  `std::stoi`/`stoul`/`stod`, `strtol`, `atoi` or `sscanf`: they depend on the locale, accept a sign and wrap it,
  ignore trailing junk, or throw for control flow.
- **Format with `std::format`.** A number the user sees goes through `UI::Format`, which applies the user's
  punctuation. Choose a unit from the rounded value, not the raw one.
- **`<cctype>` is locale-dependent, and undefined for a negative `char`.** For identifiers, keys and protocol text use
  `Platform/AsciiCase.h` and `Platform/AsciiTrim.h`. For text the user types or reads, follow the UTF-8 rules
  ([#4299](https://github.com/mgradwohl/tasksmack/issues/4299) and CONTRIBUTING.md's "Text encoding and locale").
- **NaN isn't ordered.** Handle it before `std::clamp`, `std::min`, `std::max` or a comparator, and never convert a
  NaN or an infinity to an integer. In history, NaN means "no reading" and draws as a gap.
- **Counters:** use `Domain/Numeric.h` (`counterDelta`, `wrappingCounterDelta`, `toDouble`). Compare mixed
  signedness with `std::cmp_less` or `std::in_range`. A counter that went backwards is a gap, not a zero.
- **Bytes from outside the process:** bound every offset and count before using it, with the remainder kept in a type
  that can't wrap. Read unaligned data with `std::memcpy` or `std::bit_cast`, and swap with `std::byteswap`. Cap
  every read whose size another process chooses. `reinterpret_cast` is for OS API boundaries, with a comment.
- **Enums** are `enum class` with an explicit underlying type. Switch over one without a `default`, so `-Wswitch`
  finds the switch a new enumerator needs. A state compared as a string literal in more than one place is an enum.

## Concurrency

- **A new background thread is a `Domain::Worker`.** A one-shot call from a view is an `App::BackgroundTask`, and
  periodic sampling is a `BackgroundSampler`. A raw `std::jthread` needs its reason stated at the thread. Never
  detach.
- **Publish immutable snapshots through `Domain::PublicationSlot`.** The UI thread reads; it never calls a probe,
  blocks, or does file I/O.
- **Every condition-variable wait has a predicate**, and a stop token where the thread has one.
- **Say which lock guards a member, and the order locks are taken in**, at the declaration.
- **A `NOLINT(concurrency-mt-unsafe)` gives the true reason** the call is safe ("glibc keeps `dlerror`'s state per
  thread"), not the thread it happens to run on today.

## Interfaces and templates

- Constrain templates with concepts or `requires`, not SFINAE.
- Make pure helpers `constexpr`, so a test can `static_assert` them.
- Mark results and RAII guards `[[nodiscard]]`.
- Replace a run of same-typed parameters with an options struct and designated initializers.
- Fields that only make sense together live in one struct, inside one optional.

## Tests

- **A fake or mock used by one test file goes in that file's anonymous namespace.** It then has internal linkage, so
  two test files can't define the same name differently in one binary. One that several files share goes in
  `tests/Mocks/`. `std::make_unique` and `std::make_shared` work with either. The exception is a test accessor that
  a production class befriends (`friend struct FooTestAccessor;`): it is defined in the namespace that declaration
  names, or the friendship doesn't apply.
- Wait on the state you observe, never on a sleep; give time-dependent code a clock parameter.
- Restore what you change (environment variables, the config directory) with a scope guard, not at the end of the body.
- A skip that hides a missing prerequisite needs a `TASKSMACK_REQUIRE_*` switch that CI sets.
- A parser of bytes or text from outside the process gets a fuzz harness in `tests/fuzz`.

## What the build already enforces

Reviews needn't repeat these:

- **Warnings as errors:** `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2
  -Wimplicit-fallthrough -Wnull-dereference -Wdouble-promotion -Wcast-align -Wundef` and a few more.
- **clang-tidy on `src/`**, findings as errors: `bugprone`, `clang-analyzer`, `concurrency-mt-unsafe`,
  `cppcoreguidelines`, `misc`, `modernize`, `performance`, `portability`, `readability`.
- **Sanitizers:** ASan and UBSan on every PR and TSan on `main`, on Linux, and ASan+UBSan nightly on Windows x64 (`win-asan-ubsan`); libFuzzer harnesses.

To suppress a finding, name the check and give the reason: `NOLINT(check-name) - why`. Don't name a check that
`.clang-tidy` already disables.

## Don't introduce

None of these is in the tree today (searched 2026-11-18). Each is deprecated in C++20 or C++23, or removed in C++26:
`std::atomic_load`/`atomic_store` on a `shared_ptr`, `<codecvt>` and `std::wstring_convert`,
`std::filesystem::u8path`, `std::aligned_storage`, `std::result_of`, `std::is_pod`, `std::iterator`, `std::not1`,
`<strstream>`, compound assignment on a `volatile`, and `[=]` capturing `this` implicitly.

In third-party and OS APIs: call Dear ImGui's current names (the ones still valid with
`IMGUI_DISABLE_OBSOLETE_FUNCTIONS`), `std::generic_category().message(e)` in place of `strerror`, and the versioned
exports of vendor libraries (`nvmlInit_v2`).
