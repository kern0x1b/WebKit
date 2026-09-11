/*
 * Copyright (C) 2004, 2005, 2006, 2008 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) 2004, 2005, 2006 Rob Buis <buis@kde.org>
 * Copyright (C) 2018-2024 Apple Inc. All rights reserved.
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
 */

#pragma once

#include "SVGGradientElement.h"
#include "SVGNames.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

struct LinearGradientAttributes;

class SVGLinearGradientElement final : public SVGGradientElement {
    WTF_MAKE_TZONE_ALLOCATED(SVGLinearGradientElement);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(SVGLinearGradientElement);
public:
    static Ref<SVGLinearGradientElement> create(const QualifiedName&, Document&);

    bool collectGradientAttributes(LinearGradientAttributes&);

    const SVGLengthValue& x1() const LIFETIME_BOUND { return m_x1->currentValue(); }
    const SVGLengthValue& y1() const LIFETIME_BOUND { return m_y1->currentValue(); }
    const SVGLengthValue& x2() const LIFETIME_BOUND { return m_x2->currentValue(); }
    const SVGLengthValue& y2() const LIFETIME_BOUND { return m_y2->currentValue(); }

    SVGAnimatedLength& x1Animated() { return m_x1; }
    SVGAnimatedLength& y1Animated() { return m_y1; }
    SVGAnimatedLength& x2Animated() { return m_x2; }
    SVGAnimatedLength& y2Animated() { return m_y2; }

private:
    SVGLinearGradientElement(const QualifiedName&, Document&);

    using PropertyRegistry = SVGPropertyOwnerRegistry<SVGLinearGradientElement, SVGGradientElement>;

    void attributeChanged(const QualifiedName&, const AtomString& oldValue, const AtomString& newValue, AttributeModificationReason) override;
    void svgAttributeChanged(const QualifiedName&) override;

    RenderPtr<RenderElement> createElementRenderer(Style::ComputedStyle&&, const RenderTreePosition&) override;

    bool selfHasRelativeLengths() const override;
    bool supportsFocus() const final { return false; }

    static const SVGLengthValue& startLength(SVGLengthMode mode)
    {
        if (mode == SVGLengthMode::Height) {
            static NeverDestroyed<SVGLengthValue> height { SVGLengthMode::Height, "0%"_s };
            return height.get();
        }
        static NeverDestroyed<SVGLengthValue> width { SVGLengthMode::Width, "0%"_s };
        return width.get();
    }

    static const SVGLengthValue& endLength()
    {
        static NeverDestroyed<SVGLengthValue> width { SVGLengthMode::Width, "100%"_s };
        return width.get();
    }

    const Ref<SVGAnimatedLength> m_x1 { SVGAnimatedLength::create(this, startLength(SVGLengthMode::Width)) };
    const Ref<SVGAnimatedLength> m_y1 { SVGAnimatedLength::create(this, startLength(SVGLengthMode::Height)) };
    const Ref<SVGAnimatedLength> m_x2 { SVGAnimatedLength::create(this, endLength()) };
    const Ref<SVGAnimatedLength> m_y2 { SVGAnimatedLength::create(this, startLength(SVGLengthMode::Height)) };
};

} // namespace WebCore
