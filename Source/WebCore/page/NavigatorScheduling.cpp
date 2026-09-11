/*
 * Copyright (C) 2026 the Revenant WebKit port.
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
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * ARE DISCLAIMED.
 */

#include "config.h"
#include "NavigatorScheduling.h"

#include "Navigator.h"
#include "Scheduling.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(NavigatorScheduling);

NavigatorScheduling::NavigatorScheduling(Navigator&)
    : m_scheduling(Scheduling::create())
{
}

NavigatorScheduling::~NavigatorScheduling() = default;

NavigatorScheduling* NavigatorScheduling::from(Navigator& navigator)
{
    auto* supplement = static_cast<NavigatorScheduling*>(Supplement<Navigator>::from(&navigator, supplementName()));
    if (!supplement) {
        auto created = makeUnique<NavigatorScheduling>(navigator);
        supplement = created.get();
        provideTo(&navigator, supplementName(), WTF::move(created));
    }
    return supplement;
}

Scheduling& NavigatorScheduling::scheduling(Navigator& navigator)
{
    return from(navigator)->scheduling();
}

} // namespace WebCore
