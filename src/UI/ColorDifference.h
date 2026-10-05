#pragma once

// Perceptual colour difference, and how colours look to a viewer with red/green colour vision
// deficiency, for checking that series drawn on the same chart can be told apart (#1197).
//
// Pure arithmetic with no ImGui runtime dependency, so the bundled themes can be checked in unit tests
// (see ColorContrast.h for the same pattern applied to luminance contrast).
//
// - sRGB -> CIELAB uses the sRGB primaries and the D65 white point.
// - CIEDE2000 follows Sharma, Wu and Dalal (2005), "The CIEDE2000 Color-Difference Formula:
//   Implementation Notes, Supplementary Test Data, and Mathematical Observations", with kL = kC = kH = 1.
// - Dichromacy simulation uses the severity-1.0 matrices of Machado, Oliveira and Fernandes (2009),
//   "A Physiologically-based Model for Simulation of Color Vision Deficiency", applied in linear RGB.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace UI::ColorDifference
{

/// A CIELAB colour: L* 0 (black) to 100 (white); a* green-red; b* blue-yellow.
struct Lab
{
    double l = 0.0;
    double a = 0.0;
    double b = 0.0;
};

/// One sRGB channel (0..1) to linear light. Out-of-range and non-finite input is clamped first.
[[nodiscard]] inline double toLinear(double channel) noexcept
{
    const double c = std::isfinite(channel) ? std::clamp(channel, 0.0, 1.0) : 0.0;
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

/// One linear-light channel back to sRGB (0..1), clamped to the gamut.
[[nodiscard]] inline double toSrgb(double linear) noexcept
{
    const double c = std::isfinite(linear) ? std::clamp(linear, 0.0, 1.0) : 0.0;
    return (c <= 0.0031308) ? (c * 12.92) : ((1.055 * std::pow(c, 1.0 / 2.4)) - 0.055);
}

/// An opaque sRGB colour in CIELAB (D65). Alpha is ignored.
[[nodiscard]] inline Lab toLab(const ImVec4& color) noexcept
{
    const double r = toLinear(static_cast<double>(color.x));
    const double g = toLinear(static_cast<double>(color.y));
    const double b = toLinear(static_cast<double>(color.z));

    // sRGB (D65) to CIE XYZ, normalised by the D65 white.
    constexpr double WHITE_X = 0.95047;
    constexpr double WHITE_Z = 1.08883;
    const double x = ((0.4124564 * r) + (0.3575761 * g) + (0.1804375 * b)) / WHITE_X;
    const double y = (0.2126729 * r) + (0.7151522 * g) + (0.0721750 * b);
    const double z = ((0.0193339 * r) + (0.1191920 * g) + (0.9503041 * b)) / WHITE_Z;

    const auto f = [](double t)
    {
        constexpr double EPSILON = 216.0 / 24389.0;
        constexpr double KAPPA = 24389.0 / 27.0;
        return (t > EPSILON) ? std::cbrt(t) : (((KAPPA * t) + 16.0) / 116.0);
    };
    const double fx = f(x);
    const double fy = f(y);
    const double fz = f(z);
    return {.l = (116.0 * fy) - 16.0, .a = 500.0 * (fx - fy), .b = 200.0 * (fy - fz)};
}

/// CIEDE2000 colour difference between two CIELAB colours. About 1 is a just-noticeable difference;
/// series meant to be told apart at a glance want 10 or more.
[[nodiscard]] inline double deltaE2000(const Lab& lab1, const Lab& lab2) noexcept
{
    constexpr double PI = std::numbers::pi;
    constexpr double DEG = PI / 180.0;
    constexpr double POW25_7 = 6103515625.0; // 25^7
    const auto pow7 = [](double v)
    {
        const double v2 = v * v;
        return v2 * v2 * v2 * v;
    };
    const auto hueDegrees = [](double b, double a)
    {
        if (a == 0.0 && b == 0.0)
        {
            return 0.0;
        }
        const double h = std::atan2(b, a) * (180.0 / PI);
        return (h < 0.0) ? (h + 360.0) : h;
    };

    const double c1 = std::hypot(lab1.a, lab1.b);
    const double c2 = std::hypot(lab2.a, lab2.b);
    const double cMean7 = pow7((c1 + c2) * 0.5);
    const double g = 0.5 * (1.0 - std::sqrt(cMean7 / (cMean7 + POW25_7)));

    const double a1p = (1.0 + g) * lab1.a;
    const double a2p = (1.0 + g) * lab2.a;
    const double c1p = std::hypot(a1p, lab1.b);
    const double c2p = std::hypot(a2p, lab2.b);
    const double h1p = hueDegrees(lab1.b, a1p);
    const double h2p = hueDegrees(lab2.b, a2p);

    const double deltaLp = lab2.l - lab1.l;
    const double deltaCp = c2p - c1p;
    const double chromaProduct = c1p * c2p;

    double deltahp = 0.0;
    if (chromaProduct != 0.0)
    {
        deltahp = h2p - h1p;
        if (deltahp > 180.0)
        {
            deltahp -= 360.0;
        }
        else if (deltahp < -180.0)
        {
            deltahp += 360.0;
        }
    }
    const double deltaHp = 2.0 * std::sqrt(chromaProduct) * std::sin(deltahp * 0.5 * DEG);

    const double lMeanP = (lab1.l + lab2.l) * 0.5;
    const double cMeanP = (c1p + c2p) * 0.5;
    double hMeanP = h1p + h2p;
    if (chromaProduct != 0.0)
    {
        if (std::abs(h1p - h2p) <= 180.0)
        {
            hMeanP *= 0.5;
        }
        else
        {
            hMeanP = (hMeanP < 360.0) ? ((hMeanP + 360.0) * 0.5) : ((hMeanP - 360.0) * 0.5);
        }
    }

    const double t = 1.0 - (0.17 * std::cos((hMeanP - 30.0) * DEG)) + (0.24 * std::cos(2.0 * hMeanP * DEG)) +
                     (0.32 * std::cos(((3.0 * hMeanP) + 6.0) * DEG)) - (0.20 * std::cos(((4.0 * hMeanP) - 63.0) * DEG));
    const double hueOffset = (hMeanP - 275.0) / 25.0;
    const double deltaTheta = 30.0 * std::exp(-(hueOffset * hueOffset));
    const double cMeanP7 = pow7(cMeanP);
    const double rc = 2.0 * std::sqrt(cMeanP7 / (cMeanP7 + POW25_7));
    const double lOffset2 = (lMeanP - 50.0) * (lMeanP - 50.0);
    const double sl = 1.0 + ((0.015 * lOffset2) / std::sqrt(20.0 + lOffset2));
    const double sc = 1.0 + (0.045 * cMeanP);
    const double sh = 1.0 + (0.015 * cMeanP * t);
    const double rt = -std::sin(2.0 * deltaTheta * DEG) * rc;

    const double lTerm = deltaLp / sl;
    const double cTerm = deltaCp / sc;
    const double hTerm = deltaHp / sh;
    return std::sqrt((lTerm * lTerm) + (cTerm * cTerm) + (hTerm * hTerm) + (rt * cTerm * hTerm));
}

/// CIEDE2000 difference between two opaque sRGB colours.
[[nodiscard]] inline double deltaE2000(const ImVec4& color1, const ImVec4& color2) noexcept
{
    return deltaE2000(toLab(color1), toLab(color2));
}

/// The red/green dichromacies, which together affect about 2% of men (and anomalous trichromacy of
/// the same cones a further 6%).
enum class Deficiency
{
    Protanopia,   ///< No long-wavelength (red) cones
    Deuteranopia, ///< No medium-wavelength (green) cones
};

/// How `color` appears to a dichromat (Machado 2009, severity 1.0). Alpha passes through.
[[nodiscard]] inline ImVec4 simulate(const ImVec4& color, Deficiency deficiency) noexcept
{
    using Matrix = std::array<std::array<double, 3>, 3>;
    static constexpr Matrix PROTANOPIA{{
        {0.152286, 1.052583, -0.204868},
        {0.114503, 0.786281, 0.099216},
        {-0.003882, -0.048116, 1.051998},
    }};
    static constexpr Matrix DEUTERANOPIA{{
        {0.367322, 0.860646, -0.227968},
        {0.280085, 0.672501, 0.047413},
        {-0.011820, 0.042940, 0.968881},
    }};
    const Matrix& m = (deficiency == Deficiency::Protanopia) ? PROTANOPIA : DEUTERANOPIA;

    const std::array<double, 3> lin{
        toLinear(static_cast<double>(color.x)), toLinear(static_cast<double>(color.y)), toLinear(static_cast<double>(color.z))};
    std::array<float, 3> out{};
    for (std::size_t row = 0; row < 3; ++row)
    {
        const double v = (m[row][0] * lin[0]) + (m[row][1] * lin[1]) + (m[row][2] * lin[2]);
        out[row] = static_cast<float>(toSrgb(v));
    }
    return {out[0], out[1], out[2], color.w};
}

/// CIEDE2000 difference between two colours as a dichromat sees them.
[[nodiscard]] inline double deltaE2000As(const ImVec4& color1, const ImVec4& color2, Deficiency deficiency) noexcept
{
    return deltaE2000(simulate(color1, deficiency), simulate(color2, deficiency));
}

} // namespace UI::ColorDifference
