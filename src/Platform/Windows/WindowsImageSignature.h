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
//
// A process's image is checked as the file it was mapped from (#4021), never the file at its creation-time path: a
// running image can be renamed and another, signed, file put at the old name. SignatureFunctions::openProcessImage
// binds to that file (Windows::openProcessImageFile(), the binding Look up on VirusTotal hashes through, #3861) and
// holds it; the signature is verified through that handle, and the publisher read through it too (a data mapping of
// the held file, never by a name, #4057). A process whose file can't be bound is answered Unknown, saying
// why, with no publisher: never the answer for whatever file is at the old path. Except a protected process
// (csrss.exe, lsass.exe as a PPL, ...), whose memory map Windows doesn't show, so it can never be bound: it is
// checked by its path, as before #4021, since code integrity required a protected signer to start it.

#include "Platform/AsciiCase.h"
#include "Platform/ImageSignature.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
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

/// The file a process's image was mapped from, bound and held (#4021): the open file and its stamp, and the name it
/// has now (UTF-8), which WinVerifyTrust is given beside the handle.
struct BoundImage
{
    OpenedImage image;
    std::string path;
};

/// Why a process's image couldn't be bound (#4021), and whether the process is protected, so it never can be.
struct BindFailure
{
    std::string why;
    bool protectedProcess = false;
};

/// A process by its identity, as the probe knows it: its PID and its creation time (100 ns FILETIME ticks).
struct ImageProcess
{
    std::int32_t pid = 0;
    std::uint64_t startTimeTicks = 0;

    friend bool operator==(const ImageProcess&, const ImageProcess&) = default;
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
    /// The publisher (CompanyName) in the version info of the file @p file is open on (the handle openImage() or
    /// openProcessImage() gave; @p path names it), empty when it names none (#3703): read by the signature worker
    /// with each check, while the check holds the file (#4021), so the process sampler never opens a new image for
    /// it and the publisher is the checked file's. Nullable: no publisher is read.
    std::string (*readPublisher)(const std::string& path, void* file) = nullptr;
    /// The file @p process's image was mapped from, bound and held (#4021; Windows::openProcessImageFile()), with
    /// its stamp; or why it can't be, and whether the process is protected (then it is checked by its path). Close
    /// the file with closeImage(). Nullable: a process's image is opened by the path it was asked for, with
    /// openImage().
    std::expected<BoundImage, BindFailure> (*openProcessImage)(const ImageProcess& process) = nullptr;
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

    /// The answer kept for @p path at @p stamp; with @p publisher (nullable) set to the publisher kept with it.
    [[nodiscard]] std::optional<SignatureCheck>
    find(const std::string& path, const FileStamp& stamp, std::string* publisher = nullptr) const
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
        if (publisher != nullptr)
        {
            *publisher = found->second.publisher;
        }
        return found->second.check;
    }

    /// Keeps @p check, and the publisher read with it (#4021), for @p path at @p stamp.
    void store(const std::string& path, const FileStamp& stamp, SignatureCheck check, std::string publisher = {})
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
        m_Entries.insert_or_assign(path, Entry{.stamp = stamp, .check = std::move(check), .publisher = std::move(publisher)});
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
        std::string publisher;
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

/// A check's answer and the publisher read with it, through the same handle (#3703, #4021).
struct ImageCheck
{
    SignatureCheck check;
    std::string publisher;
    bool verified = false; ///< WinVerifyTrust was called (not a cached answer, nor a file that couldn't be opened)
};

/// The publisher of the file @p file is open on, through @p fns (empty without readPublisher). One that can't be read
/// (the call throws) is none: it doesn't change the check's answer.
[[nodiscard]] inline std::string readHeldPublisher(const SignatureFunctions& fns, const std::string& path, void* file)
{
    if (fns.readPublisher == nullptr)
    {
        return {};
    }
    try
    {
        return fns.readPublisher(path, file);
    }
    catch (const std::exception&)
    {
        return {};
    }
}

/// The check of @p image, held open (and closed here), as @p path: the embedded signature, else the catalogs, both
/// read through its handle, and with @p withPublisher the publisher while it is held; from @p cache (nullable) when
/// it already holds @p cacheKey with the same file stamp.
[[nodiscard]] inline ImageCheck checkHeldImage(const SignatureFunctions& fns,
                                               SignatureCache* cache,
                                               const std::string& path,
                                               const std::string& cacheKey,
                                               const OpenedImage& image,
                                               bool withPublisher)
{
    const ImageCloser closer(fns.closeImage, image.file);
    const FileStamp& stamp = image.stamp;
    ImageCheck answer;
    if (cache != nullptr)
    {
        if (std::optional<SignatureCheck> cached = cache->find(cacheKey, stamp, &answer.publisher); cached.has_value())
        {
            answer.check = std::move(*cached);
            return answer;
        }
    }
    TrustCheck embedded = fns.verifyEmbeddedSignature(path, image.file);
    std::optional<TrustCheck> catalog = embedded.result == 0 ? std::nullopt : fns.verifyCatalogSignature(path, image.file);
    const SignatureVerdict verdict =
        classifySignature(embedded.result, catalog.has_value() ? std::optional<std::int32_t>(catalog->result) : std::nullopt);
    SignatureCheck& check = answer.check;
    check = SignatureCheck{.signature = verdict.signature, .note = {}, .signer = {}};
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
    answer.verified = true;
    answer.publisher = withPublisher ? readHeldPublisher(fns, path, image.file) : std::string{};
    if (cache != nullptr && verdict.signature != ImageSignature::Unknown)
    {
        cache->store(cacheKey, stamp, check, answer.publisher);
    }
    return answer;
}

