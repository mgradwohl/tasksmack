/// @file test_WindowsImageSignature.cpp
/// @brief An image's Authenticode signature (#3070), its pure parts: how the Signature column and Process
/// Details word, explain and order each result (Platform/ImageSignature.h); and the check itself through
/// fakes (WindowsImageSignature.h): the embedded and catalog answers combined, both read through the
/// handle the stamp came from and closed after; the cache keyed by path (case folded, bounded), file ID,
/// change time and size, a swapped same-size file checked again and a file with no ID never cached
/// (#3292); a catalog member's tag and an error's text. Pure, so built everywhere.

#include "Platform/ImageSignature.h"
#include "Platform/Windows/WindowsImageSignature.h"

#include <gtest/gtest.h>

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::WindowsSignature
{
namespace
{

// --- The words, the tooltip and the order (ImageSignature.h) ------------------------------------------

TEST(ImageSignatureTest, TheColumnNamesAVerifiedSignerOrSaysWhyNot)
{
    EXPECT_EQ(signatureLabel({.signature = ImageSignature::Embedded, .note = {}, .signer = "Microsoft Windows"}), "Microsoft Windows");
    EXPECT_EQ(signatureLabel({.signature = ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows"}), "Microsoft Windows");
    EXPECT_EQ(signatureLabel({.signature = ImageSignature::Embedded, .note = {}, .signer = {}}), "Signed") << "a certificate naming no one";
    EXPECT_EQ(signatureLabel({.signature = ImageSignature::Unsigned, .note = {}, .signer = {}}), "Unsigned");
    EXPECT_EQ(signatureLabel({.signature = ImageSignature::Untrusted, .note = "expired", .signer = {}}), "Invalid");
    EXPECT_EQ(signatureLabel({.signature = ImageSignature::Unknown, .note = "denied", .signer = {}}), "Not checked");
    EXPECT_EQ(signatureLabel({}), "") << "not checked yet: the cell's dash";
}

// #3244: the column's text is a view of the check's own signer, worded where it is drawn: no copy of it.
TEST(ImageSignatureTest, TheLabelViewsTheSignerWithoutCopyingIt)
{
    const SignatureCheck check{.signature = ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows Publisher"};
    const std::string_view label = signatureLabel(check);
    EXPECT_EQ(label, "Microsoft Windows Publisher");
    EXPECT_EQ(label.data(), check.signer.data());
}

// #3244: the counters and snapshots share one check: a copy reads the same one, not a copy of its strings,
// and a check not made yet reads as NotChecked.
TEST(ImageSignatureTest, ASharedCheckIsReadByEveryCopy)
{
    const SharedSignatureCheck shared = SignatureCheck{.signature = ImageSignature::Embedded, .note = {}, .signer = "Contoso Ltd"};
    const SharedSignatureCheck copy = shared; // NOLINT(performance-unnecessary-copy-initialization): the copy is the point
    EXPECT_EQ(&copy.get(), &shared.get());
    EXPECT_EQ(copy.get().signer, "Contoso Ltd");
    EXPECT_EQ(copy, shared);

    const SharedSignatureCheck none;
    EXPECT_EQ(none.get().signature, ImageSignature::NotChecked);
    EXPECT_EQ(none, SharedSignatureCheck(SignatureCheck{}));
    EXPECT_NE(none, shared);
}

TEST(ImageSignatureTest, TheTooltipSaysHowItIsSignedOrWhyNot)
{
    const std::string embedded = signatureTooltip({.signature = ImageSignature::Embedded, .note = {}, .signer = "Contoso"});
    EXPECT_NE(embedded.find("Authenticode signature by Contoso"), std::string::npos) << embedded;
    EXPECT_NE(embedded.find("revocation"), std::string::npos) << "says revocation isn't checked";
    const std::string catalog = signatureTooltip({.signature = ImageSignature::Catalog, .note = {}, .signer = {}});
    EXPECT_NE(catalog.find("catalog signed by an unnamed signer"), std::string::npos) << catalog;
    EXPECT_NE(signatureTooltip({.signature = ImageSignature::Unsigned, .note = {}, .signer = {}}).find("Not signed"), std::string::npos);
    EXPECT_EQ(signatureTooltip({.signature = ImageSignature::Untrusted, .note = "The digest doesn't match", .signer = {}}),
              "Invalid or untrusted signature: The digest doesn't match.");
    EXPECT_EQ(signatureTooltip({.signature = ImageSignature::Untrusted, .note = {}, .signer = {}}), "Invalid or untrusted signature.");
    EXPECT_EQ(signatureTooltip({.signature = ImageSignature::Unknown, .note = "Access is denied", .signer = {}}),
              "The signature couldn't be checked: Access is denied.");
    EXPECT_EQ(signatureTooltip({.signature = ImageSignature::Unknown, .note = {}, .signer = {}}), "The signature couldn't be checked.");
    EXPECT_EQ(signatureTooltip({}), SIGNATURE_NOT_CHECKED_NOTE);
}

TEST(ImageSignatureTest, VerifiedSortsFirstBySignerThenUnsignedInvalidAndUnchecked)
{
    const SignatureCheck intel{.signature = ImageSignature::Catalog, .note = {}, .signer = "Intel Corporation"};
    const SignatureCheck microsoft{.signature = ImageSignature::Embedded, .note = {}, .signer = "microsoft windows"};
    const SignatureCheck microsoftUpper{.signature = ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows"};
    const SignatureCheck unsignedImage{.signature = ImageSignature::Unsigned, .note = {}, .signer = {}};
    const SignatureCheck invalid{.signature = ImageSignature::Untrusted, .note = {}, .signer = {}};
    const SignatureCheck failed{.signature = ImageSignature::Unknown, .note = {}, .signer = {}};
    const SignatureCheck pending{};
    EXPECT_EQ(compareSignature(intel, microsoft), std::strong_ordering::less);
    EXPECT_EQ(compareSignature(microsoft, microsoftUpper), std::strong_ordering::equal)
        << "embedded and catalog rank together, case ignored";
    EXPECT_EQ(compareSignature(microsoft, unsignedImage), std::strong_ordering::less);
    EXPECT_EQ(compareSignature(unsignedImage, invalid), std::strong_ordering::less);
    EXPECT_EQ(compareSignature(invalid, failed), std::strong_ordering::less);
    EXPECT_EQ(compareSignature(failed, pending), std::strong_ordering::less);
    EXPECT_EQ(compareSignature(pending, intel), std::strong_ordering::greater);
    EXPECT_TRUE(isVerifiedSignature(ImageSignature::Embedded));
    EXPECT_TRUE(isVerifiedSignature(ImageSignature::Catalog));
    EXPECT_FALSE(isVerifiedSignature(ImageSignature::Untrusted));
    EXPECT_FALSE(isVerifiedSignature(ImageSignature::NotChecked));
}

// --- The cache key and the catalog tag ----------------------------------------------------------------

TEST(WindowsImageSignatureTest, APathIsKeyedWithItsAsciiCaseFolded)
{
    EXPECT_EQ(pathKey(R"(C:\Windows\System32\NOTEPAD.EXE)"), R"(c:\windows\system32\notepad.exe)");
    EXPECT_EQ(pathKey("C:\\\xC3\x89t\xC3\xA9\\A.exe"), "c:\\\xC3\x89t\xC3\xA9\\a.exe") << "UTF-8 bytes are left alone";
    EXPECT_EQ(pathKey(""), "");
}

TEST(WindowsImageSignatureTest, ACatalogMemberIsTaggedByItsHashInUpperCaseHex)
{
    constexpr std::array<std::uint8_t, 4> HASH{0x00, 0x0F, 0xA5, 0xFF};
    EXPECT_EQ(catalogMemberTag(HASH), "000FA5FF");
    EXPECT_EQ(catalogMemberTag({}), "");
}

/// A file ID whose first byte is @p first, on volume 7.
FileId idOf(std::uint8_t first)
{
    FileId id{.volumeSerial = 7, .id = {}};
    id.id[0] = first;
    return id;
}

/// The stamp of the file with ID idOf(@p id), changed at @p changeTime and @p size bytes long.
FileStamp stampOf(std::uint64_t changeTime, std::uint64_t size = 100, std::uint8_t id = 1)
{
    return {.fileId = idOf(id), .changeTime = changeTime, .size = size};
}

TEST(WindowsImageSignatureTest, AFileIdOfAllZerosIsNone)
{
    std::array<std::uint8_t, FileId::SIZE> bytes{};
    EXPECT_FALSE(makeFileId(7, bytes).has_value()) << "FAT and some network shares give no ID";
    bytes[FileId::SIZE - 1] = 0x5A;
    const std::optional<FileId> id = makeFileId(7, bytes);
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->volumeSerial, 7U);
    EXPECT_EQ(id->id, bytes);
    EXPECT_NE(makeFileId(8, bytes), id) << "the same ID on another volume is another file";
}

TEST(WindowsImageSignatureTest, TheCacheMatchesOnlyTheSameStampAndEmptiesWhenFull)
{
    SignatureCache cache(2);
    const FileStamp stamp = stampOf(10);
    const SignatureCheck signedCheck{.signature = ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows"};
    cache.store("a", stamp, signedCheck);
    EXPECT_EQ(cache.find("a", stamp), signedCheck);
    EXPECT_FALSE(cache.find("a", stampOf(11)).has_value()) << "changed in place";
    EXPECT_FALSE(cache.find("a", stampOf(10, 101)).has_value()) << "another size";
    EXPECT_FALSE(cache.find("a", stampOf(10, 100, 2)).has_value()) << "another file in its place";
    FileStamp otherVolume = stamp;
    otherVolume.fileId->volumeSerial = 8;
    EXPECT_FALSE(cache.find("a", otherVolume).has_value()) << "the same ID on another volume";
    EXPECT_FALSE(cache.find("b", stamp).has_value());

    cache.store("b", stamp, signedCheck);
    cache.store("a", stampOf(12), signedCheck); // a path it holds: replaced, not overflowing
    EXPECT_EQ(cache.size(), 2U);
    cache.store("c", stamp, signedCheck); // a third path: the full cache is emptied first
    EXPECT_EQ(cache.size(), 1U);
    EXPECT_TRUE(cache.find("c", stamp).has_value());
    EXPECT_FALSE(cache.find("b", stamp).has_value());
}

TEST(WindowsImageSignatureTest, AStampWithoutAFileIdIsNeverCached)
{
    SignatureCache cache;
    const FileStamp noId{.fileId = std::nullopt, .changeTime = 10, .size = 100};
    cache.store("fat", noId, SignatureCheck{.signature = ImageSignature::Embedded, .note = {}, .signer = "Contoso"});
    EXPECT_EQ(cache.size(), 0U) << "nothing would say it's the same file next time";
    EXPECT_FALSE(cache.find("fat", noId).has_value());

    cache.store("fat", stampOf(10), SignatureCheck{.signature = ImageSignature::Embedded, .note = {}, .signer = "Contoso"});
    EXPECT_FALSE(cache.find("fat", noId).has_value()) << "an answer kept for a file with an ID is no answer for one without";
}

TEST(WindowsImageSignatureTest, AnErrorIsNamedByItsTextOrItsCode)
{
    EXPECT_EQ(describeError(nullptr, 0x800B0101U), "error 0x800B0101");
    EXPECT_EQ(describeError(nullptr, 5), "error 5");
    EXPECT_EQ(describeError([](std::uint32_t) { return std::string("Access is denied.\r\n"); }, 5), "Access is denied");
    EXPECT_EQ(describeError([](std::uint32_t) { return std::string(" . \n"); }, 31), "error 31") << "nothing but the trailer";
}

TEST(WindowsImageSignatureTest, TheEmbeddedAndCatalogAnswersCombine)
{
    namespace Trust = TrustResult;
    EXPECT_EQ(classifySignature(0, std::nullopt).signature, ImageSignature::Embedded);
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, 0).signature, ImageSignature::Catalog);
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, std::nullopt).signature, ImageSignature::Unsigned);
    EXPECT_EQ(classifySignature(Trust::SUBJECT_FORM_UNKNOWN, std::nullopt).signature, ImageSignature::Unsigned) << "not a PE file";
    EXPECT_EQ(classifySignature(Trust::BAD_DIGEST, std::nullopt).signature, ImageSignature::Untrusted) << "tampered";
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, Trust::EXPLICIT_DISTRUST).signature, ImageSignature::Untrusted);
    EXPECT_EQ(classifySignature(Trust::NO_SIGNATURE, static_cast<std::int32_t>(0x80070005U)).signature, ImageSignature::Unknown);
    EXPECT_TRUE(isTrustFailure(Trust::UNTRUSTED_ROOT));
    EXPECT_FALSE(isTrustFailure(Trust::NO_SIGNATURE));
}

// --- checkSignature() through fakes ---------------------------------------------------------------------

struct Fakes
{
    std::map<std::string, FileStamp> stamps;      // a path missing here can't be opened
    std::map<std::string, std::int32_t> embedded; // its embedded answer; NO_SIGNATURE by default
    std::map<std::string, std::int32_t> catalogs; // its catalog's answer; missing: in no catalog
    std::map<std::string, int> embeddedChecks;
    std::map<std::string, int> catalogChecks;
    int opened = 0;
    int closed = 0;
    std::vector<void*> verifiedThrough; // the file each verify call was given
};

Fakes& fakes()
{
    static Fakes state;
    return state;
}

int g_openFile = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - the fake handle's address

SignatureFunctions fakeFunctions()
{
    return {
        .errorMessage = [](std::uint32_t code)
        { return code == 0x800B0101U ? std::string("The certificate has expired.") : std::string{}; },
        .openImage = [](const std::string& path) -> std::optional<OpenedImage>
        {
            const auto found = fakes().stamps.find(path);
            if (found == fakes().stamps.end())
            {
                return std::nullopt;
            }
            ++fakes().opened;
            return OpenedImage{.file = &g_openFile, .stamp = found->second};
        },
        .closeImage =
            [](void* file)
        {
            EXPECT_EQ(file, &g_openFile);
            ++fakes().closed;
        },
        .verifyEmbeddedSignature =
            [](const std::string& path, void* file)
        {
            ++fakes().embeddedChecks[path];
            fakes().verifiedThrough.push_back(file);
            const auto found = fakes().embedded.find(path);
            const std::int32_t result = found != fakes().embedded.end() ? found->second : TrustResult::NO_SIGNATURE;
            return TrustCheck{.result = result, .signer = result == 0 ? "Embedded Signer" : std::string{}};
        },
        .verifyCatalogSignature = [](const std::string& path, void* file) -> std::optional<TrustCheck>
        {
            ++fakes().catalogChecks[path];
            fakes().verifiedThrough.push_back(file);
            const auto found = fakes().catalogs.find(path);
            if (found == fakes().catalogs.end())
            {
                return std::nullopt;
            }
            return TrustCheck{.result = found->second, .signer = found->second == 0 ? "Microsoft Windows" : std::string{}};
        },
    };
}

class CheckSignatureTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fakes() = Fakes{};
    }
};

