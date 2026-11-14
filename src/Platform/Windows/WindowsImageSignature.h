#pragma once

// The pure part of Windows' image signature check (#1661, #3070), shared by the Drivers section's
// driver images and the Processes table's process images: WinVerifyTrust's results told apart, the
// embedded and catalog answers combined into one ImageSignature, the cache of answers by path and file
// stamp, and checkSignature(), which runs a check through a table of calls. Free of <windows.h>, so it
// is tested on every platform (tests/Platform/WindowsMath/test_WindowsImageSignature.cpp), Linux's
// coverage included; WindowsSignatureCheck.cpp supplies the real calls.
//
// A check holds the image open from its stamp read to its verdict (#3292): one handle, opened for read and
// shared for read only, so nobody can write, rename or delete the file in between, and both WinVerifyTrust
// calls read the file through it. The stamp is the file's identity (volume serial and 128-bit file ID)
// with its ChangeTime, size and change journal USN (#3313), not its path and last write time, which a
// program running as the user can reset with SetFileTime after swapping in a same-size file.

#include "Platform/AsciiCase.h"
#include "Platform/ImageSignature.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Platform::WindowsSignature
{

/// A file's identity on its machine (FILE_ID_INFO): its volume's serial number and its 128-bit file ID.
/// Another file put in its place, by a rename, a copy or a delete and re-create, has another ID.
struct FileId
{
    static constexpr std::size_t SIZE = 16;
    std::uint64_t volumeSerial = 0;
    std::array<std::uint8_t, SIZE> id{};

    friend bool operator==(const FileId&, const FileId&) = default;
};

/// The file ID FILE_ID_INFO gives, or nullopt when it is all zeros: a file system that keeps no IDs (FAT,
/// some network shares) has none to give.
[[nodiscard]] constexpr std::optional<FileId> makeFileId(std::uint64_t volumeSerial,
                                                         std::span<const std::uint8_t, FileId::SIZE> id) noexcept
{
    if (std::ranges::all_of(id, [](std::uint8_t byte) { return byte == 0; }))
    {
        return std::nullopt;
    }
    FileId fileId{.volumeSerial = volumeSerial, .id = {}};
    std::ranges::copy(id, fileId.id.begin());
    return fileId;
}

/// An image file as the signature cache knows it (#3292): another file in its place, or the same file
/// changed in any way, is checked again. Read through the handle the check holds.
struct FileStamp
{
    std::optional<FileId> fileId; ///< nullopt when the file system gives none: the answer is never cached
    /// FILE_BASIC_INFO's ChangeTime, 100 ns ticks: every write, and every change to the file's attributes
    /// or times, moves it, and SetFileTime can't set it back. (SetFileInformationByHandle's FileBasicInfo
    /// can, which is what the USN is for.)
    std::uint64_t changeTime = 0;
    std::uint64_t size = 0;
    /// The USN of the file's most recent change journal record (FSCTL_READ_FILE_USN_DATA, #3313). Not a
    /// per-write counter: while a file stays open, NTFS folds repeated changes of one kind into one record.
    /// But it moves on whenever a new record is written for the file: the first change of each kind on an
    /// open (a write, then the basic-info change that restores ChangeTime) and the close after them. So
    /// an in-place rewrite always leaves a newer USN, and user mode can't set it back. 0 when the volume keeps no journal (FAT, most
    /// network shares, a volume whose journal is off) or the read fails: the stamp is then the other three, as before #3313, and still
    /// cached.
    std::uint64_t usn = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

/// An image file held open for its check, and its stamp read through that handle.
struct OpenedImage
{
    /// The open file: on Windows a HANDLE, opened for read and shared for read only, so the file can't be
    /// written, renamed or deleted while it is held. Opaque here; closed by SignatureFunctions::closeImage.
    void* file = nullptr;
    FileStamp stamp;
};

/// One WinVerifyTrust call's answer.
struct TrustCheck
{
    std::int32_t result = 0; ///< 0 when the signature is trusted, else the failing HRESULT
    std::string signer;      ///< When trusted: the signing certificate's simple name; empty if unread
};

/// The calls checkSignature() makes, each taking the image's path as UTF-8. Without any of the last four,
/// signatures aren't checked.
struct SignatureFunctions
{
    /// FormatMessage's text for a Win32 error, NTSTATUS or HRESULT; empty when there is none. Nullable:
    /// a failure is then named by its code.
    std::string (*errorMessage)(std::uint32_t code) = nullptr;
    /// The file opened for its check, with its stamp; nullopt when it can't be opened or its stamp read.
    std::optional<OpenedImage> (*openImage)(const std::string& path) = nullptr;
    /// Closes what openImage() opened.
    void (*closeImage)(void* file) = nullptr;
    /// WinVerifyTrust's answer for the file's own (embedded) signature, read through @p file (the handle
    /// openImage() gave): result 0 when it is trusted.
    TrustCheck (*verifyEmbeddedSignature)(const std::string& path, void* file) = nullptr;
    /// WinVerifyTrust's answer for the catalog that holds the file's hash, hashed through @p file: result 0
    /// when it is trusted; nullopt when no catalog holds it; a failure's HRESULT when the catalogs couldn't
    /// be searched.
    std::optional<TrustCheck> (*verifyCatalogSignature)(const std::string& path, void* file) = nullptr;
    /// The publisher (CompanyName) in the file's version info, empty when it names none (#3703): read by the
    /// signature worker with each check, so the process sampler never opens a new image for it. Nullable:
    /// no publisher is read.
    std::string (*readPublisher)(const std::string& path) = nullptr;
};

/// WinVerifyTrust results (HRESULTs) that the check tells apart.
namespace TrustResult
{
inline constexpr std::int32_t NO_SIGNATURE = static_cast<std::int32_t>(0x800B0100U);         ///< TRUST_E_NOSIGNATURE
inline constexpr std::int32_t SUBJECT_FORM_UNKNOWN = static_cast<std::int32_t>(0x800B0003U); ///< TRUST_E_SUBJECT_FORM_UNKNOWN
inline constexpr std::int32_t SUBJECT_NOT_TRUSTED = static_cast<std::int32_t>(0x800B0004U);  ///< TRUST_E_SUBJECT_NOT_TRUSTED
inline constexpr std::int32_t EXPIRED = static_cast<std::int32_t>(0x800B0101U);              ///< CERT_E_EXPIRED
inline constexpr std::int32_t UNTRUSTED_ROOT = static_cast<std::int32_t>(0x800B0109U);       ///< CERT_E_UNTRUSTEDROOT
inline constexpr std::int32_t REVOKED = static_cast<std::int32_t>(0x800B010CU);              ///< CERT_E_REVOKED
inline constexpr std::int32_t UNTRUSTED_TEST_ROOT = static_cast<std::int32_t>(0x800B010DU);  ///< CERT_E_UNTRUSTEDTESTROOT
inline constexpr std::int32_t WRONG_USAGE = static_cast<std::int32_t>(0x800B0110U);          ///< CERT_E_WRONG_USAGE
inline constexpr std::int32_t EXPLICIT_DISTRUST = static_cast<std::int32_t>(0x800B0111U);    ///< TRUST_E_EXPLICIT_DISTRUST
inline constexpr std::int32_t CERT_SIGNATURE = static_cast<std::int32_t>(0x80096004U);       ///< TRUST_E_CERT_SIGNATURE
inline constexpr std::int32_t BAD_DIGEST = static_cast<std::int32_t>(0x80096010U);           ///< TRUST_E_BAD_DIGEST
} // namespace TrustResult

/// A failing WinVerifyTrust result that says the signature is there but isn't trusted, rather than that
/// the check couldn't be made.
[[nodiscard]] constexpr bool isTrustFailure(std::int32_t result) noexcept
{
    constexpr std::array<std::int32_t, 9> FAILURES{
        TrustResult::SUBJECT_NOT_TRUSTED,
        TrustResult::EXPIRED,
        TrustResult::UNTRUSTED_ROOT,
        TrustResult::REVOKED,
        TrustResult::UNTRUSTED_TEST_ROOT,
        TrustResult::WRONG_USAGE,
        TrustResult::EXPLICIT_DISTRUST,
        TrustResult::CERT_SIGNATURE,
        TrustResult::BAD_DIGEST,
    };
    return std::ranges::find(FAILURES, result) != FAILURES.end();
}

/// The signature and the result that decided it (0 when signed).
struct SignatureVerdict
{
    ImageSignature signature = ImageSignature::NotChecked;
    std::int32_t result = 0;
};

/// The signature from the embedded check's result and, when that failed, the catalog check's.
[[nodiscard]] constexpr SignatureVerdict classifySignature(std::int32_t embedded, std::optional<std::int32_t> catalog) noexcept
{
    if (embedded == 0)
    {
        return {.signature = ImageSignature::Embedded, .result = 0};
    }
    if (catalog.has_value())
    {
        if (*catalog == 0)
        {
            return {.signature = ImageSignature::Catalog, .result = 0};
        }
        return {.signature = isTrustFailure(*catalog) ? ImageSignature::Untrusted : ImageSignature::Unknown, .result = *catalog};
    }
    if (embedded == TrustResult::NO_SIGNATURE || embedded == TrustResult::SUBJECT_FORM_UNKNOWN)
    {
        return {.signature = ImageSignature::Unsigned, .result = embedded};
    }
    return {.signature = isTrustFailure(embedded) ? ImageSignature::Untrusted : ImageSignature::Unknown, .result = embedded};
}

/// The key a path is cached under: Windows paths name the same file whatever their case, so ASCII
/// letters are folded. (A non-ASCII letter in another case is another key: at worst a second check.)
[[nodiscard]] inline std::string pathKey(std::string_view path)
{
    std::string key(path);
    std::ranges::transform(key, key.begin(), foldAscii);
    return key;
}

/// A catalog's name for its member (WINTRUST_CATALOG_INFO::pcwszMemberTag): the file's hash in upper-case
/// hex, two digits a byte.
[[nodiscard]] inline std::string catalogMemberTag(std::span<const std::uint8_t> hash)
{
    constexpr std::string_view DIGITS = "0123456789ABCDEF";
    constexpr unsigned NIBBLE = 4;
    constexpr std::uint8_t LOW = 0x0F;
    std::string tag;
    tag.reserve(hash.size() * 2);
    for (const std::uint8_t byte : hash)
    {
        tag += DIGITS[byte >> NIBBLE];
        tag += DIGITS[byte & LOW];
    }
    return tag;
}

/// Signature checks by image path and file stamp {file ID, change time, size, USN}: another file in the path's
/// place, or the file changed, has a new stamp and is checked again. A stamp without a file ID is never
/// kept, nor found: that file is checked every time. Only definite answers are kept: a check that failed
/// is tried again on the next request. Bounded at @p maxEntries paths (unbounded by default, as the
/// drivers are few): storing a new path in a full cache empties it first, as the publisher cache does
/// (#775), so a long-running session with many distinct executables can't grow it without end.
/// Thread-safe.
class SignatureCache
{
  public:
    static constexpr std::size_t UNBOUNDED = std::numeric_limits<std::size_t>::max();

    explicit SignatureCache(std::size_t maxEntries = UNBOUNDED) : m_MaxEntries(maxEntries)
    {}

    [[nodiscard]] std::optional<SignatureCheck> find(const std::string& path, const FileStamp& stamp) const
    {
        if (!stamp.fileId.has_value())
        {
            return std::nullopt;
        }
        const std::scoped_lock lock(m_Mutex);
        const auto found = m_Entries.find(path);
        if (found == m_Entries.end() || found->second.stamp != stamp)
        {
            return std::nullopt;
        }
        return found->second.check;
    }

    void store(const std::string& path, const FileStamp& stamp, SignatureCheck check)
    {
        if (!stamp.fileId.has_value())
        {
            return; // nothing would say it's the same file next time
        }
        std::unordered_map<std::string, Entry> overflowed; // freed after the lock is released
        const std::scoped_lock lock(m_Mutex);
        if (m_Entries.size() >= m_MaxEntries && !m_Entries.contains(path))
        {
            std::swap(overflowed, m_Entries);
        }
        m_Entries.insert_or_assign(path, Entry{.stamp = stamp, .check = std::move(check)});
    }

    [[nodiscard]] std::size_t size() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Entries.size();
    }

  private:
    struct Entry
    {
        FileStamp stamp;
        SignatureCheck check;
    };
    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, Entry> m_Entries;
    std::size_t m_MaxEntries;
};

/// An error code's text through @p errorMessage (nullable), without the trailing period and line break
/// FormatMessage adds; the code itself when there is no text ("error 0xC035001E" for an NTSTATUS or
/// HRESULT, "error 31" otherwise).
[[nodiscard]] inline std::string describeError(std::string (*errorMessage)(std::uint32_t), std::uint32_t code)
{
    std::string text = errorMessage != nullptr ? errorMessage(code) : std::string{};
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0 || text.back() == '.'))
    {
        text.pop_back();
    }
    if (!text.empty())
    {
        return text;
    }
    constexpr std::uint32_t SEVERITY_BIT = 0x80000000U;
    return (code & SEVERITY_BIT) != 0 ? std::format("error 0x{:08X}", code) : std::format("error {}", code);
}