/// Whether @p fns has every call a signature check needs.
[[nodiscard]] inline bool canCheckSignatures(const SignatureFunctions& fns) noexcept
{
    return fns.openImage != nullptr && fns.closeImage != nullptr && fns.verifyEmbeddedSignature != nullptr &&
           fns.verifyCatalogSignature != nullptr;
}

/// The note on a process's check when the file its image was mapped from can't be bound (#4021): @p why.
[[nodiscard]] inline std::string unboundImageNote(std::string_view why)
{
    return std::format("{}; its signature and publisher weren't read", why);
}

/// The signature of the image @p process runs, and its publisher (#4021): the file its image was mapped from, bound
/// and held by openProcessImage(), verified through that handle and its publisher read while it is held, never the
/// file at @p path (the name it started with, which another file may have taken since); a protected process, which
/// can't be bound, by @p path. The answer is cached under the bound file's own name and stamp, so a second process of the same file costs a
/// binding and a stamp read, and a decoy at the old name keeps its own entry. Without openProcessImage, or with no @p process, @p path is
/// opened and checked as checkSignature() does, cached under @p cacheKey. NotChecked when the table has no signature calls; Unknown, with
/// no publisher, when the file can't be bound or opened.
[[nodiscard]] inline ImageCheck checkImage(const SignatureFunctions& fns,
                                           SignatureCache* cache,
                                           const std::string& path,
                                           const std::string& cacheKey,
                                           const std::optional<ImageProcess>& process)
{
    if (!canCheckSignatures(fns))
    {
        return {};
    }
    if (process.has_value() && fns.openProcessImage != nullptr)
    {
        std::expected<BoundImage, BindFailure> bound = fns.openProcessImage(*process);
        if (bound.has_value())
        {
            std::string boundKey;
            try
            {
                boundKey = pathKey(bound->path);
            }
            catch (...)
            {
                fns.closeImage(bound->image.file); // checkHeldImage() closes it from here on
                throw;
            }
            return checkHeldImage(fns, cache, bound->path, boundKey, bound->image, true);
        }
        if (!bound.error().protectedProcess)
        {
            return {
                .check = {.signature = ImageSignature::Unknown, .note = unboundImageNote(bound.error().why), .signer = {}},
                .publisher = {},
                .verified = false,
            };
        }
        // A protected process can't be bound (Windows doesn't show its memory map) but could only start from a file
        // with a protected signer: checked by its path, as before #4021.
    }
    const std::optional<OpenedImage> image = fns.openImage(path);
    if (!image.has_value())
    {
        return {
            .check = {.signature = ImageSignature::Unknown, .note = "The image file couldn't be read", .signer = {}},
            .publisher = {},
        };
    }
    return checkHeldImage(fns, cache, path, cacheKey, *image, true);
}

/// @p path's signature through @p fns: the embedded signature, else the catalogs, both read through the
/// one handle openImage() gave, held until the verdict is in; from @p cache (nullable) when it already
/// holds @p cacheKey with the same file stamp. NotChecked when the table has no signature calls; Unknown
/// when the file can't be opened or its stamp read (it's gone, not readable, or open for writing). No publisher
/// is read: the Drivers section's check.
[[nodiscard]] inline SignatureCheck
checkSignature(const SignatureFunctions& fns, SignatureCache* cache, const std::string& path, const std::string& cacheKey)
{
    if (!canCheckSignatures(fns))
    {
        return {};
    }
    const std::optional<OpenedImage> image = fns.openImage(path);
    if (!image.has_value())
    {
        return {.signature = ImageSignature::Unknown, .note = "The image file couldn't be read", .signer = {}};
    }
    return checkHeldImage(fns, cache, path, cacheKey, *image, false).check;
}

/// The publisher of the image @p process runs, read while openProcessImage() holds its file (#4021), or with no
/// binding (no openProcessImage, or no @p process) through openImage()'s of @p path; empty when the file can't be
/// opened or names none. For the probe once the signature worker has ended (#3862): the publisher it reads is still
/// the bound file's, never the file at the old path.
[[nodiscard]] inline std::string
readImagePublisher(const SignatureFunctions& fns, const std::string& path, const std::optional<ImageProcess>& process)
{
    if (fns.readPublisher == nullptr || fns.closeImage == nullptr)
    {
        return {};
    }
    if (process.has_value() && fns.openProcessImage != nullptr)
    {
        std::expected<BoundImage, BindFailure> bound = fns.openProcessImage(*process);
        if (bound.has_value())
        {
            const ImageCloser closer(fns.closeImage, bound->image.file);
            return readHeldPublisher(fns, bound->path, bound->image.file);
        }
        if (!bound.error().protectedProcess)
        {
            return {};
        }
        // A protected process: by its path, as checkImage() checks it.
    }
    if (fns.openImage == nullptr)
    {
        return {};
    }
    const std::optional<OpenedImage> image = fns.openImage(path);
    if (!image.has_value())
    {
        return {};
    }
    const ImageCloser closer(fns.closeImage, image->file);
    return readHeldPublisher(fns, path, image->file);
}

} // namespace Platform::WindowsSignature
