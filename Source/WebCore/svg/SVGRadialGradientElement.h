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

struct RadialGradientAttributes;

class SVGRadialGradientElement final : public SVGGradientElement {
    WTF_MAKE_TZONE_ALLOCATED(SVGRadialGradientElement);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(SVGRadialGradientElement);
public:
    static Ref<SVGRadialGradientElement> create(const QualifiedName&, Document&);

    bool collectGradientAttributes(RadialGradientAttributes&);

    const SVGLengthValue& cx() const LIFETIME_BOUND { return m_cx->currentValue(); }
    const SVGLengthValue& cy() const LIFETIME_BOUND { return m_cy->currentValue(); }
    const SVGLengthValue& r() const LIFETIME_BOUND { return m_r->currentValue(); }
    const SVGLengthValue& fx() const LIFETIME_BOUND { return m_fx->currentValue(); }
    const SVGLengthValue& fy() const LIFETIME_BOUND { return m_fy->currentValue(); }
    const SVGLengthValue& fr() const LIFETIME_BOUND { return m_fr->currentValue(); }

    SVGAnimatedLength& cxAnimated() { return m_cx; }
    SVGAnimatedLength& cyAnimated() { return m_cy; }
    SVGAnimatedLength& rAnimated() { return m_r; }
    SVGAnimatedLength& fxAnimated() { return m_fx; }
    SVGAnimatedLength& fyAnimated() { return m_fy; }
    SVGAnimatedLength& frAnimated() { return m_fr; }

    using PropertyRegistry = SVGPropertyOwnerRegistry<SVGRadialGradientElement, SVGGradientElement>;

private:
    SVGRadialGradientElement(const QualifiedName&, Document&);

    void attributeChanged(const QualifiedName&, const AtomString& oldValue, const AtomString& newValue, AttributeModificationReason) override;
    void svgAttributeChanged(const QualifiedName&) override;

    RenderPtr<RenderElement> createElementRenderer(Style::ComputedStyle&&, const RenderTreePosition&) override;

    bool selfHasRelativeLengths() const override;
    bool supportsFocus() const final { return false; }

    static const SVGLengthValue& halfLength(SVGLengthMode mode)
    {
        if (mode == SVGLengthMode::Height) {
            static NeverDestroyed<SVGLengthValue> height { SVGLengthMode::Height, "50%"_s };
            return height.get();
        }
        if (mode == SVGLengthMode::Other) {
            static NeverDestroyed<SVGLengthValue> other { SVGLengthMode::Other, "50%"_s };
            return other.get();
        }
        static NeverDestroyed<SVGLengthValue> width { SVGLengthMode::Width, "50%"_s };
        return width.get();
    }

    static const SVGLengthValue& zeroFocalRadius()
    {
        static NeverDestroyed<SVGLengthValue> other { SVGLengthMode::Other, "0%"_s };
        return other.get();
    }

    const Ref<SVGAnimatedLength> m_cx { SVGAnimatedLength::create(this, halfLength(SVGLengthMode::Width)) };
    const Ref<SVGAnimatedLength> m_cy { SVGAnimatedLength::create(this, halfLength(SVGLengthMode::Height)) };
    const Ref<SVGAnimatedLength> m_r { SVGAnimatedLength::create(this, halfLength(SVGLengthMode::Other)) };
    const Ref<SVGAnimatedLength> m_fx { SVGAnimatedLength::create(this, halfLength(SVGLengthMode::Width)) };
    const Ref<SVGAnimatedLength> m_fy { SVGAnimatedLength::create(this, halfLength(SVGLengthMode::Height)) };
    const Ref<SVGAnimatedLength> m_fr { SVGAnimatedLength::create(this, zeroFocalRadius()) };
};

} // namespace WebCore
