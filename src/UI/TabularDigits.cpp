#include "TabularDigits.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace UI
{

namespace
{

/// FT_Library and FT_Face, released in reverse order however the measuring ends.
class FreeTypeFace
{
  public:
    explicit FreeTypeFace(std::span<const std::byte> fontData)
    {
        if (fontData.empty() || fontData.size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max()))
        {
            return;
        }
        if (FT_Init_FreeType(&m_Library) != 0)
        {
            m_Library = nullptr;
            return;
        }
        // FreeType reads the bytes in place; the span outlives this object.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - FreeType takes the file as FT_Byte*
        const auto* bytes = reinterpret_cast<const FT_Byte*>(fontData.data());
        if (FT_New_Memory_Face(m_Library, bytes, static_cast<FT_Long>(fontData.size()), 0, &m_Face) != 0)
        {
            m_Face = nullptr;
        }
    }

    ~FreeTypeFace()
    {
        if (m_Face != nullptr)
        {
            FT_Done_Face(m_Face);
        }
        if (m_Library != nullptr)
        {
            FT_Done_FreeType(m_Library);
        }
    }

    FreeTypeFace(const FreeTypeFace&) = delete;
    FreeTypeFace& operator=(const FreeTypeFace&) = delete;
    FreeTypeFace(FreeTypeFace&&) = delete;
    FreeTypeFace& operator=(FreeTypeFace&&) = delete;

    [[nodiscard]] FT_Face face() const noexcept
    {
        return m_Face;
    }

  private:
    FT_Library m_Library = nullptr;
    FT_Face m_Face = nullptr;
};

} // namespace

std::optional<float> widestDigitAdvanceRatio(std::span<const std::byte> fontData)
{
    const FreeTypeFace font(fontData);
    FT_FaceRec* const face = font.face();
    if (face == nullptr)
    {
        return std::nullopt;
    }
    // The height FT_SIZE_REQUEST_TYPE_REAL_DIM scales to the requested size (see the header).
    const auto height = static_cast<double>(face->ascender) - static_cast<double>(face->descender);
    if (!(height > 0.0))
    {
        return std::nullopt;
    }

    FT_Fixed widest = 0;
    for (char digit = '0'; digit <= '9'; ++digit)
    {
        const FT_UInt glyph = FT_Get_Char_Index(face, static_cast<FT_ULong>(digit));
        FT_Fixed advance = 0;
        // Unscaled: in font units, the same units as the ascender and descender.
        if (glyph != 0 && FT_Get_Advance(face, glyph, FT_LOAD_NO_SCALE, &advance) == 0)
        {
            widest = std::max(widest, advance);
        }
    }
    if (widest <= 0)
    {
        return std::nullopt;
    }
    return static_cast<float>(static_cast<double>(widest) / height);
}

float tabularDigitAdvancePx(float widestDigitRatio, float fontSizePx) noexcept
{
    if (!std::isfinite(widestDigitRatio) || widestDigitRatio <= 0.0F || !std::isfinite(fontSizePx) || fontSizePx <= 0.0F)
    {
        return 0.0F;
    }
    return std::ceil(widestDigitRatio * fontSizePx);
}

} // namespace UI
