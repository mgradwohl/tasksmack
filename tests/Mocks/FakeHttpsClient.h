#pragma once

/// @file FakeHttpsClient.h
/// @brief An IHttpsClient for tests (#3849): every get() answered with one reply, and every request kept, on whatever
/// thread asks (the update check's worker, VirusTotal's). One copy, so an IHttpsClient change is fixed once.

#include "Platform/IHttpsClient.h"

#include <cstddef>
#include <expected>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace TestMocks
{

class FakeHttpsClient final : public Platform::IHttpsClient
{
  public:
    explicit FakeHttpsClient(std::expected<Platform::HttpsResponse, std::string> reply) : m_Reply(std::move(reply))
    {}

    [[nodiscard]] std::expected<Platform::HttpsResponse, std::string> get(const Platform::HttpsRequest& request,
                                                                          const std::stop_token& /*stop*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Requests.push_back(request);
        return m_Reply;
    }

    /// Every request so far, in order.
    [[nodiscard]] std::vector<Platform::HttpsRequest> requests() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Requests;
    }

    [[nodiscard]] std::size_t requestCount() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Requests.size();
    }

    /// The last request's path, or empty before any.
    [[nodiscard]] std::string lastPath() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Requests.empty() ? std::string{} : m_Requests.back().path;
    }

    /// The reply the next get() gives.
    void setReply(std::expected<Platform::HttpsResponse, std::string> reply)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Reply = std::move(reply);
    }

  private:
    mutable std::mutex m_Mutex;
    std::expected<Platform::HttpsResponse, std::string> m_Reply;
    std::vector<Platform::HttpsRequest> m_Requests;
};

} // namespace TestMocks
