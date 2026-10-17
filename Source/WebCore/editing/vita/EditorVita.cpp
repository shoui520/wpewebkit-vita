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
#include "Editor.h"

#include "CommonAtomStrings.h"
#include "DocumentFragment.h"
#include "FrameDestructionObserverInlines.h"
#include "LocalFrameInlines.h"
#include "NotImplemented.h"
#include "Pasteboard.h"
#include "PasteboardCustomData.h"
#include "SimpleRange.h"
#include "markup.h"

namespace WebCore {

static RefPtr<DocumentFragment> createFragmentFromPasteboardData(Pasteboard& pasteboard, LocalFrame& frame, const SimpleRange& range, bool allowPlainText, bool& chosePlainText)
{
    chosePlainText = false;

    if (RefPtr document = frame.document()) {
        auto markup = pasteboard.readString(textHTMLContentTypeAtom());
        if (!markup.isEmpty())
            return createFragmentFromMarkup(*document, markup, emptyString(), { });
    }

    if (!allowPlainText)
        return nullptr;

    auto text = pasteboard.readString(textPlainContentTypeAtom());
    if (text.isNull())
        return nullptr;
    chosePlainText = true;
    return createFragmentFromText(range, text);
}

void Editor::writeSelectionToPasteboard(Pasteboard& pasteboard)
{
    PasteboardCustomData data;
    data.writeString(textPlainContentTypeAtom(), selectedTextForDataTransfer());
    data.writeString(textHTMLContentTypeAtom(), serializePreservingVisualAppearance(document().selection().selection(), ResolveURLs::YesExcludingURLsForPrivacy, SerializeComposedTree::Yes, IgnoreUserSelectNone::Yes));
    pasteboard.writeCustomData({ WTF::move(data) });
}

void Editor::writeImageToPasteboard(Pasteboard&, Element&, const URL&, const String&)
{
    notImplemented();
}

void Editor::pasteWithPasteboard(Pasteboard* pasteboard, OptionSet<PasteOption> options)
{
    auto range = selectedRange();
    if (!range)
        return;

    bool chosePlainText;
    auto fragment = createFragmentFromPasteboardData(*pasteboard, *document().frame(), *range, options.contains(PasteOption::AllowPlainText), chosePlainText);

    if (fragment && options.contains(PasteOption::AsQuotation))
        quoteFragmentForPasting(*fragment);

    if (fragment && shouldInsertFragment(*fragment, *range, EditorInsertAction::Pasted))
        pasteAsFragment(*fragment, canSmartReplaceWithPasteboard(*pasteboard), chosePlainText, options.contains(PasteOption::IgnoreMailBlockquote) ? MailBlockquoteHandling::IgnoreBlockquote : MailBlockquoteHandling::RespectBlockquote);
}

void Editor::platformCopyFont()
{
}

void Editor::platformPasteFont()
{
}

} // namespace WebCore
