#pragma once

// How the Processes table's Signature column and Process Details' Signature row show a process image's
// Authenticode signature (#3070), shared so the two say and colour it the same way: a check mark before
// a verified signer, Unsigned in the warning colour, Invalid in the error colour, a check that failed
// muted. The words themselves are Platform::signatureLabel()'s, a view of the snapshot's signature worded
// where it is drawn (#3244). Pure: it reads a ColorScheme and makes no ImGui calls.

#include "Platform/ImageSignature.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <string>
#include <string_view>

namespace App::ProcessSignatureText
{

/// The check mark drawn before a verified signer.
inline constexpr std::string_view VERIFIED_MARK = ICON_FA_CHECK;

/// The theme colour of a signature's text: the primary text for a verified signer (its mark is in the
/// success colour), the warning colour for Unsigned, the error colour for Invalid, muted otherwise.
[[nodiscard]] inline ImVec4 textColor(Platform::ImageSignature signature, const UI::ColorScheme& scheme) noexcept
{
    switch (signature)
    {
    case Platform::ImageSignature::Embedded:
    case Platform::ImageSignature::Catalog:
        return scheme.textPrimary;
    case Platform::ImageSignature::Unsigned:
        return scheme.textWarning;
    case Platform::ImageSignature::Untrusted:
        return scheme.textError;
    case Platform::ImageSignature::Unknown:
    case Platform::ImageSignature::NotChecked:
        break;
    }
    return scheme.textMuted;
}

/// Process Details' value for the row: the check mark and the signer when verified, else the column's
/// text; "Not checked yet" before the check is in.
[[nodiscard]] inline std::string detailsValue(const Platform::SignatureCheck& check, std::string_view label)
{
    if (label.empty())
    {
        return "Not checked yet";
    }
    if (Platform::isVerifiedSignature(check.signature))
    {
        std::string value(VERIFIED_MARK);
        value += ' ';
        value += label;
        return value;
    }
    return std::string(label);
}

} // namespace App::ProcessSignatureText
