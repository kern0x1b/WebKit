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

#pragma once

#include "Supplementable.h"
#include <wtf/Forward.h>
#include <wtf/TZoneMalloc.h>

namespace WebCore {

class Navigator;
class Scheduling;

class NavigatorScheduling final : public Supplement<Navigator> {
    WTF_MAKE_TZONE_ALLOCATED(NavigatorScheduling);
public:
    explicit NavigatorScheduling(Navigator&);
    ~NavigatorScheduling();

    static Scheduling& scheduling(Navigator&);
    Scheduling& scheduling() const { return m_scheduling; }

private:
    static NavigatorScheduling* from(Navigator&);
    static ASCIILiteral supplementName() { return "NavigatorScheduling"_s; }

    const Ref<Scheduling> m_scheduling;
};

} // namespace WebCore
