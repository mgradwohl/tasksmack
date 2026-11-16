/// @file test_ComTypes.cpp
/// @brief Platform::Windows::Bstr, Variant and PropVariant (#3865), the owners of the COM automation values: each
/// owns what it holds, hands it on when moved, frees it exactly once when destroyed, and frees the old value
/// before put() hands it to a call to write again. An interface held in a VARIANT or PROPVARIANT counts its
/// Release() calls, so each free is seen.

#include "Platform/Windows/ComTypes.h"

#include <gtest/gtest.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <propidl.h>
#include <unknwn.h>
// clang-format on

#include <string_view>
#include <utility>

namespace Platform::Windows
{
namespace
{

// VARIANT and PROPVARIANT are the OS's tagged unions: these tests read and set their tags and members, and read
// the moved-from owners, whose empty state is what they check.
// NOLINTBEGIN(cppcoreguidelines-pro-type-union-access,bugprone-use-after-move,clang-analyzer-cplusplus.Move)

/// An IUnknown that counts its references, so a test sees when a VARIANT or PROPVARIANT releases it.
class CountedUnknown final : public IUnknown
{
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        if (IsEqualIID(iid, IID_IUnknown) != FALSE)
        {
            *object = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_References;
    }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ++m_Releases;
        return --m_References; // on the test's stack: never deleted
    }

    [[nodiscard]] ULONG references() const noexcept
    {
        return m_References;
    }
    [[nodiscard]] int releases() const noexcept
    {
        return m_Releases;
    }

