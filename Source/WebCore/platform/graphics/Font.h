/*
 * Copyright (C) 2006-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2007-2008 Torch Mobile, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include <WebCore/FloatRect.h>
#include <WebCore/FontBase.h>
#include <WebCore/GlyphMetricsMap.h>
#include <WebCore/TrustedFonts.h>
#include <wtf/Platform.h>
#include <wtf/WeakPtr.h>

#if PLATFORM(WIN)
#include <usp10.h>
#endif

namespace WTF {
class TextStream;
}

namespace WebCore {

class FontCache;
class FontDescription;
class GlyphPage;

struct GlyphData;


enum class FontVariant : uint8_t { Auto, Normal, SmallCaps, EmphasisMark, BrokenIdeograph };
enum class PitchType : uint8_t { Unknown, Fixed, Variable };
enum class IsForPlatformFont : bool { No, Yes };

#if USE(CORE_TEXT)
using IPCFontData = Variant<WebCore::InstalledFont, WebCore::CustomFontCreationData>;
#endif

DECLARE_ALLOCATOR_WITH_HEAP_IDENTIFIER(Font);
class Font : public FontBase, public RefCounted<Font>, public CanMakeSingleThreadWeakPtr<Font> {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED_WITH_HEAP_IDENTIFIER(Font, Font);
public:
    WEBCORE_EXPORT static Ref<Font> create(const FontPlatformData&, Origin = Origin::Local, IsInterstitial = IsInterstitial::No, Visibility = Visibility::Visible, IsOrientationFallback = IsOrientationFallback::No, std::optional<RenderingResourceIdentifier> = std::nullopt);
    WEBCORE_EXPORT static Ref<Font> create(Ref<SharedBuffer>&& fontFaceData, Font::Origin, float fontSize, bool syntheticBold, bool syntheticItalic, DownloadableBinaryFontTrustedTypes);
    WEBCORE_EXPORT static Ref<Font> create(FontInternalAttributes&&, FontPlatformData&&);

    WEBCORE_EXPORT ~Font();

    static Ref<Font> createSystemFallbackFontPlaceholder() { return adoptRef(*new Font(IsSystemFallbackFontPlaceholder::Yes)); }
    bool isSystemFontFallbackPlaceholder() const { return m_isSystemFontFallbackPlaceholder; }

    const Font* smallCapsFont(const FontDescription&) const;
    const Font& noSynthesizableFeaturesFont() const;
    const Font* emphasisMarkFont(const FontDescription&) const;
    const Font& brokenIdeographFont() const;
    const RefPtr<Font> halfWidthFont() const;

    bool isProbablyOnlyUsedToRenderIcons() const;

    const Font* variantFont(const FontDescription& description, FontVariant variant) const
    {
        switch (variant) {
        case FontVariant::SmallCaps:
            return smallCapsFont(description);
        case FontVariant::EmphasisMark:
            return emphasisMarkFont(description);
        case FontVariant::BrokenIdeograph:
            return &brokenIdeographFont();
        case FontVariant::Auto:
        case FontVariant::Normal:
            break;
        }
        ASSERT_NOT_REACHED();
        return const_cast<Font*>(this);
    }

    bool variantCapsSupportedForSynthesis(FontVariantCaps) const;

    const Font& verticalRightOrientationFont() const;
    const Font& uprightOrientationFont() const;
    const Font& invisibleFont() const;

    FloatRect boundsForGlyph(Glyph) const;
#if USE(CORE_TEXT) || USE(SKIA)
    static constexpr size_t inlineGlyphRunCapacity = 256;
    Vector<FloatRect, inlineGlyphRunCapacity> boundsForGlyphs(std::span<const Glyph>) const;
#endif

    float widthForGlyph(Glyph, SyntheticBoldInclusion = SyntheticBoldInclusion::Incorporate) const;

    Path pathForGlyph(Glyph) const;

    Glyph spaceGlyph() const { return m_spaceGlyph; }
    Glyph zeroWidthSpaceGlyph() const { return m_zeroWidthSpaceGlyph; }
    bool isZeroWidthSpaceGlyph(Glyph glyph) const { return glyph == m_zeroWidthSpaceGlyph && glyph; }

    GlyphData glyphDataForCharacter(char32_t) const;
    Glyph glyphForCharacter(char32_t) const;
    bool supportsCodePoint(char32_t) const;
    bool platformSupportsCodePoint(char32_t, std::optional<char32_t> variation = std::nullopt) const;

    RefPtr<Font> systemFallbackFontForCharacterCluster(StringView, const FontDescription&, ResolvedEmojiPolicy, IsForPlatformFont) const;

    const GlyphPage* glyphPage(unsigned pageNumber) const;

#if USE(CORE_TEXT) && defined(WEBKIT_IOS6) && !ENABLE(OPENTYPE_VERTICAL)
    void prewarmGlyphAdvances(const GlyphPage&) const;
#endif

    void determinePitch();
    PitchType pitch() const { return m_treatAsFixedPitch ? PitchType::Fixed : PitchType::Variable; }
    bool canTakeFixedPitchFastContentMeasuring() const { return m_canTakeFixedPitchFastContentMeasuring; }

#if !LOG_DISABLED
    String description() const;
#endif

    bool canRenderCombiningCharacterSequence(StringView) const;
    GlyphBufferAdvance applyTransforms(GlyphBuffer&, unsigned beginningGlyphIndex, unsigned beginningStringIndex, bool enableKerning, bool requiresShaping, const AtomString& locale, StringView text, TextDirection) const;

#if USE(CORE_TEXT)
    WEBCORE_EXPORT static std::optional<Ref<Font>> fromIPCData(IPCFontData&&);
    WEBCORE_EXPORT IPCFontData toSerializableFont() const;
    WEBCORE_EXPORT std::optional<InstalledFont> toSerializableInstalledFont() const;

    // The Core Text string attributes for shaping with this font. They depend only on the
    // font, the kerning switch and the locale, so the dictionary is built once per font
    // rather than once per complex text run.
    RetainPtr<CFDictionaryRef> cfStringAttributes(bool enableKerning, const AtomString& locale) const;
#endif
#if PLATFORM(WIN)
    SCRIPT_CACHE* scriptCache() const LIFETIME_BOUND { return &m_scriptCache; }
#endif

    ColorGlyphType colorGlyphType(Glyph) const;

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    WEBCORE_EXPORT Font(const FontPlatformData&, Origin, IsInterstitial, Visibility, IsOrientationFallback, std::optional<RenderingResourceIdentifier>);
    Font(IsSystemFallbackFontPlaceholder);

    void platformGlyphInit();
    void platformCharWidthInit();
    void platformCharHeightInit();

    void initCharWidths();
    void initZeroWidth(Glyph);

    void NODELETE platformDestroy();

    RefPtr<Font> createFontWithoutSynthesizableFeatures() const;
    RefPtr<Font> createScaledFont(const FontDescription&, float scaleFactor) const;
    RefPtr<Font> platformCreateScaledFont(const FontDescription&, float scaleFactor) const;
    RefPtr<Font> createHalfWidthFont() const;
    RefPtr<Font> platformCreateHalfWidthFont() const;

    struct DerivedFonts;
    DerivedFonts& ensureDerivedFontData() const;

    FloatRect platformBoundsForGlyph(Glyph) const;
#if USE(CORE_TEXT) || USE(SKIA)
    Vector<FloatRect, inlineGlyphRunCapacity> platformBoundsForGlyphs(const Vector<Glyph, inlineGlyphRunCapacity>&) const;
#endif
    float platformWidthForGlyph(Glyph) const;
    Path platformPathForGlyph(Glyph) const;

#if PLATFORM(COCOA)
    class ComplexColorFormatGlyphs {
    public:
        static ComplexColorFormatGlyphs createWithNoRelevantTables();
        static ComplexColorFormatGlyphs createWithRelevantTablesAndGlyphCount(unsigned glyphCount);

        bool hasValueFor(Glyph) const;
        bool get(Glyph) const;
        void set(Glyph, bool value);

        bool hasRelevantTables() const { return m_hasRelevantTables; }

    private:
        static constexpr size_t bitForInitialized(Glyph glyphID) { return static_cast<size_t>(glyphID) * 2; }
        static constexpr size_t bitForValue(Glyph glyphID) { return static_cast<size_t>(glyphID) * 2 + 1; }
        static constexpr size_t bitsRequiredForGlyphCount(unsigned glyphCount) { return glyphCount * 2; }

        ComplexColorFormatGlyphs(bool hasRelevantTables, unsigned glyphCount)
            : m_hasRelevantTables(hasRelevantTables)
            , m_bits(bitsRequiredForGlyphCount(glyphCount))
        { }

        bool m_hasRelevantTables;
        BitVector m_bits; // pairs of (initialized, value) bits
    };

    const PAL::OTSVGTable& otSVGTable() const;
    bool glyphHasComplexColorFormat(Glyph) const;
    bool hasComplexColorFormatTables() const;
    ComplexColorFormatGlyphs& glyphsWithComplexColorFormat() const;
#endif

    FontMetrics m_fontMetrics;
    float m_maxCharWidth { -1 };
    float m_avgCharWidth { -1 };

    const FontPlatformData m_platformData;

    // Code points 0-255 live in the first few pages and are hit once per character measured
    // or painted. Their page pointers are kept in a direct-mapped side table so the common
    // case is an array index instead of a hash probe. Entries in m_glyphPages are never
    // removed, so a raw pointer stays valid for the lifetime of the Font.
    static constexpr unsigned directMappedGlyphPageCount = 16;
    mutable std::array<const GlyphPage*, directMappedGlyphPageCount> m_directMappedGlyphPages { };
    mutable uint16_t m_directMappedGlyphPagesFilled { 0 };

    mutable HashMap<unsigned, RefPtr<GlyphPage>, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> m_glyphPages;
    mutable GlyphMetricsMap<float> m_glyphToWidthMap;
    mutable std::unique_ptr<GlyphMetricsMap<FloatRect>> m_glyphToBoundsMap;
    // FIXME: Find a more efficient way to represent std::optional<Path>.
    mutable std::unique_ptr<GlyphMetricsMap<std::optional<Path>>> m_glyphPathMap;
    mutable BitVector m_codePointSupport;

    struct DerivedFonts {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(DerivedFonts);
    public:

        RefPtr<Font> smallCapsFont;
        RefPtr<Font> noSynthesizableFeaturesFont;
        RefPtr<Font> emphasisMarkFont;
        RefPtr<Font> brokenIdeographFont;
        RefPtr<Font> verticalRightOrientationFont;
        RefPtr<Font> uprightOrientationFont;
        RefPtr<Font> invisibleFont;
        RefPtr<Font> halfWidthFont;
    };

    mutable std::unique_ptr<DerivedFonts> m_derivedFontData;

    struct NoEmojiGlyphs { };
#if USE(SKIA) || defined(WEBKIT_IOS6)
    struct AllEmojiGlyphs { };
#endif
    struct SomeEmojiGlyphs {
        BitVector colorGlyphs;
    };
#if USE(SKIA) || defined(WEBKIT_IOS6)
    using EmojiType = Variant<NoEmojiGlyphs, AllEmojiGlyphs, SomeEmojiGlyphs>;
#else
    using EmojiType = Variant<NoEmojiGlyphs, SomeEmojiGlyphs>;
#endif
    EmojiType m_emojiType { NoEmojiGlyphs { } };

#if PLATFORM(COCOA)
    mutable std::optional<PAL::OTSVGTable> m_otSVGTable;
    mutable std::optional<ComplexColorFormatGlyphs> m_glyphsWithComplexColorFormat; // SVG and sbix

    enum class SupportsFeature : uint8_t {
        No,
        Yes,
        Unknown
    };
    mutable SupportsFeature m_supportsSmallCaps { SupportsFeature::Unknown };
    mutable SupportsFeature m_supportsAllSmallCaps { SupportsFeature::Unknown };
    mutable SupportsFeature m_supportsPetiteCaps { SupportsFeature::Unknown };
    mutable SupportsFeature m_supportsAllPetiteCaps { SupportsFeature::Unknown };
    mutable SupportsFeature m_supportsOpenTypeAlternateHalfWidths { SupportsFeature::Unknown };
#endif

#if USE(CORE_TEXT)
    mutable RetainPtr<CFDictionaryRef> m_cachedStringAttributes;
    mutable AtomString m_cachedStringAttributesLocale;
    mutable bool m_cachedStringAttributesEnableKerning { false };
#endif

#if PLATFORM(WIN)
    mutable SCRIPT_CACHE m_scriptCache { 0 };
#endif

    Glyph m_spaceGlyph { 0 };
    Glyph m_zeroWidthSpaceGlyph { 0 };

    unsigned m_isSystemFontFallbackPlaceholder : 1 { false };
    unsigned m_treatAsFixedPitch : 1 { false };
    unsigned m_canTakeFixedPitchFastContentMeasuring : 1 { false };
    unsigned m_isBrokenIdeographFallback : 1 { false };

    // Adding any non-derived information to Font needs a parallel change in WebCoreArgumentCoders.cpp.
};

#if PLATFORM(IOS_FAMILY)
bool fontFamilyShouldNotBeUsedForArabic(CFStringRef);
#endif

#if !LOG_DISABLED
WEBCORE_EXPORT TextStream& operator<<(TextStream&, const Font&);
TextStream& operator<<(TextStream&, const GlyphBuffer&);
#endif

} // namespace WebCore
