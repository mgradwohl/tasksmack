#pragma once

// An executable image's Authenticode signature (#1661, #3070): what WinVerifyTrust says of a driver's
// image (the Drivers section) and of a process's (the Processes table's Signature column and Process
// Details' Signature row). Checked offline: no revocation check, no URL fetched beyond the cache, so a
// revoked certificate still reads as trusted. Pure, so the wording and the order are tested on every
// platform; Linux never fills it (ProcessCapabilities::hasSignature is false there).

#include "Platform/AsciiCase.h"

#include <compare>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Platform
{

/// How an image is signed, from WinVerifyTrust: embedded in the file, or its hash in a signed catalog.
enum class ImageSignature : std::uint8_t
{
    NotChecked, ///< Not read (yet): Linux, no image path, or the check is still to come
    Embedded,   ///< A trusted signature in the file itself
    Catalog,    ///< A trusted catalog holds the file's hash (most of Windows' own files)
    Unsigned,   ///< No signature, embedded or in a catalog
    Untrusted,  ///< Signed, but the signature doesn't verify (bad digest, distrusted, expired...)
    Unknown,    ///< The check itself failed (the file couldn't be opened, say); the note says why
};

/// What checking an image's signature found.
struct SignatureCheck
{
    ImageSignature signature = ImageSignature::NotChecked;
    std::string note;   ///< Untrusted or Unknown: the failing result's text
    std::string signer; ///< Embedded or Catalog: the signing certificate's name ("Microsoft Windows")

    friend bool operator==(const SignatureCheck&, const SignatureCheck&) = default;
};

/// What a signature not checked yet says (the Signature cell's dash, Process Details' row): the check runs in
/// the background, one file at a time, and never comes for a process whose executable path can't be read.
inline constexpr const char* SIGNATURE_NOT_CHECKED_NOTE =
    "Not checked yet: signatures are checked in the background, one file at a time. A protected process's "
    "executable can't be read, so its signature is never checked.";

/// Whether @p signature is a trusted one, embedded or through a catalog.
[[nodiscard]] constexpr bool isVerifiedSignature(ImageSignature signature) noexcept
{
    return signature == ImageSignature::Embedded || signature == ImageSignature::Catalog;
}

/// The Signature column's text (#3070): the signer of a verified image ("Signed" when its certificate
/// names no one), "Unsigned", "Invalid", or "Not checked" when the check failed. Empty while not
/// checked yet: the cell shows the unavailable dash. The UI puts a check mark before a verified signer.
[[nodiscard]] inline std::string signatureLabel(const SignatureCheck& check)
{
    switch (check.signature)
    {
    case ImageSignature::Embedded:
    case ImageSignature::Catalog:
        return check.signer.empty() ? std::string("Signed") : check.signer;
    case ImageSignature::Unsigned:
        return "Unsigned";
    case ImageSignature::Untrusted:
        return "Invalid";
    case ImageSignature::Unknown:
        return "Not checked";
    case ImageSignature::NotChecked:
        break;
    }
    return {};
}

/// What hovering the Signature cell or Process Details' row says: how the image is signed and by whom, or
/// why it isn't verified. Revocation isn't checked (the check never goes to the network), so it says so.
[[nodiscard]] inline std::string signatureTooltip(const SignatureCheck& check)
{
    constexpr std::string_view OFFLINE = "Checked offline: certificate revocation isn't checked.";
    const std::string signer = check.signer.empty() ? std::string("an unnamed signer") : check.signer;
    switch (check.signature)
    {
    case ImageSignature::Embedded:
        return std::format("Verified: the file carries an Authenticode signature by {}.\n{}", signer, OFFLINE);
    case ImageSignature::Catalog:
        return std::format("Verified: the file's hash is in a Windows catalog signed by {}.\n{}", signer, OFFLINE);
    case ImageSignature::Unsigned:
        return "Not signed: the file carries no signature, and no Windows catalog holds its hash.";
    case ImageSignature::Untrusted:
        return check.note.empty() ? std::string("Invalid or untrusted signature.")
                                  : std::format("Invalid or untrusted signature: {}.", check.note);
    case ImageSignature::Unknown:
        return check.note.empty() ? std::string("The signature couldn't be checked.")
                                  : std::format("The signature couldn't be checked: {}.", check.note);
    case ImageSignature::NotChecked:
        break;
    }
    return SIGNATURE_NOT_CHECKED_NOTE;
}

/// The Signature column's order: verified images first, by signer (ASCII case ignored), then unsigned,
/// invalid, unchecked and not yet checked. Embedded and catalog signatures rank together: both are verified.
[[nodiscard]] constexpr std::strong_ordering compareSignature(const SignatureCheck& a, const SignatureCheck& b) noexcept
{
    const auto rank = [](ImageSignature signature) noexcept -> int
    {
        switch (signature)
        {
        case ImageSignature::Embedded:
        case ImageSignature::Catalog:
            return 0;
        case ImageSignature::Unsigned:
            return 1;
        case ImageSignature::Untrusted:
            return 2;
        case ImageSignature::Unknown:
            return 3;
        case ImageSignature::NotChecked:
            break;
        }
        return 4;
    };
    if (const std::strong_ordering byKind = rank(a.signature) <=> rank(b.signature); byKind != 0)
    {
        return byKind;
    }
    return compareIgnoreCase(a.signer, b.signer);
}

} // namespace Platform
