/*
 * Copyright (C) 2026 Vita port contributors.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/WTFConfig.h>
#include <wtf/threads/Signals.h>

#if ENABLE(SIGNAL_BASED_VM_TRAPS)
#error Vita requires an exception backend before enabling signal-based VM traps.
#endif

namespace WTF {

void SignalHandlers::initialize()
{
    auto& handlers = g_wtfConfig.signalHandlers;
    RELEASE_ASSERT(handlers.initState == InitState::Uninitialized);
    handlers.initState = InitState::Initializing;
}

void SignalHandlers::finalize()
{
    Config::AssertNotFrozenScope assertScope;
    auto& handlers = g_wtfConfig.signalHandlers;
    RELEASE_ASSERT(handlers.initState == InitState::Initializing);
    for (auto count : handlers.numberOfHandlers)
        RELEASE_ASSERT(!count);
    handlers.initState = InitState::Finalized;
}

void SignalHandlers::add(Signal, SignalHandler&&)
{
    RELEASE_ASSERT_WITH_MESSAGE(false, "Vita exception handlers are not implemented.");
}

void addSignalHandler(Signal signal, SignalHandler&& handler)
{
    g_wtfConfig.signalHandlers.add(signal, WTF::move(handler));
}

void activateSignalHandlersFor(Signal)
{
    RELEASE_ASSERT_WITH_MESSAGE(false, "Vita exception handlers are not implemented.");
}

} // namespace WTF
