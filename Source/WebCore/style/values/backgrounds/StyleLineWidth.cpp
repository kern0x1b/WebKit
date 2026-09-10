/*
 * Copyright (C) 2025 Samuel Weinig <sam@webkit.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "StyleLineWidth.h"

#include "CSSKeywordValue.h"
#include "Document.h"
#include "Settings.h"
#include "StyleBuilderChecking.h"
#include "StyleComputedStyle+GettersInlines.h"
#include "StyleInterpolationClient.h"
#include "StyleInterpolationContext.h"
#include "StylePrimitiveNumericTypes+Blending.h"
#include "StylePrimitiveNumericTypes+CSSValueConversion.h"
#include "StylePrimitiveNumericTypes+Evaluation.h"
#include "StylePrimitiveNumericTypes+Serialization.h"

namespace WebCore {
namespace Style {

// MARK: - Conversion

static float snapLengthAsBorderWidth(float length, float deviceScaleFactor)
{
    // A border of no width stays a border of no width whatever the device
    // scale is, and the overwhelming majority of boxes on a page have no
    // border at all. Taking the arithmetic below for those means a call to
    // floorf per edge per box per layout - on armv7 that is a real libm call,
    // there being no rounding instruction on this VFP unit - and it measured
    // as the single hottest thing in a layout of an ordinary page.
    if (!length)
        return 0;

    // https://drafts.csswg.org/css-values-4/#snap-a-length-as-a-border-width

    // 1. Assert: `length` is non-negative.
    // NOTE: Not asserted, but checked in step 3.

    // 2. If `length` is an integer number of device pixels, do nothing.
    // NOTE: Handled by step 4 without explicitly checking here.

    // 3. If `length` is greater than zero, but less than 1 device pixel, round `length` up to 1 device pixel.
    if (auto singleDevicePixelLength = 1.0f / deviceScaleFactor; length > 0.0f && length < singleDevicePixelLength)
        return singleDevicePixelLength;

    // 4. If `length` is greater than 1 device pixel, round it down to the nearest integer number of device pixels.
    return std::floor(length * deviceScaleFactor) / deviceScaleFactor;
}

LineWidth::Length LineWidth::snapLengthAsBorderWidth(float length, float deviceScaleFactor)
{
    return LineWidth::Length { Style::snapLengthAsBorderWidth(length, deviceScaleFactor) };
}

LineWidth::Length LineWidth::snapLengthAsBorderWidth(LineWidth::Length length, float deviceScaleFactor)
{
    return LineWidth::Length { Style::snapLengthAsBorderWidth(length.unresolvedValue(), deviceScaleFactor) };
}

auto CSSValueConversion<LineWidth>::operator()(BuilderState& state, const CSSValue& value) -> LineWidth
{
    if (!state.document().settings().evaluationTimeZoomEnabled()) {
        if (RefPtr keywordValue = dynamicDowncast<CSSKeywordValue>(value)) {
            switch (keywordValue->valueID()) {
            case CSSValueThin:
                return LineWidth::snapLengthAsBorderWidth(1.0f * state.style().usedZoom(), state.style().deviceScaleFactor());
            case CSSValueMedium:
                return LineWidth::snapLengthAsBorderWidth(3.0f * state.style().usedZoom(), state.style().deviceScaleFactor());
            case CSSValueThick:
                return LineWidth::snapLengthAsBorderWidth(5.0f * state.style().usedZoom(), state.style().deviceScaleFactor());
            default:
                state.setCurrentPropertyInvalidAtComputedValueTime();
                return LineWidth::Length { 3.0f };
            }
        }

        return LineWidth::snapLengthAsBorderWidth(toStyleFromCSSValue<LineWidth::Length>(state, value), state.style().deviceScaleFactor());
    }

    if (RefPtr keywordValue = dynamicDowncast<CSSKeywordValue>(value)) {
        switch (keywordValue->valueID()) {
        case CSSValueThin:
            return LineWidth { 1.0f };
        case CSSValueMedium:
            return LineWidth { 3.0f };
        case CSSValueThick:
            return LineWidth { 5.0f };
        default:
            state.setCurrentPropertyInvalidAtComputedValueTime();
            return LineWidth { 3.0f };
        }
    }

    return LineWidth::snapLengthAsBorderWidth(toStyleFromCSSValue<LineWidth::Length>(state, value), state.style().deviceScaleFactor());
}

// MARK: - Blending

auto Blending<LineWidth>::blend(const LineWidth& a, const LineWidth& b, const Style::ComputedStyle& aStyle, const Style::ComputedStyle& bStyle, const Interpolation::Context& context) -> LineWidth
{
    auto blendedValue = Style::blend(a.value, b.value, aStyle, bStyle, context);
    if (RefPtr document = context.client.document())
        return LineWidth::snapLengthAsBorderWidth(blendedValue, document->deviceScaleFactor());
    return blendedValue;
}

// MARK: - Evaluation

#if defined(WEBKIT_IOS6)

float evaluateNonZeroLineWidth(const LineWidth& value, ZoomFactor zoom, float deviceScaleFactor)
{
    return snapLengthAsBorderWidth(evaluate<float>(value.value, zoom), deviceScaleFactor);
}

LayoutUnit evaluateNonZeroLineWidthAsLayoutUnit(const LineWidth& value, ZoomFactor zoom, float deviceScaleFactor)
{
    return LayoutUnit { snapLengthAsBorderWidth(evaluate<float>(value.value, zoom), deviceScaleFactor) };
}

#else

auto Evaluation<LineWidth, float>::operator()(const LineWidth& value, ZoomFactor zoom, float deviceScaleFactor) -> float
{
    return snapLengthAsBorderWidth(evaluate<float>(value.value, zoom), deviceScaleFactor);
}

auto Evaluation<LineWidth, LayoutUnit>::operator()(const LineWidth& value, ZoomFactor zoom, float deviceScaleFactor) -> LayoutUnit
{
    return LayoutUnit { snapLengthAsBorderWidth(evaluate<float>(value.value, zoom), deviceScaleFactor) };
}

#endif

auto Evaluation<LineWidthBox, FloatBoxExtent>::operator()(const LineWidthBox& value, ZoomFactor zoom, float deviceScaleFactor) -> FloatBoxExtent
{
    return {
        evaluate<float>(value.top(), zoom, deviceScaleFactor),
        evaluate<float>(value.right(), zoom, deviceScaleFactor),
        evaluate<float>(value.bottom(), zoom, deviceScaleFactor),
        evaluate<float>(value.left(), zoom, deviceScaleFactor),
    };
}

auto Evaluation<LineWidthBox, LayoutBoxExtent>::operator()(const LineWidthBox& value, ZoomFactor zoom, float deviceScaleFactor) -> LayoutBoxExtent
{
    return {
        evaluate<LayoutUnit>(value.top(), zoom, deviceScaleFactor),
        evaluate<LayoutUnit>(value.right(), zoom, deviceScaleFactor),
        evaluate<LayoutUnit>(value.bottom(), zoom, deviceScaleFactor),
        evaluate<LayoutUnit>(value.left(), zoom, deviceScaleFactor),
    };
}

} // namespace Style
} // namespace WebCore
