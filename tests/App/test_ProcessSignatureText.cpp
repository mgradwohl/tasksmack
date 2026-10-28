/// @file test_ProcessSignatureText.cpp
/// @brief How the Signature column and Process Details' Signature row colour and word an image's
/// signature (#3070): ProcessSignatureText's colour per result, and the row's value with its check mark.

#include "App/Panels/ProcessSignatureText.h"
#include "Platform/ImageSignature.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <string>

namespace
{

using Platform::ImageSignature;

/// A scheme whose signature colours are all distinct, so a test can tell which one was chosen.
UI::ColorScheme distinctScheme()
{
    UI::ColorScheme scheme{};
    scheme.textPrimary = ImVec4(1.0F, 0.0F, 0.0F, 1.0F);
    scheme.textWarning = ImVec4(0.0F, 1.0F, 0.0F, 1.0F);
    scheme.textError = ImVec4(0.0F, 0.0F, 1.0F, 1.0F);
    scheme.textMuted = ImVec4(0.5F, 0.5F, 0.5F, 1.0F);
    return scheme;
}

void expectSameColor(const ImVec4& actual, const ImVec4& expected)
{
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.z, expected.z);
    EXPECT_FLOAT_EQ(actual.w, expected.w);
}

TEST(ProcessSignatureTextTest, EachResultHasItsColour)
{
    const UI::ColorScheme scheme = distinctScheme();
    expectSameColor(App::ProcessSignatureText::textColor(ImageSignature::Embedded, scheme), scheme.textPrimary);
    expectSameColor(App::ProcessSignatureText::textColor(ImageSignature::Catalog, scheme), scheme.textPrimary);
    expectSameColor(App::ProcessSignatureText::textColor(ImageSignature::Unsigned, scheme), scheme.textWarning);
    expectSameColor(App::ProcessSignatureText::textColor(ImageSignature::Untrusted, scheme), scheme.textError);
    expectSameColor(App::ProcessSignatureText::textColor(ImageSignature::Unknown, scheme), scheme.textMuted);
    expectSameColor(App::ProcessSignatureText::textColor(ImageSignature::NotChecked, scheme), scheme.textMuted);
}

TEST(ProcessSignatureTextTest, TheRowMarksAVerifiedSignerAndSaysWhenNotCheckedYet)
{
    const Platform::SignatureCheck verified{.signature = ImageSignature::Catalog, .note = {}, .signer = "Microsoft Windows"};
    EXPECT_EQ(App::ProcessSignatureText::detailsValue(verified, Platform::signatureLabel(verified)),
              std::string(App::ProcessSignatureText::VERIFIED_MARK) + " Microsoft Windows");
    const Platform::SignatureCheck invalid{.signature = ImageSignature::Untrusted, .note = "expired", .signer = {}};
    EXPECT_EQ(App::ProcessSignatureText::detailsValue(invalid, Platform::signatureLabel(invalid)), "Invalid");
    EXPECT_EQ(App::ProcessSignatureText::detailsValue({}, ""), "Not checked yet");
}

} // namespace
