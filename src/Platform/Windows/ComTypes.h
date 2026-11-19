#pragma once

// RAII owners for the COM automation values the Windows readers pass and get back (#3865): a BSTR, a VARIANT and
// a PROPVARIANT, each freed exactly once, on destruction or when a call writes it again through put(), however the
// scope that holds it ends (an exception included). ComPtr.h owns the interface pointers; these own the values.

#include "WinString.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <oleauto.h> // NOLINT(misc-include-cleaner) - SysAllocStringLen, SysFreeString, SysStringLen, VariantInit, VariantClear
#include <propidl.h> // NOLINT(misc-include-cleaner) - PROPVARIANT, PropVariantInit, PropVariantClear
// clang-format on

#include <limits>
#include <new>
#include <string>
#include <string_view>

#pragma comment(lib, "ole32.lib")    // PropVariantClear
#pragma comment(lib, "oleaut32.lib") // the BSTR and VARIANT calls

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 and COM types come from the sub-headers above

/// Owns one BSTR: freed exactly once, on destruction, on reset(), or when put() hands it to a call to write again.
class Bstr
{
  public:
    Bstr() noexcept = default;
    /// A copy of @p text (embedded nulls kept); throws std::bad_alloc when it can't be allocated.
    explicit Bstr(std::wstring_view text) : m_Value(allocate(text))
    {}
    /// A copy of UTF-8 @p utf8, as UTF-16; throws std::bad_alloc when it can't be allocated.
    explicit Bstr(std::string_view utf8) : Bstr(std::wstring_view(WinString::utf8ToWide(utf8)))
    {}
    ~Bstr()
    {
        reset();
    }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    Bstr(Bstr&& other) noexcept : m_Value(other.release())
    {}
    Bstr& operator=(Bstr&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Value = other.release();
        }
        return *this;
    }

    [[nodiscard]] BSTR get() const noexcept
    {
        return m_Value;
    }
    /// Frees the current string and returns where a call writes a new one (an out BSTR*).
    [[nodiscard]] BSTR* put() noexcept
    {
        reset();
        return &m_Value;
    }
    /// Hands the string to the caller, who frees it (or to a VARIANT that does), and leaves this empty.
    [[nodiscard]] BSTR release() noexcept
    {
        BSTR value = m_Value;
        m_Value = nullptr;
        return value;
    }
    /// Frees the string, if any.
    void reset() noexcept
    {
        SysFreeString(m_Value); // null is allowed
        m_Value = nullptr;
    }
    /// The string as UTF-8, all SysStringLen() characters of it; empty when there is none.
    [[nodiscard]] std::string utf8() const
    {
        return m_Value == nullptr ? std::string() : WinString::wideToUtf8(std::wstring_view(m_Value, SysStringLen(m_Value)));
    }
    /// The string as it is, all SysStringLen() characters of it, for a native identity (#4304); empty when there is none.
    [[nodiscard]] std::wstring wide() const
    {
        return m_Value == nullptr ? std::wstring() : std::wstring(m_Value, SysStringLen(m_Value));
    }

  private:
    [[nodiscard]] static BSTR allocate(std::wstring_view text)
    {
        if (text.size() > std::numeric_limits<UINT>::max())
        {
            throw std::bad_alloc();
        }
        BSTR value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        if (value == nullptr)
        {
            throw std::bad_alloc();
        }
        return value;
    }

    BSTR m_Value = nullptr;
};

/// Owns one VARIANT: VariantClear'd exactly once, on destruction, on reset(), or when put() hands it to a call to
/// write again, whatever it holds (a BSTR it owns is freed, an interface released).
class Variant
{
  public:
    /// VT_EMPTY: an automation call's optional argument left out.
    Variant() noexcept
    {
        VariantInit(&m_Value);
    }
    /// A VT_I4, as the collections' get_Item and ShellExecute's show argument take.
    explicit Variant(LONG value) noexcept : Variant()
    {
        // VARIANT is the OS's tagged union: vt says which member holds the value.
        m_Value.vt = VT_I4;   // NOLINT(cppcoreguidelines-pro-type-union-access) - VARIANT's own tag
        m_Value.lVal = value; // NOLINT(cppcoreguidelines-pro-type-union-access) - the member VT_I4 names
    }
    /// A VT_BSTR that takes @p text over: freed with this VARIANT.
    explicit Variant(Bstr text) noexcept : Variant()
    {
        m_Value.vt = VT_BSTR;             // NOLINT(cppcoreguidelines-pro-type-union-access) - VARIANT's own tag
        m_Value.bstrVal = text.release(); // NOLINT(cppcoreguidelines-pro-type-union-access) - the member VT_BSTR names
    }
    ~Variant()
    {
        reset();
    }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    Variant(Variant&& other) noexcept : m_Value(other.m_Value)
    {
        VariantInit(&other.m_Value); // the value is this one's now: the other must not clear it
    }
    Variant& operator=(Variant&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Value = other.m_Value;
            VariantInit(&other.m_Value);
        }
        return *this;
    }

    /// A bitwise copy for a by-value VARIANT parameter; this one keeps ownership (callees don't free their copy).
    [[nodiscard]] VARIANT get() const noexcept
    {
        return m_Value;
    }
    /// The value held, to read.
    [[nodiscard]] const VARIANT& value() const noexcept
    {
        return m_Value;
    }
    /// Clears the current value and returns where a call writes a new one (an out VARIANT*).
    [[nodiscard]] VARIANT* put() noexcept
    {
        reset();
        return &m_Value;
    }
    /// Clears the value, leaving VT_EMPTY.
    void reset() noexcept
    {
        static_cast<void>(VariantClear(&m_Value));
        VariantInit(&m_Value); // VT_EMPTY even when VariantClear refused a type it doesn't know
    }

  private:
    VARIANT m_Value{};
};

/// Owns one PROPVARIANT: PropVariantClear'd exactly once, on destruction, on reset(), or when put() hands it to a
/// call (IPropertyStore::GetValue) to write again, whatever it holds (a string freed, an interface released).
class PropVariant
{
  public:
    /// VT_EMPTY.
    PropVariant() noexcept
    {
        PropVariantInit(&m_Value);
    }
    ~PropVariant()
    {
        reset();
    }
    PropVariant(const PropVariant&) = delete;
    PropVariant& operator=(const PropVariant&) = delete;
    PropVariant(PropVariant&& other) noexcept : m_Value(other.m_Value)
    {
        PropVariantInit(&other.m_Value); // the value is this one's now: the other must not clear it
    }
    PropVariant& operator=(PropVariant&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Value = other.m_Value;
            PropVariantInit(&other.m_Value);
        }
        return *this;
    }

    /// The value held, to read.
    [[nodiscard]] const PROPVARIANT& get() const noexcept
    {
        return m_Value;
    }
    /// Clears the current value and returns where a call writes a new one (an out PROPVARIANT*).
    [[nodiscard]] PROPVARIANT* put() noexcept
    {
        reset();
        return &m_Value;
    }
    /// Clears the value, leaving VT_EMPTY.
    void reset() noexcept
    {
        static_cast<void>(PropVariantClear(&m_Value));
        PropVariantInit(&m_Value); // VT_EMPTY even when PropVariantClear refused a type it doesn't know
    }

  private:
    PROPVARIANT m_Value{};
};

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
