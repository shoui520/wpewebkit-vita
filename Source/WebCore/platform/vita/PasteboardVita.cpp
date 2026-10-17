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
#include "Pasteboard.h"

#include "CommonAtomStrings.h"
#include "PasteboardCustomData.h"
#include "PasteboardItemInfo.h"
#include "PasteboardStrategy.h"
#include "PlatformStrategies.h"
#include "SharedBuffer.h"
#include <wtf/NeverDestroyed.h>

namespace WebCore {

// Vita has one copy-and-paste pasteboard holding a single item. Reads and
// writes go through the embedder's PasteboardStrategy, which owns the data
// and may mirror plain text to the system clipboard. Single-type writes are
// merged into the item this process wrote last, as long as the strategy has
// not replaced it since.

static PasteboardCustomData& lastWrittenData()
{
    static NeverDestroyed<PasteboardCustomData> data;
    return data.get();
}

static int64_t lastWrittenGeneration;

static CheckedPtr<PasteboardStrategy> pasteboardStrategy()
{
    return platformStrategies()->pasteboardStrategy();
}

Pasteboard::Pasteboard(std::unique_ptr<PasteboardContext>&& context)
    : m_context(WTF::move(context))
{
}

std::unique_ptr<Pasteboard> Pasteboard::createForCopyAndPaste(std::unique_ptr<PasteboardContext>&& context)
{
    return makeUnique<Pasteboard>(WTF::move(context));
}

static bool lastWrittenDataIsCurrent(const Pasteboard& pasteboard)
{
    auto strategy = pasteboardStrategy();
    return strategy && strategy->allPasteboardItemInfo(pasteboard.name(), lastWrittenGeneration, pasteboard.context());
}

static void writeItems(const Pasteboard& pasteboard, Vector<PasteboardCustomData>&& items)
{
    auto strategy = pasteboardStrategy();
    if (!strategy)
        return;
    lastWrittenGeneration = strategy->writeCustomData(items, pasteboard.name(), pasteboard.context());
    lastWrittenData() = items.isEmpty() ? PasteboardCustomData { } : WTF::move(items[0]);
}

template<typename Function>
static void updateItem(const Pasteboard& pasteboard, Function&& update)
{
    auto data = lastWrittenDataIsCurrent(pasteboard) ? lastWrittenData() : PasteboardCustomData { };
    update(data);
    writeItems(pasteboard, { WTF::move(data) });
}

bool Pasteboard::hasData()
{
    auto strategy = pasteboardStrategy();
    return strategy && strategy->getPasteboardItemsCount(name(), context()) > 0;
}

Vector<String> Pasteboard::typesSafeForBindings(const String& origin)
{
    auto strategy = pasteboardStrategy();
    return strategy ? strategy->typesSafeForDOMToReadAndWrite(name(), origin, context()) : Vector<String> { };
}

Vector<String> Pasteboard::typesForLegacyUnsafeBindings()
{
    if (lastWrittenDataIsCurrent(*this))
        return lastWrittenData().orderedTypes();
    return typesSafeForBindings({ });
}

String Pasteboard::readOrigin()
{
    return lastWrittenDataIsCurrent(*this) ? lastWrittenData().origin() : String { };
}

String Pasteboard::readString(const String& type)
{
    return readString(0, type);
}

String Pasteboard::readStringInCustomData(const String& type)
{
    return lastWrittenDataIsCurrent(*this) ? lastWrittenData().readStringInCustomData(type) : String { };
}

void Pasteboard::writeString(const String& type, const String& data)
{
    updateItem(*this, [&](auto& item) {
        item.writeString(type, data);
    });
}

void Pasteboard::clear()
{
    writeItems(*this, { });
}

void Pasteboard::clear(const String& type)
{
    updateItem(*this, [&](auto& item) {
        item.clear(type);
    });
}

void Pasteboard::read(PasteboardPlainText& text, PlainTextURLReadingPolicy, std::optional<size_t> itemIndex)
{
    text.text = readString(itemIndex.value_or(0), textPlainContentTypeAtom());
}

void Pasteboard::read(PasteboardWebContentReader&, WebContentReadingPolicy, std::optional<size_t>)
{
    // PasteboardWebContentReader has no readers on this platform; the editor
    // reads markup and text directly.
}

void Pasteboard::read(PasteboardFileReader&, std::optional<size_t>)
{
}

void Pasteboard::write(const Color&)
{
}

void Pasteboard::write(const PasteboardURL& pasteboardURL)
{
    PasteboardCustomData data;
    data.writeString("text/uri-list"_s, pasteboardURL.url.string());
    data.writeString(textPlainContentTypeAtom(), pasteboardURL.url.string());
    writeItems(*this, { WTF::move(data) });
}

void Pasteboard::writeTrustworthyWebURLsPboardType(const PasteboardURL&)
{
}

void Pasteboard::write(const PasteboardImage& pasteboardImage)
{
    if (!pasteboardImage.url.url.isEmpty())
        write(pasteboardImage.url);
}

void Pasteboard::write(const PasteboardBuffer& pasteboardBuffer)
{
    if (!pasteboardBuffer.data)
        return;
    updateItem(*this, [&](auto& item) {
        item.writeData(pasteboardBuffer.type, Ref { *pasteboardBuffer.data });
    });
}

void Pasteboard::write(const PasteboardWebContent&)
{
    // PasteboardWebContent carries no fields on this platform; the editor
    // writes markup and text through writeCustomData().
}

void Pasteboard::writeCustomData(const Vector<PasteboardCustomData>& items)
{
    writeItems(*this, Vector<PasteboardCustomData> { items });
}

Pasteboard::FileContentState Pasteboard::fileContentState()
{
    return FileContentState::NoFileOrImageData;
}

bool Pasteboard::canSmartReplace()
{
    return false;
}

void Pasteboard::writeMarkup(const String& markup)
{
    updateItem(*this, [&](auto& item) {
        item.writeString(textHTMLContentTypeAtom(), markup);
    });
}

void Pasteboard::writePlainText(const String& text, SmartReplaceOption)
{
    PasteboardCustomData data;
    data.writeString(textPlainContentTypeAtom(), text);
    writeItems(*this, { WTF::move(data) });
}

} // namespace WebCore