  private:
    ULONG m_References = 1; // the test's own
    int m_Releases = 0;
};

/// Stores @p unknown, with a reference of its own, as the VT_UNKNOWN @p value.
template<typename Value> void holdUnknown(Value* value, CountedUnknown& unknown)
{
    unknown.AddRef();
    value->vt = VT_UNKNOWN;
    value->punkVal = &unknown;
}

TEST(ComTypesBstrTest, AConstructedStringHoldsItsTextAndLength)
{
    const Bstr empty;
    EXPECT_EQ(empty.get(), nullptr);
    EXPECT_EQ(empty.utf8(), "");

    const Bstr wide(std::wstring_view(L"C:\\Program Files\\T\u00e2sk"));
    ASSERT_NE(wide.get(), nullptr);
    EXPECT_EQ(SysStringLen(wide.get()), 21U);
    EXPECT_EQ(wide.utf8(), "C:\\Program Files\\T\xc3\xa2sk");

    const Bstr utf8(std::string_view("\\Microsoft\\Windows\\\xe6\x9d\xb1\xe4\xba\xac"));
    ASSERT_NE(utf8.get(), nullptr);
    EXPECT_EQ(std::wstring_view(utf8.get(), SysStringLen(utf8.get())), L"\\Microsoft\\Windows\\\u6771\u4eac");
    EXPECT_EQ(utf8.utf8(), "\\Microsoft\\Windows\\\xe6\x9d\xb1\xe4\xba\xac");

    const Bstr blank(std::wstring_view{});
    EXPECT_NE(blank.get(), nullptr) << "an empty text is an empty BSTR, not none";
    EXPECT_EQ(SysStringLen(blank.get()), 0U);

    const Bstr withNull(std::wstring_view(L"a\0b", 3));
    EXPECT_EQ(SysStringLen(withNull.get()), 3U) << "every character is kept, an embedded null too";
}

TEST(ComTypesBstrTest, MovingHandsTheStringOnAndLeavesTheSourceEmpty)
{
    Bstr first(std::wstring_view(L"first"));
    const OLECHAR* const held = first.get();

    Bstr moved(std::move(first));
    EXPECT_EQ(moved.get(), held);
    EXPECT_EQ(first.get(), nullptr);

    Bstr assigned(std::wstring_view(L"second"));
    assigned = std::move(moved);
    EXPECT_EQ(assigned.get(), held);
    EXPECT_EQ(moved.get(), nullptr);
    EXPECT_EQ(assigned.utf8(), "first");
}

TEST(ComTypesBstrTest, PutFreesTheOldStringAndTakesTheOneWritten)
{
    Bstr value(std::wstring_view(L"old"));
    BSTR* out = value.put();
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(*out, nullptr) << "the old string is freed before the call writes";
    *out = SysAllocString(L"written");
    EXPECT_EQ(value.utf8(), "written");
}

TEST(ComTypesBstrTest, ReleaseHandsOwnershipToTheCaller)
{
    Bstr value(std::wstring_view(L"mine"));
    const OLECHAR* const held = value.get();
    BSTR released = value.release();
    EXPECT_EQ(released, held);
    EXPECT_EQ(value.get(), nullptr);
    SysFreeString(released); // the caller's now

    value.reset();
    EXPECT_EQ(value.get(), nullptr) << "reset() of an empty one is allowed";
}

TEST(ComTypesVariantTest, ItStartsEmptyOrHoldsAnIndexOrAString)
{
    const Variant empty;
    EXPECT_EQ(empty.value().vt, VT_EMPTY);

    const Variant index(LONG{7});
    EXPECT_EQ(index.value().vt, VT_I4);
    EXPECT_EQ(index.value().lVal, 7);

    Bstr text(std::wstring_view(L"open"));
    const OLECHAR* const held = text.get();
    const Variant owned(std::move(text));
    EXPECT_EQ(owned.value().vt, VT_BSTR);
    EXPECT_EQ(owned.value().bstrVal, held) << "the VARIANT takes the string over";
    EXPECT_EQ(owned.get().bstrVal, held) << "get() is a copy to pass by value";
}

TEST(ComTypesVariantTest, ItClearsWhatItHoldsWhenDestroyed)
{
    CountedUnknown unknown;
    {
        Variant value;
        holdUnknown(value.put(), unknown);
        EXPECT_EQ(unknown.references(), 2U);
        EXPECT_EQ(unknown.releases(), 0);
    }
    EXPECT_EQ(unknown.releases(), 1) << "released once, at the end of its scope";
    EXPECT_EQ(unknown.references(), 1U);
}

TEST(ComTypesVariantTest, PutAndResetClearTheOldValue)
{
    CountedUnknown unknown;
    Variant value;
    holdUnknown(value.put(), unknown);
    const VARIANT* out = value.put();
    EXPECT_EQ(unknown.releases(), 1) << "put() clears the old value before a call writes a new one";
    EXPECT_EQ(out->vt, VT_EMPTY);

    holdUnknown(value.put(), unknown);
    value.reset();
    EXPECT_EQ(unknown.releases(), 2);
    EXPECT_EQ(value.value().vt, VT_EMPTY);
    value.reset();
    EXPECT_EQ(unknown.releases(), 2) << "an empty one clears nothing";
}

TEST(ComTypesVariantTest, MovingHandsTheValueOnAndItIsClearedOnce)
{
    CountedUnknown unknown;
    {
        Variant first;
        holdUnknown(first.put(), unknown);
        Variant moved(std::move(first));
        EXPECT_EQ(first.value().vt, VT_EMPTY);
        EXPECT_EQ(moved.value().vt, VT_UNKNOWN);

        CountedUnknown other;
        {
            Variant assigned;
            holdUnknown(assigned.put(), other);
            assigned = std::move(moved);
            EXPECT_EQ(other.releases(), 1) << "assigning clears the value it held";
            EXPECT_EQ(unknown.releases(), 0);
        }
        EXPECT_EQ(unknown.releases(), 1);
    }
    EXPECT_EQ(unknown.releases(), 1) << "the moved-from ones clear nothing";
    EXPECT_EQ(unknown.references(), 1U);
}

TEST(ComTypesPropVariantTest, ItStartsEmptyAndClearsWhatItHoldsWhenDestroyed)
{
    CountedUnknown unknown;
    {
        PropVariant value;
        EXPECT_EQ(value.get().vt, VT_EMPTY);
        holdUnknown(value.put(), unknown);
        EXPECT_EQ(value.get().vt, VT_UNKNOWN);
    }
    EXPECT_EQ(unknown.releases(), 1) << "released once, at the end of its scope";
    EXPECT_EQ(unknown.references(), 1U);
}

TEST(ComTypesPropVariantTest, PutAndResetClearTheOldValue)
{
    CountedUnknown unknown;
    PropVariant value;
    holdUnknown(value.put(), unknown);
    const PROPVARIANT* out = value.put();
    EXPECT_EQ(unknown.releases(), 1) << "put() clears the old value before a call writes a new one";
    EXPECT_EQ(out->vt, VT_EMPTY);

    holdUnknown(value.put(), unknown);
    value.reset();
    EXPECT_EQ(unknown.releases(), 2);
    EXPECT_EQ(value.get().vt, VT_EMPTY);
}

TEST(ComTypesPropVariantTest, AStringAPropertyStoreWritesIsFreed)
{
    PropVariant value;
    PROPVARIANT* out = value.put();
    // What IPropertyStore::GetValue writes for a string: a CoTaskMemAlloc'd copy the caller frees.
    const std::wstring_view text = L"Speakers";
    auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc((text.size() + 1) * sizeof(wchar_t)));
    ASSERT_NE(copy, nullptr);
    text.copy(copy, text.size());
    copy[text.size()] = L'\0'; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic) - the allocation's terminator
    out->vt = VT_LPWSTR;
    out->pwszVal = copy;
    EXPECT_EQ(std::wstring_view(value.get().pwszVal), text);
    value.reset();
    EXPECT_EQ(value.get().vt, VT_EMPTY) << "freed with PropVariantClear, and left empty";
}

TEST(ComTypesPropVariantTest, MovingHandsTheValueOnAndItIsClearedOnce)
{
    CountedUnknown unknown;
    {
        PropVariant first;
        holdUnknown(first.put(), unknown);
        PropVariant moved(std::move(first));
        EXPECT_EQ(first.get().vt, VT_EMPTY);

        CountedUnknown other;
        {
            PropVariant assigned;
            holdUnknown(assigned.put(), other);
            assigned = std::move(moved);
            EXPECT_EQ(other.releases(), 1) << "assigning clears the value it held";
            EXPECT_EQ(unknown.releases(), 0);
        }
        EXPECT_EQ(unknown.releases(), 1);
    }
    EXPECT_EQ(unknown.releases(), 1) << "the moved-from ones clear nothing";
    EXPECT_EQ(unknown.references(), 1U);
}

// NOLINTEND(cppcoreguidelines-pro-type-union-access,bugprone-use-after-move,clang-analyzer-cplusplus.Move)

} // namespace
} // namespace Platform::Windows
