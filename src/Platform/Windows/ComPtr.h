#pragma once

// Minimal RAII wrapper for COM interface pointers (AddRef/Release only; callers
// still invoke QueryInterface directly and hand the result to
// releaseAndGetAddressOf()). Standing in for Microsoft::WRL::ComPtr, whose header
// (wrl/client.h) is only shipped under the Windows Kits "winrt" include tree,
// which is not on this project's include path.

#include <cstddef>

namespace Platform
{

template<typename T> class ComPtr
{
  public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept
    {}

    ComPtr(const ComPtr& other) noexcept : m_Ptr(other.m_Ptr)
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->AddRef();
        }
    }

    ComPtr(ComPtr&& other) noexcept : m_Ptr(other.m_Ptr)
    {
        other.m_Ptr = nullptr;
    }

    ~ComPtr()
    {
        reset();
    }

    ComPtr& operator=(const ComPtr& other) noexcept
    {
        if (this != &other)
        {
            T* newPtr = other.m_Ptr;
            if (newPtr != nullptr)
            {
                newPtr->AddRef();
            }
            reset();
            m_Ptr = newPtr;
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_Ptr = other.m_Ptr;
            other.m_Ptr = nullptr;
        }
        return *this;
    }

    /// Releases the current interface, if any.
    void reset() noexcept
    {
        if (m_Ptr != nullptr)
        {
            m_Ptr->Release();
            m_Ptr = nullptr;
        }
    }

    /// Releases the current interface and returns the address of the internal
    /// pointer, for use as an out-parameter to COM APIs that write a new interface.
    [[nodiscard]] T** releaseAndGetAddressOf() noexcept
    {
        reset();
        return &m_Ptr;
    }

    [[nodiscard]] T* get() const noexcept
    {
        return m_Ptr;
    }

    T* operator->() const noexcept
    {
        return m_Ptr;
    }

    explicit operator bool() const noexcept
    {
        return m_Ptr != nullptr;
    }

  private:
    T* m_Ptr{nullptr};
};

} // namespace Platform
