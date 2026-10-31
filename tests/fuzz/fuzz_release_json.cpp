#include "Domain/UpdateCheck.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The update check's reply reader (#3093) on arbitrary bytes: GitHub's release JSON is the one input
// TaskSmack reads from the network over HTTPS. Nothing may read past the input, recurse, or throw, and
// what it reads must pass through the version and URL checks without harm.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) // NOLINT(readability-identifier-naming)
{
    namespace UpdateCheck = Domain::UpdateCheck;
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    if (const std::optional<UpdateCheck::ReleaseFields> fields = UpdateCheck::readReleaseFields(text); fields.has_value())
    {
        if (fields->tagName.has_value())
        {
            static_cast<void>(UpdateCheck::parseVersion(*fields->tagName));
        }
        if (fields->htmlUrl.has_value())
        {
            static_cast<void>(UpdateCheck::isReleasePageUrl(*fields->htmlUrl));
        }
    }
    static_cast<void>(UpdateCheck::evaluateResponse(200, text, UpdateCheck::Version{}));
    if (const std::optional<UpdateCheck::Version> version = UpdateCheck::parseVersion(text); version.has_value())
    {
        static_cast<void>(UpdateCheck::compare(*version, UpdateCheck::Version{}));
        static_cast<void>(version->text());
    }
    return 0;
}
