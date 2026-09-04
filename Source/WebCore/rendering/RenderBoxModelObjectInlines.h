/*
 * Copyright (C) 2003-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Samuel Weinig <sam@webkit.org>
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

#include <WebCore/RenderBoxModelObject.h>
#include <WebCore/StyleComputedStyle+GettersInlines.h>
#include <WebCore/StylePrimitiveNumericTypes+EvaluationMinimum.h>

namespace WebCore {

inline LayoutUnit renderBoxModelObjectBorderWidthValue(const Style::ComputedStyle& style, const Style::LineWidth& width)
{
    if (width.isZero())
        return 0_lu;
    return Style::evaluate<LayoutUnit>(width, style.usedZoomForLength(), style.deviceScaleFactor());
}

inline LayoutUnit RenderBoxModelObject::borderAfter() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderWidthAfter()); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingAfter() const { return borderAfter() + paddingAfter(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingBefore() const { return borderBefore() + paddingBefore(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingLogicalHeight() const { return borderAndPaddingBefore() + borderAndPaddingAfter(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingLogicalWidth() const { return borderStart() + borderEnd() + paddingStart() + paddingEnd(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingLogicalLeft() const { return writingMode().isHorizontal() ? borderLeft() + paddingLeft() : borderTop() + paddingTop(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingLogicalRight() const { return writingMode().isHorizontal() ? borderRight() + paddingRight() : borderBottom() + paddingBottom(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingStart() const { return borderStart() + paddingStart(); }
inline LayoutUnit RenderBoxModelObject::borderAndPaddingEnd() const { return borderEnd() + paddingEnd(); }
inline LayoutUnit RenderBoxModelObject::borderBefore() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderWidthBefore()); }
inline LayoutUnit RenderBoxModelObject::borderBottom() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderBottomWidth()); }
inline LayoutUnit RenderBoxModelObject::borderEnd() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderWidthEnd()); }
inline LayoutUnit RenderBoxModelObject::borderLeft() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderLeftWidth()); }
inline LayoutUnit RenderBoxModelObject::borderLogicalHeight() const { return borderBefore() + borderAfter(); }
inline LayoutUnit RenderBoxModelObject::borderLogicalLeft() const { return writingMode().isHorizontal() ? borderLeft() : borderTop(); }
inline LayoutUnit RenderBoxModelObject::borderLogicalRight() const { return writingMode().isHorizontal() ? borderRight() : borderBottom(); }
inline LayoutUnit RenderBoxModelObject::borderLogicalWidth() const { return borderStart() + borderEnd(); }
inline LayoutUnit RenderBoxModelObject::borderRight() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderRightWidth()); }
inline LayoutUnit RenderBoxModelObject::borderStart() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderWidthStart()); }
inline LayoutUnit RenderBoxModelObject::borderTop() const { return renderBoxModelObjectBorderWidthValue(style(), style().usedBorderTopWidth()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingAfter() const { auto& padding = style().paddingAfter(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingBefore() const { auto& padding = style().paddingBefore(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingBottom() const { auto& padding = style().paddingBottom(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingEnd() const { auto& padding = style().paddingEnd(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingLeft() const { auto& padding = style().paddingLeft(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingRight() const { auto& padding = style().paddingRight(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingStart() const { auto& padding = style().paddingStart(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline LayoutUnit RenderBoxModelObject::computedCSSPaddingTop() const { auto& padding = style().paddingTop(); return padding.isKnownZero() ? 0_lu : resolveLengthPercentageUsingContainerLogicalWidth(padding, style().usedZoomForLength()); }
inline bool RenderBoxModelObject::hasInlineDirectionBordersOrPadding() const { return borderStart() || borderEnd() || paddingStart() || paddingEnd(); }
inline bool RenderBoxModelObject::hasInlineDirectionBordersPaddingOrMargin() const { return hasInlineDirectionBordersOrPadding() || marginStart() || marginEnd(); }
inline LayoutUnit RenderBoxModelObject::horizontalBorderAndPaddingExtent() const { return borderLeft() + borderRight() + paddingLeft() + paddingRight(); }
inline LayoutUnit RenderBoxModelObject::horizontalBorderExtent() const { return borderLeft() + borderRight(); }
inline LayoutUnit RenderBoxModelObject::marginAndBorderAndPaddingAfter() const { return marginAfter() + borderAfter() + paddingAfter(); }
inline LayoutUnit RenderBoxModelObject::marginAndBorderAndPaddingBefore() const { return marginBefore() + borderBefore() + paddingBefore(); }
inline LayoutUnit RenderBoxModelObject::marginAndBorderAndPaddingEnd() const { return marginEnd() + borderEnd() + paddingEnd(); }
inline LayoutUnit RenderBoxModelObject::marginAndBorderAndPaddingStart() const { return marginStart() + borderStart() + paddingStart(); }
inline LayoutUnit RenderBoxModelObject::paddingAfter() const { return computedCSSPaddingAfter(); }
inline LayoutUnit RenderBoxModelObject::paddingBefore() const { return computedCSSPaddingBefore(); }
inline LayoutUnit RenderBoxModelObject::paddingBottom() const { return computedCSSPaddingBottom(); }
inline LayoutUnit RenderBoxModelObject::paddingEnd() const { return computedCSSPaddingEnd(); }
inline LayoutUnit RenderBoxModelObject::paddingLeft() const { return computedCSSPaddingLeft(); }
inline LayoutUnit RenderBoxModelObject::paddingLogicalHeight() const { return paddingBefore() + paddingAfter(); }
inline LayoutUnit RenderBoxModelObject::paddingLogicalLeft() const { return writingMode().isHorizontal() ? paddingLeft() : paddingTop(); }
inline LayoutUnit RenderBoxModelObject::paddingLogicalRight() const { return writingMode().isHorizontal() ? paddingRight() : paddingBottom(); }
inline LayoutUnit RenderBoxModelObject::paddingLogicalWidth() const { return paddingStart() + paddingEnd(); }
inline LayoutUnit RenderBoxModelObject::paddingRight() const { return computedCSSPaddingRight(); }
inline LayoutUnit RenderBoxModelObject::paddingStart() const { return computedCSSPaddingStart(); }
inline LayoutUnit RenderBoxModelObject::paddingTop() const { return computedCSSPaddingTop(); }
inline LayoutUnit RenderBoxModelObject::verticalBorderAndPaddingExtent() const { return borderTop() + borderBottom() + paddingTop() + paddingBottom(); }
inline LayoutUnit RenderBoxModelObject::verticalBorderExtent() const { return borderTop() + borderBottom(); }
inline LayoutUnit RenderBoxModelObject::marginBefore() const { return marginBefore(writingMode()); }
inline LayoutUnit RenderBoxModelObject::marginAfter() const { return marginAfter(writingMode()); }
inline LayoutUnit RenderBoxModelObject::marginStart() const { return marginStart(writingMode()); }
inline LayoutUnit RenderBoxModelObject::marginEnd() const { return marginEnd(writingMode()); }
inline LayoutUnit RenderBoxModelObject::verticalMarginExtent() const { return marginTop() + marginBottom(); }
inline LayoutUnit RenderBoxModelObject::horizontalMarginExtent() const { return marginLeft() + marginRight(); }
inline LayoutUnit RenderBoxModelObject::marginLogicalHeight() const { return marginBefore() + marginAfter(); }
inline LayoutUnit RenderBoxModelObject::marginLogicalWidth() const { return marginStart() + marginEnd(); }

inline RectEdges<LayoutUnit> RenderBoxModelObject::borderWidths() const
{
    auto zoom = style().usedZoomForLength();
    auto deviceScaleFactor = style().deviceScaleFactor();
    return RectEdges<LayoutUnit>::map(style().usedBorderWidths(), [&](auto width) {
        return Style::evaluate<LayoutUnit>(width, zoom, deviceScaleFactor);
    });
}

RectEdges<LayoutUnit> RenderBoxModelObject::padding() const
{
    return {
        computedCSSPaddingTop(),
        computedCSSPaddingRight(),
        computedCSSPaddingBottom(),
        computedCSSPaddingLeft()
    };
}

inline LayoutUnit RenderBoxModelObject::resolveLengthPercentageUsingContainerLogicalWidth(const auto& value) const
{
    LayoutUnit containerWidth;
    if (value.isPercentOrCalculated()) [[unlikely]]
        containerWidth = containingBlockLogicalWidthForContent();
    return Style::evaluateMinimum<LayoutUnit>(value, containerWidth, Style::ZoomNeeded { });
}

inline LayoutUnit RenderBoxModelObject::resolveLengthPercentageUsingContainerLogicalWidth(const auto& value, const Style::ZoomFactor& zoomFactor) const
{
    LayoutUnit containerWidth;
    if (value.isPercentOrCalculated()) [[unlikely]]
        containerWidth = containingBlockLogicalWidthForContent();
    return Style::evaluateMinimum<LayoutUnit>(value, containerWidth, zoomFactor);
}

} // namespace WebCore
