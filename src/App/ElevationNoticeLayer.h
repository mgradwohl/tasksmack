#pragma once

#include "App/AccessNoticeDialog.h"
#include "App/AdministratorHold.h"
#include "App/ElevationNoticeDialog.h"
#include "Core/Layer.h"
#include "Platform/IPrivilegedBroker.h"

#include <cstdint>
#include <memory>

namespace App
{

/// The startup notice's layer: shown once at startup when capabilities().hasReducedPrivileges is true
/// and UserConfig::showPrivilegeNotice is true. The user can suppress future notices via its "Don't
/// show this again" checkbox.
///
/// On Linux it is Standard or Administrator (#2728, AccessNoticeDialog): Administrator holds the
/// privileged helper for the session (IPrivilegedBroker::holdForSession()), on AdministratorHold's worker since
/// it waits for the password prompt; a dismissed or refused prompt leaves TaskSmack Standard, with a note.
/// On Windows it is still the Limited Data notice (ElevationNoticeDialog) until TaskSmack can restart
/// itself elevated, which is what Administrator means there.
///
/// Thread safety: All layer lifecycle methods are called from the main thread only,
/// as required by SDL and ImGui. AdministratorHold's worker only calls the broker and publishes its answer.
class ElevationNoticeLayer : public Core::Layer
{
  public:
    ElevationNoticeLayer();
    ~ElevationNoticeLayer() override;

    ElevationNoticeLayer(const ElevationNoticeLayer&) = delete;
    ElevationNoticeLayer& operator=(const ElevationNoticeLayer&) = delete;
    ElevationNoticeLayer(ElevationNoticeLayer&&) = delete;
    ElevationNoticeLayer& operator=(ElevationNoticeLayer&&) = delete;

    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void requestOpen(std::uint8_t missingPrivileges);
    static void suppressFutureNotices();
    /// Starts holding the helper for the session on m_AdministratorHold's worker.
    void startAdministrator();
    /// Takes the hold's answer in once it has one.
    void collectAdministrator();

    ElevationNoticeDialog::State m_State;
    AccessNoticeDialog::State m_AccessState;
    std::shared_ptr<Platform::IPrivilegedBroker> m_Broker;
    AdministratorHold m_AdministratorHold; // Last: its worker is stopped (the hold cancelled) and joined first
};

} // namespace App
