/*
 * Copyright (C) 2017 Yusuke Suzuki <utatane.tea@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include <wtf/CPUTime.h>

#include <psp2/kernel/threadmgr/thread.h>

namespace WTF {

std::optional<CPUTime> CPUTime::get()
{
    // The user API reports individual thread times, without process totals or
    // a user/kernel split. A snapshot of live threads would lose exited threads.
    return std::nullopt;
}

Seconds CPUTime::forCurrentThread()
{
    // Retail Vita accounts runClocks when a thread is scheduled out. Force
    // that boundary so CPU-bound JavaScript cannot evade its execution limit.
    int result = sceKernelDelayThread(1);
    RELEASE_ASSERT(!result, result);
    SceKernelThreadInfo info { };
    info.size = sizeof(info);
    result = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
    RELEASE_ASSERT(!result, result);
    // Despite its name, runClocks is accumulated execution time in microseconds.
    return Seconds::fromMicroseconds(info.runClocks);
}

} // namespace WTF
