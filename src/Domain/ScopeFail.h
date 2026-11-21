#pragma once

// Domain::ScopeFail: runs a cleanup when its scope is left by an exception, and not on a normal exit (#4518).
//
// It replaces `try { ... } catch (...) { cleanup(); throw; }`. Throwing while a catch handler runs (a bare `throw;`
// or a new exception) crashes clang 23's Windows AddressSanitizer runtime (llvm-project#34713), and the guard
// never throws inside a handler: the exception just unwinds past it. The cleanup runs in the destructor, during the
// unwinding, and sees the same state a catch handler would.
//
// The guard compares std::uncaught_exceptions() with its value at construction, so it works when nested
// (inside the destructor of an object being unwound, inside another guard) and fires only for an exception that
// began after it was made. The cleanup must not throw: anything it throws is swallowed, as a destructor must.
//
// Not copyable or movable: declare it in the scope it guards, before the code that may throw. Tested in
// tests/Domain/test_ScopeFail.cpp.

#include <exception>
#include <type_traits>
#include <utility>

namespace Domain
{

template<typename Fn> class ScopeFail
{
  public:
    static_assert(std::is_invocable_v<Fn&>, "ScopeFail needs a callable taking no arguments");

    explicit ScopeFail(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : m_Fn(std::move(fn)), m_Uncaught(std::uncaught_exceptions())
    {}

    ~ScopeFail() noexcept
    {
        if (std::uncaught_exceptions() > m_Uncaught)
        {
            try
            {
                m_Fn();
            }
            catch (...) // NOLINT(bugprone-empty-catch) - a destructor must not throw; the cleanup should not either
            {}
        }
    }

    ScopeFail(const ScopeFail&) = delete;
    ScopeFail& operator=(const ScopeFail&) = delete;
    ScopeFail(ScopeFail&&) = delete;
    ScopeFail& operator=(ScopeFail&&) = delete;

  private:
    Fn m_Fn;
    int m_Uncaught;
};

} // namespace Domain
