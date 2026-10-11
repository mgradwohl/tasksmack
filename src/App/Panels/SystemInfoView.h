#pragma once

// The System tab's page (#1399): the toolbar (filter, Show identifiers, Copy all, Refresh) and the
// titled label/value sections. Split from SystemInfoPanel, which owns the model and its reads, so it
// runs headless in TaskSmackTests; the content itself is built in SystemInfoSections.h.

#include "App/Panels/SystemInfoSections.h"
#include "Core/GraphicsHostInfo.h"
#include "Domain/SystemInfoModel.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <string>
#include <vector>

namespace App
{

/// What the view drew this frame.
enum class SystemInfoViewContent : std::uint8_t
{
    Unsupported, ///< The platform has no System Information facts.
    Loading,     ///< No read yet.
    Sections,
};

/// The view's state between frames. The sections are rebuilt only when a new read is published, and
/// the visible rows only when that, the filter or the identifier toggle changes (#580).
struct SystemInfoViewState
{
    std::string filter;
    bool showIdentifiers = false;

    /// Core's graphics facts for the Graphics & displays section (#1519), set by the panel with each new
    /// read; the sections are built from it and the snapshot together.
    Core::GraphicsHostInfo host;

    std::vector<SystemInfo::Section> sections;
    std::uint64_t sectionsVersion = 0;
    std::string readAtText; ///< "Read at HH:MM:SS", with the sections.

    std::vector<SystemInfo::VisibleSection> visible;
    std::uint64_t visibleVersion = 0;
    std::string visibleFilter;
    bool visibleShowIdentifiers = false;

    std::string copiedText; ///< What the last Copy put on the clipboard.
};

struct SystemInfoViewResult
{
    SystemInfoViewContent content = SystemInfoViewContent::Loading;
    bool refreshRequested = false; ///< Refresh was clicked.
};

/// Draws the page into the current window.
/// @param snapshot Null is treated as no read yet.
/// @param reading  A read is in flight: Refresh is disabled.
SystemInfoViewResult renderSystemInfoView(const Domain::SystemInfoSnapshot* snapshot,
                                          const Platform::SystemInfoCapabilities& capabilities,
                                          bool reading,
                                          SystemInfoViewState& state);

} // namespace App