TEST_F(CheckSignatureTest, EachAnswerComesWithItsSignerOrItsReason)
{
    fakes().stamps = {{"emb", {}}, {"cat", {}}, {"none", {}}, {"expired", {}}};
    fakes().embedded["emb"] = 0;
    fakes().catalogs["cat"] = 0;
    fakes().embedded["expired"] = static_cast<std::int32_t>(0x800B0101U);
    const SignatureFunctions fns = fakeFunctions();
    EXPECT_EQ(checkSignature(fns, nullptr, "emb", "emb"),
              (SignatureCheck{.signature = ImageSignature::Embedded, .note = {}, .signer = "Embedded Signer"}));
    EXPECT_EQ(checkSignature(fns, nullptr, "cat", "cat"),
              (SignatureCheck{.signature = ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows"}));
    EXPECT_EQ(checkSignature(fns, nullptr, "none", "none").signature, ImageSignature::Unsigned);
    EXPECT_EQ(checkSignature(fns, nullptr, "expired", "expired"),
              (SignatureCheck{.signature = ImageSignature::Untrusted, .note = "The certificate has expired", .signer = {}}));
    const SignatureCheck gone = checkSignature(fns, nullptr, "gone", "gone");
    EXPECT_EQ(gone.signature, ImageSignature::Unknown);
    EXPECT_EQ(gone.note, "The image file couldn't be read");
    EXPECT_EQ(checkSignature(SignatureFunctions{}, nullptr, "emb", "emb").signature, ImageSignature::NotChecked) << "no calls";
    SignatureFunctions noClose = fns;
    noClose.closeImage = nullptr;
    EXPECT_EQ(checkSignature(noClose, nullptr, "emb", "emb").signature, ImageSignature::NotChecked) << "nothing would close it";
    EXPECT_EQ(fakes().opened, fakes().closed) << "every file opened is closed";
}

TEST_F(CheckSignatureTest, BothChecksReadTheFileThroughTheHandleItsStampCameFromAndItIsClosedAfter)
{
    fakes().stamps["cat"] = stampOf(1);
    fakes().catalogs["cat"] = 0;
    const SignatureCheck check = checkSignature(fakeFunctions(), nullptr, "cat", "cat");
    EXPECT_EQ(check.signature, ImageSignature::Catalog);
    EXPECT_EQ(fakes().verifiedThrough, (std::vector<void*>{&g_openFile, &g_openFile})) << "embedded, then catalog";
    EXPECT_EQ(fakes().opened, 1);
    EXPECT_EQ(fakes().closed, 1);
}

TEST_F(CheckSignatureTest, TheFileIsClosedWhenACheckThrows)
{
    fakes().stamps["bad"] = stampOf(1);
    SignatureFunctions fns = fakeFunctions();
    fns.verifyCatalogSignature = [](const std::string& /*path*/, void* /*file*/) -> std::optional<TrustCheck>
    {
        throw std::runtime_error("the catalogs threw");
    };
    EXPECT_THROW((void) checkSignature(fns, nullptr, "bad", "bad"), std::runtime_error);
    EXPECT_EQ(fakes().closed, 1);
}

TEST_F(CheckSignatureTest, TheCacheIsKeyedByPathFileIdChangeTimeAndSize)
{
    fakes().stamps["C:\\App.exe"] = stampOf(1, 10);
    fakes().stamps["c:\\app.exe"] = stampOf(1, 10);
    fakes().embedded["C:\\App.exe"] = 0;
    fakes().embedded["c:\\app.exe"] = 0;
    SignatureCache cache;
    const SignatureFunctions fns = fakeFunctions();
    (void) checkSignature(fns, &cache, "C:\\App.exe", pathKey("C:\\App.exe"));
    (void) checkSignature(fns, &cache, "c:\\app.exe", pathKey("c:\\app.exe"));
    EXPECT_EQ(fakes().embeddedChecks["C:\\App.exe"], 1);
    EXPECT_EQ(fakes().embeddedChecks["c:\\app.exe"], 0) << "the same file in another case: from the cache";
    EXPECT_EQ(fakes().closed, 2) << "a cached answer closes the file too";

    fakes().stamps["C:\\App.exe"].changeTime = 2; // written in place
    (void) checkSignature(fns, &cache, "C:\\App.exe", pathKey("C:\\App.exe"));
    EXPECT_EQ(fakes().embeddedChecks["C:\\App.exe"], 2);
    fakes().stamps["C:\\App.exe"].size = 11;
    (void) checkSignature(fns, &cache, "C:\\App.exe", pathKey("C:\\App.exe"));
    EXPECT_EQ(fakes().embeddedChecks["C:\\App.exe"], 3);
    (void) checkSignature(fns, &cache, "C:\\App.exe", pathKey("C:\\App.exe"));
    EXPECT_EQ(fakes().embeddedChecks["C:\\App.exe"], 3);
}

// #3292: a program running as the user swaps a signed file for a same-size one and sets its write time
// back. The swap is another file (another ID), or the same file written (a new ChangeTime, which
// SetFileTime can't set): either way the old "Signed" isn't reused.
TEST_F(CheckSignatureTest, ASwappedSameSizeFileIsCheckedAgain)
{
    fakes().stamps["app.exe"] = stampOf(5, 4096, 1);
    fakes().embedded["app.exe"] = 0;
    SignatureCache cache;
    const SignatureFunctions fns = fakeFunctions();
    EXPECT_EQ(checkSignature(fns, &cache, "app.exe", "app.exe").signature, ImageSignature::Embedded);

    fakes().stamps["app.exe"] = stampOf(6, 4096, 1); // written in place, its write time set back: a new ChangeTime
    fakes().embedded["app.exe"] = TrustResult::NO_SIGNATURE;
    EXPECT_EQ(checkSignature(fns, &cache, "app.exe", "app.exe").signature, ImageSignature::Unsigned);
    EXPECT_EQ(fakes().embeddedChecks["app.exe"], 2);

    fakes().stamps["app.exe"] = stampOf(5, 4096, 1); // the signed file's stamp exactly again...
    fakes().embedded["app.exe"] = 0;
    EXPECT_EQ(checkSignature(fns, &cache, "app.exe", "app.exe").signature, ImageSignature::Embedded) << "...checked, not cached";
    fakes().stamps["app.exe"] = stampOf(5, 4096, 2); // another file renamed over it, with the same times and size
    fakes().embedded["app.exe"] = TrustResult::NO_SIGNATURE;
    EXPECT_EQ(checkSignature(fns, &cache, "app.exe", "app.exe").signature, ImageSignature::Unsigned);
    EXPECT_EQ(fakes().embeddedChecks["app.exe"], 4);
}

TEST_F(CheckSignatureTest, AFileWithoutAFileIdIsCheckedEveryTime)
{
    fakes().stamps["E:\\tool.exe"] = FileStamp{.fileId = std::nullopt, .changeTime = 1, .size = 10};
    fakes().embedded["E:\\tool.exe"] = 0;
    SignatureCache cache;
    const SignatureFunctions fns = fakeFunctions();
    for (int check = 0; check < 3; ++check)
    {
        EXPECT_EQ(checkSignature(fns, &cache, "E:\\tool.exe", "e:\\tool.exe").signature, ImageSignature::Embedded);
    }
    EXPECT_EQ(fakes().embeddedChecks["E:\\tool.exe"], 3);
    EXPECT_EQ(cache.size(), 0U);
}

TEST_F(CheckSignatureTest, AFailedCheckIsNotCached)
{
    fakes().stamps["locked"] = stampOf(1);
    fakes().catalogs["locked"] = static_cast<std::int32_t>(0x80070020U); // a sharing violation
    SignatureCache cache;
    const SignatureFunctions fns = fakeFunctions();
    const SignatureCheck first = checkSignature(fns, &cache, "locked", "locked");
    EXPECT_EQ(first.signature, ImageSignature::Unknown);
    EXPECT_EQ(first.note, "error 0x80070020");
    EXPECT_EQ(cache.size(), 0U);
    (void) checkSignature(fns, &cache, "locked", "locked");
    EXPECT_EQ(fakes().embeddedChecks["locked"], 2);
}

} // namespace
} // namespace Platform::WindowsSignature
