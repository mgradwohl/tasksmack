#pragma once

/// @file RestartOffered.h
/// @brief Offers "Restart as administrator..." for a test's length (#3658, #3669), as main.cpp does where TaskSmack
/// can restart elevated and has no helper, and keeps the events a press raised. The offer is process-wide
/// (RestartAsAdministrator::setOffered()), so a test that makes one empties it again when it ends.

#include "App/Panels/RestartAsAdministratorButton.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"

#include <vector>

namespace TestSupport
{

class RestartOffered
{
  public:
    RestartOffered()
    {
        App::RestartAsAdministrator::setOffered([this](Core::Event& event) { m_Raised.push_back(event.getEventType()); });
    }
    ~RestartOffered()
    {
        App::RestartAsAdministrator::setOffered({});
    }
    RestartOffered(const RestartOffered&) = delete;
    RestartOffered& operator=(const RestartOffered&) = delete;
    RestartOffered(RestartOffered&&) = delete;
    RestartOffered& operator=(RestartOffered&&) = delete;

    /// The events presses raised, in order.
    [[nodiscard]] const std::vector<Core::EventType>& raised() const noexcept
    {
        return m_Raised;
    }

    /// Whether exactly one press raised the elevation notice (Core::OpenElevationNoticeEvent).
    [[nodiscard]] bool raisedTheNoticeOnce() const
    {
        return m_Raised.size() == 1 && m_Raised.front() == Core::OpenElevationNoticeEvent::getStaticType();
    }

  private:
    std::vector<Core::EventType> m_Raised;
};

} // namespace TestSupport