/// Closes an OpenedImage's file when it goes out of scope.
class ImageCloser
{
  public:
    ImageCloser(void (*close)(void*), void* file) noexcept : m_Close(close), m_File(file)
    {}
    ImageCloser(const ImageCloser&) = delete;
    ImageCloser& operator=(const ImageCloser&) = delete;
    ImageCloser(ImageCloser&&) = delete;
    ImageCloser& operator=(ImageCloser&&) = delete;
    ~ImageCloser()
    {
        m_Close(m_File);
    }

  private:
    void (*m_Close)(void*);
    void* m_File;
};

/// @p path's signature through @p fns: the embedded signature, else the catalogs, both read through the
/// one handle openImage() gave, held until the verdict is in; from @p cache (nullable) when it already
/// holds @p cacheKey with the same file stamp. NotChecked when the table has no signature calls; Unknown
/// when the file can't be opened or its stamp read (it's gone, not readable, or open for writing).
[[nodiscard]] inline SignatureCheck
checkSignature(const SignatureFunctions& fns, SignatureCache* cache, const std::string& path, const std::string& cacheKey)
{
    if (fns.openImage == nullptr || fns.closeImage == nullptr || fns.verifyEmbeddedSignature == nullptr ||
        fns.verifyCatalogSignature == nullptr)
    {
        return {};
    }
    const std::optional<OpenedImage> image = fns.openImage(path);
    if (!image.has_value())
    {
        return {.signature = ImageSignature::Unknown, .note = "The image file couldn't be read", .signer = {}};
    }
    const ImageCloser closer(fns.closeImage, image->file);
    const FileStamp& stamp = image->stamp;
    if (cache != nullptr)
    {
        if (std::optional<SignatureCheck> cached = cache->find(cacheKey, stamp); cached.has_value())
        {
            return std::move(*cached);
        }
    }
    TrustCheck embedded = fns.verifyEmbeddedSignature(path, image->file);
    std::optional<TrustCheck> catalog = embedded.result == 0 ? std::nullopt : fns.verifyCatalogSignature(path, image->file);
    const SignatureVerdict verdict =
        classifySignature(embedded.result, catalog.has_value() ? std::optional<std::int32_t>(catalog->result) : std::nullopt);
    SignatureCheck check{.signature = verdict.signature, .note = {}, .signer = {}};
    if (verdict.signature == ImageSignature::Untrusted || verdict.signature == ImageSignature::Unknown)
    {
        check.note = describeError(fns.errorMessage, static_cast<std::uint32_t>(verdict.result)); // an HRESULT's bits
    }
    else if (verdict.signature == ImageSignature::Embedded)
    {
        check.signer = std::move(embedded.signer);
    }
    else if (verdict.signature == ImageSignature::Catalog && catalog.has_value())
    {
        check.signer = std::move(catalog->signer);
    }
    if (cache != nullptr && verdict.signature != ImageSignature::Unknown)
    {
        cache->store(cacheKey, stamp, check);
    }
    return check;
}

} // namespace Platform::WindowsSignature
