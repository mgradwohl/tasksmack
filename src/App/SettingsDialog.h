#pragma once

#include "App/SettingsLayerDetail.h"
#include "App/UserConfig.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <cstdint>
#include <string>
#include <vector>

namespace App::SettingsDialog
{

/// The dialog's popup ID (and its title).
inline constexpr const char* POPUP_ID = "Settings";

// Button labels. The dialog's width is worked out from these before the buttons are drawn (see the
// combo sizing in render()), so each is named once and used for both.
inline constexpr const char* EDIT_CONFIG_LABEL = ICON_FA_FILE_PEN "  Edit Config File";
inline constexpr const char* OPEN_THEMES_LABEL = ICON_FA_FOLDER "  Open Themes Folder";
inline constexpr const char* ABOUT_LABEL = ICON_FA_CIRCLE_INFO "  About TaskSmack";
inline constexpr const char* CANCEL_LABEL = "Cancel";
// "Save", not "Apply": the button writes config.toml and closes the dialog, which is what Save
// means; "Apply" suggested the dialog would stay open (#1273).
inline constexpr const char* SAVE_LABEL = ICON_FA_FLOPPY_DISK "  Save";
// Fills the dialog's controls with the defaults; nothing is written until Save.
inline constexpr const char* RESET_LABEL = ICON_FA_ROTATE_LEFT "  Reset to defaults";
inline constexpr const char* PRIVILEGE_NOTICE_LABEL = "Show limited-data notice";

/// What the dialog keeps while it is open: the controls' values, edited in place until Save.
struct State
{
    bool openRequested = false; ///< Set to open it on the next frame; cleared when it opens
    // Whether one of the dialog's combos was open on the previous frame, so the Escape that closes
    // a combo does not also cancel the dialog (#1129).
    bool comboOpenLastFrame = false;

    // The combos' state while the dialog is open. Save writes only the ones the user picked (#1120).
    Detail::ComboState themeChoice;
    Detail::ComboState fontSizeChoice;
    Detail::ComboState refreshRateChoice;
    Detail::ComboState historyChoice;
    bool forceNativeDecorationsOnWayland = false;
    bool showPrivilegeNotice = true;

    // Previews for stored values that aren't among the options ("Custom (750 ms)"), built on open.
    std::string customThemePreview;
    std::string customRefreshPreview;
    std::string customHistoryPreview;

    std::vector<UI::DiscoveredTheme> themes; ///< The theme combo's options
};

/// What the user asked for this frame, for the layer to carry out.
enum class Action : std::uint8_t
{
    None,
    Save,             ///< Write the picked values (the dialog has closed)
    EditConfig,       ///< Open config.toml in the system's editor
    OpenThemesFolder, ///< Open the user themes folder
    OpenAbout,        ///< Show the About dialog (Settings has closed, without saving)
};

/// Fill @p state from the stored @p settings and the discovered @p themes, as the dialog opens: each
/// combo starts on the stored value, or on none when that isn't one of its options.
void load(State& state, const UserSettings& settings, std::vector<UI::DiscoveredTheme> themes);

/// Move every control to its default and mark it picked, so Save writes it; Cancel still leaves the
/// stored settings as they were (#1273).
void resetToDefaults(State& state);

/// Draw the Settings dialog for this frame: open it when @p state.openRequested is set, then draw it
/// while it is open. Cancel and Escape close it; Reset to defaults changes @p state in place.
///
/// Split out of SettingsLayer, which keeps the event wiring and carries out the returned action
/// (saving, opening files, raising the About event), so the dialog can be rendered headless in the
/// tests (#1547; CONTRIBUTING.md, "Testing App/UI code that needs a live ImGui context").
[[nodiscard]] Action render(State& state);

} // namespace App::SettingsDialog
