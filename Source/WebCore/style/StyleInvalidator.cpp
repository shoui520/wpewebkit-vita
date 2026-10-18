/*
 * Copyright (C) 2012-2022 Apple Inc. All rights reserved.
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
#include "StyleInvalidator.h"

#include "CSSKeyframesRule.h"
#include "Document.h"
#include "ElementChildIteratorInlines.h"
#include "ElementInlines.h"
#include "ElementRareData.h"
#include "ElementRuleCollector.h"
#include "HTMLSlotElement.h"
#include "RenderStyle+GettersInlines.h"
#include "RuleSetBuilder.h"
#include "SelectorMatchingState.h"
#include "ShadowRoot.h"
#include "StyleResolver.h"
#include "StyleRuleImport.h"
#include "StyleScope.h"
#include "StyleScopeRuleSets.h"
#include "StyleSheetContents.h"
#include "TypedElementDescendantIteratorInlines.h"
#include <ranges>
#include <wtf/SetForScope.h>

namespace WebCore {
namespace Style {

// Keyframes rules only affect elements with animations, which are found separately.
static bool shouldDirtyAllStyle(const Vector<Ref<StyleRuleBase>>& rules, bool& hasKeyframesRules)
{
    for (auto& rule : rules) {
        if (RefPtr styleRuleMedia = dynamicDowncast<StyleRuleMedia>(rule.get())) {
            if (shouldDirtyAllStyle(styleRuleMedia->childRules(), hasKeyframesRules))
                return true;
            continue;
        }
        if (RefPtr styleRuleSupports = dynamicDowncast<StyleRuleSupports>(rule.get())) {
            if (shouldDirtyAllStyle(styleRuleSupports->childRules(), hasKeyframesRules))
                return true;
            continue;
        }
        if (RefPtr styleRuleWithNesting = dynamicDowncast<StyleRuleWithNesting>(rule.get())) {
            if (shouldDirtyAllStyle(styleRuleWithNesting->nestedRules(), hasKeyframesRules))
                return true;
            continue;
        }
        if (is<StyleRuleKeyframes>(rule)) {
            hasKeyframesRules = true;
            continue;
        }
        // FIXME: At least font faces don't need full recalc in all cases.
        if (!is<StyleRule>(rule))
            return true;
    }
    return false;
}

static bool shouldDirtyAllStyle(const StyleSheetContents& sheet, bool& hasKeyframesRules)
{
    for (auto& import : sheet.importRules()) {
        if (!import->styleSheet())
            continue;
        if (shouldDirtyAllStyle(*import->styleSheet(), hasKeyframesRules))
            return true;
    }
    if (shouldDirtyAllStyle(sheet.childRules(), hasKeyframesRules))
        return true;
    return false;
}

static bool shouldDirtyAllStyle(const Vector<Ref<StyleSheetContents>>& sheets, bool& hasKeyframesRules)
{
    for (auto& sheet : sheets) {
        if (shouldDirtyAllStyle(sheet, hasKeyframesRules))
            return true;
    }
    return false;
}

Invalidator::Invalidator(const Vector<Ref<StyleSheetContents>>& sheets, const MQ::MediaQueryEvaluator& mediaQueryEvaluator)
    : m_ownedRuleSet(RuleSet::create())
    , m_ruleSets({ { m_ownedRuleSet } })
{
    m_dirtiesAllStyle = shouldDirtyAllStyle(sheets, m_invalidatesAnimations);
    if (m_dirtiesAllStyle)
        return;

    RuleSetBuilder ruleSetBuilder(*m_ownedRuleSet, mediaQueryEvaluator, nullptr, RuleSetBuilder::ShrinkToFit::Disable);

    for (auto& sheet : sheets)
        ruleSetBuilder.addRulesFromSheet(sheet);

    m_ruleInformation = collectRuleInformation();
}

Invalidator::Invalidator(const InvalidationRuleSetVector& ruleSets)
    : m_ruleSets(ruleSets)
    , m_ruleInformation(collectRuleInformation())
{
    ASSERT(m_ruleSets.size());
}

Invalidator::~Invalidator() = default;

Invalidator::RuleInformation Invalidator::collectRuleInformation()
{
    RuleInformation information;
    for (auto& ruleSet : m_ruleSets) {
        if (!ruleSet.ruleSet->slottedPseudoElementRules().isEmpty())
            information.hasSlottedPseudoElementRules = true;
        if (!ruleSet.ruleSet->hostPseudoClassRules().isEmpty())
            information.hasHostPseudoClassRules = true;
        if (ruleSet.ruleSet->hasHostPseudoClassRulesMatchingInShadowTree())
            information.hasHostPseudoClassRulesMatchingInShadowTree = true;
        if (ruleSet.ruleSet->hasUserAgentPartRules())
            information.hasUserAgentPartRules = true;
#if ENABLE(VIDEO)
        if (!ruleSet.ruleSet->cuePseudoRules().isEmpty())
            information.hasCuePseudoElementRules = true;
#endif
        if (!ruleSet.ruleSet->partPseudoElementRules().isEmpty())
            information.hasPartPseudoElementRules = true;
    }
    return information;
}

static void invalidateAssignedElements(HTMLSlotElement& slot)
{
    auto* assignedNodes = slot.assignedNodes();
    if (!assignedNodes)
        return;
    for (auto& node : *assignedNodes) {
        RefPtr element = dynamicDowncast<Element>(node.get());
        if (!element)
            continue;
        if (RefPtr slotElement = dynamicDowncast<HTMLSlotElement>(*element); slotElement && node->containingShadowRoot()) {
            invalidateAssignedElements(*slotElement);
            continue;
        }
        element->invalidateStyleInternal();
    }
}

Invalidator::CheckDescendants Invalidator::invalidateIfNeeded(Element& element, SelectorMatchingState* selectorMatchingState)
{
    invalidateInShadowTreeIfNeeded(element);

    if (m_ruleInformation.hasSlottedPseudoElementRules) {
        if (auto* slotElement = dynamicDowncast<HTMLSlotElement>(element))
            invalidateAssignedElements(*slotElement);
    }

    switch (element.styleValidity()) {
    case Validity::Valid:
    case Validity::AnimationInvalid:
    case Validity::InlineStyleInvalid: {
        for (auto& ruleSet : m_ruleSets) {
            ElementRuleCollector ruleCollector(element, *ruleSet.ruleSet, selectorMatchingState, SelectorChecker::Mode::StyleInvalidation);

            auto matches = ruleCollector.matchesAnyAuthorRules();
            if (ruleSet.isNegation == IsNegation::No ? matches : !matches) {
                element.invalidateStyleInternal();
                break;
            }
        }

        return CheckDescendants::Yes;
    }
    case Validity::ElementInvalid:
        return CheckDescendants::Yes;
    case Validity::SubtreeInvalid:
        return CheckDescendants::No;
    }
    ASSERT_NOT_REACHED();
    return CheckDescendants::Yes;
}

void Invalidator::invalidateStyleForTree(Element& root, SelectorMatchingState* selectorMatchingState)
{
    if (invalidateIfNeeded(root, selectorMatchingState) == CheckDescendants::No)
        return;
    invalidateStyleForDescendants(root, selectorMatchingState);
}

void Invalidator::invalidateStyleForDescendants(Element& root, SelectorMatchingState* selectorMatchingState)
{
    Vector<Element*, 20> parentStack;
    RefPtr previousElement = &root;
    for (auto it = descendantsOfType<Element>(root).begin(); it; ) {
        Ref descendant = *it;
        RefPtr parent = descendant->parentElement();
        if (parentStack.isEmpty() || parentStack.last() != parent.get()) {
            if (parent.get() == previousElement.get()) {
                parentStack.append(parent.get());
                if (selectorMatchingState)
                    selectorMatchingState->selectorFilter.pushParentInitializingIfNeeded(*parent);
            } else {
                while (parentStack.last() != parent.get()) {
                    parentStack.removeLast();
                    if (selectorMatchingState)
                        selectorMatchingState->selectorFilter.popParent();
                }
            }
        }
        previousElement = descendant.ptr();

        if (invalidateIfNeeded(descendant.get(), selectorMatchingState) == CheckDescendants::Yes)
            it.traverseNext();
        else
            it.traverseNextSkippingChildren();
    }
}

void Invalidator::invalidateStyle(Document& document)
{
    ASSERT(!m_dirtiesAllStyle);

    RefPtr documentElement = document.documentElement();
    if (!documentElement)
        return;

    SelectorMatchingState selectorMatchingState;
    invalidateStyleForTree(*documentElement, &selectorMatchingState);
}

static bool hasAnimations(const RenderStyle* style)
{
    if (!style)
        return false;
    if (style->hasAnimations())
        return true;
    for (auto& pseudoStyle : style->cachedPseudoStyles().values()) {
        if (pseudoStyle && pseudoStyle->hasAnimations())
            return true;
    }
    return false;
}

// An animation whose keyframes rule did not exist starts only when its element's style is resolved again.
static void invalidateElementsWithAnimations(ContainerNode& root)
{
    for (Ref element : descendantsOfType<Element>(root)) {
        if (hasAnimations(element->renderOrDisplayContentsStyle()))
            element->invalidateStyleInternal();
        if (RefPtr shadowRoot = element->shadowRoot())
            invalidateElementsWithAnimations(*shadowRoot);
    }
}

void Invalidator::invalidateStyle(Scope& scope)
{
    if (m_dirtiesAllStyle) {
        invalidateAllStyle(scope);
        return;
    }

    RefPtr shadowRoot = scope.shadowRoot();
    if (shadowRoot)
        invalidateStyle(*shadowRoot);
    else
        invalidateStyle(scope.document());

    if (!m_invalidatesAnimations)
        return;
    if (!shadowRoot) {
        invalidateElementsWithAnimations(scope.document());
        return;
    }
    // The shadow tree's keyframes can also animate its host and slotted elements.
    if (RefPtr host = shadowRoot->host()) {
        if (hasAnimations(host->renderOrDisplayContentsStyle()))
            host->invalidateStyleInternal();
        invalidateElementsWithAnimations(*host);
    }
    invalidateElementsWithAnimations(*shadowRoot);
}

void Invalidator::invalidateStyle(ShadowRoot& shadowRoot)
{
    ASSERT(!m_dirtiesAllStyle);

    if (m_ruleInformation.hasHostPseudoClassRules && shadowRoot.host())
        shadowRoot.host()->invalidateStyleInternal();

    for (Ref child : childrenOfType<Element>(shadowRoot)) {
        SelectorMatchingState selectorMatchingState;
        invalidateStyleForTree(child.get(), &selectorMatchingState);
    }
}

void Invalidator::invalidateStyle(Element& element)
{
    ASSERT(!m_dirtiesAllStyle);

    // Don't use SelectorFilter as the rule sets here tend to be small and the filter would have setup cost deep in the tree.
    invalidateStyleForTree(element, nullptr);
}

// Collects the compound selectors whose :has() takes HAS_ARGUMENT (the anchors). Fails if such a :has() is
// nested in another pseudo-class, as in :not(:has(.changed)) .subject, or if there is none.
static bool collectHasAnchors(const CSSSelector& complexSelector, const CSSSelector& hasArgument, bool isNested, Vector<const CSSSelector*, 8>& anchors, bool& found)
{
    auto* lastInCompound = &complexSelector;
    for (auto* simpleSelector = &complexSelector; simpleSelector; simpleSelector = simpleSelector->precedingInComplexSelector()) {
        if (auto* selectorList = simpleSelector->selectorList()) {
            bool isHas = simpleSelector->match() == CSSSelector::Match::PseudoClass && simpleSelector->pseudoClass() == CSSSelector::PseudoClass::Has;
            for (auto& subSelector : *selectorList) {
                if (!isHas) {
                    if (!collectHasAnchors(subSelector, hasArgument, true, anchors, found))
                        return false;
                    continue;
                }
                if (!complexSelectorsEqual(subSelector, hasArgument))
                    continue;
                if (isNested)
                    return false;
                anchors.appendIfNotContains(lastInCompound);
                found = true;
            }
        }
        if (simpleSelector->relation() != CSSSelector::Relation::Subselector)
            lastInCompound = simpleSelector->precedingInComplexSelector();
    }
    return isNested || found;
}

// Whether the element can match the compound selector, judging by its tag, id and class selectors only.
static bool mayMatchCompound(const CSSSelector& lastInCompound, const Element& element)
{
    bool inQuirksMode = element.document().inQuirksMode();
    for (auto* simpleSelector = &lastInCompound; simpleSelector; simpleSelector = simpleSelector->precedingInCompound()) {
        switch (simpleSelector->match()) {
        case CSSSelector::Match::Tag: {
            auto& localName = simpleSelector->tagQName().localName();
            if (localName != starAtom() && !equalIgnoringASCIICase(localName, element.localName()))
                return false;
            break;
        }
        case CSSSelector::Match::Id:
            if (!inQuirksMode && element.getIdAttribute() != simpleSelector->value())
                return false;
            break;
        case CSSSelector::Match::Class:
            if (!inQuirksMode && !element.hasClassName(simpleSelector->value()))
                return false;
            break;
        default:
            break;
        }
    }
    return true;
}

// For :has(.changed) .subject, the element matching the :has() compound (the anchor) is an ancestor of the changed
// element, or a previous sibling of it or of an ancestor; scope breaking :has() arguments are handled separately.
// The subject is a descendant or following sibling of the anchor, or a descendant of such a sibling, so only the
// subtree of the topmost possible anchor's parent needs to be checked.
static RefPtr<Element> hasNonSubjectInvalidationRoot(Element& element)
{
    RefPtr documentElement = element.document().documentElement();
    if (element.isInShadowTree() || !element.isConnected())
        return documentElement;

    Vector<const CSSSelector*, 8> anchors;
    for (auto& features : element.styleResolver().ruleSets().features().hasPseudoClassRules.values()) {
        for (auto& feature : *features) {
            if (feature.matchElement != MatchElement::HasNonSubject)
                continue;
            bool found = false;
            if (!collectHasAnchors(feature.selector(), feature.invalidationSelector.first(), false, anchors, found))
                return documentElement;
        }
    }

    Vector<Element*, 32> elementAndAncestors;
    for (RefPtr ancestor = &element; ancestor; ancestor = ancestor->parentElement())
        elementAndAncestors.append(ancestor.get());

    for (auto* elementOrAncestor : elementAndAncestors | std::views::reverse) {
        for (RefPtr candidate = elementOrAncestor; candidate; candidate = candidate->previousElementSibling()) {
            for (auto* anchor : anchors) {
                if (mayMatchCompound(*anchor, *candidate)) {
                    RefPtr parent = elementOrAncestor->parentElement();
                    return parent ? parent : documentElement;
                }
            }
        }
    }
    return nullptr;
}

void Invalidator::invalidateStyleWithMatchElement(Element& element, MatchElement matchElement)
{
    switch (matchElement) {
    case MatchElement::Subject: {
        // .changed
        invalidateIfNeeded(element, nullptr);
        break;
    }
    case MatchElement::Parent: {
        // .changed > .subject
        auto children = childrenOfType<Element>(element);
        for (Ref child : children)
            invalidateIfNeeded(child.get(), nullptr);
        break;
    }
    case MatchElement::Ancestor: {
        // .changed .subject
        SelectorMatchingState selectorMatchingState;
        invalidateStyleForDescendants(element, &selectorMatchingState);
        break;
    }
    case MatchElement::DirectSibling:
        // .changed + .subject
        if (RefPtr sibling = element.nextElementSibling())
            invalidateIfNeeded(*sibling, nullptr);
        break;
    case MatchElement::IndirectSibling:
        // .changed ~ .subject
        for (RefPtr sibling = element.nextElementSibling(); sibling; sibling = sibling->nextElementSibling())
            invalidateIfNeeded(*sibling, nullptr);
        break;
    case MatchElement::AnySibling:
        // :nth-last-child(even of .changed)
        for (Ref parentChild : childrenOfType<Element>(*element.parentNode()))
            invalidateIfNeeded(parentChild.get(), nullptr);
        break;
    case MatchElement::ParentSibling:
        // .changed ~ .a > .subject
        for (RefPtr sibling = element.nextElementSibling(); sibling; sibling = sibling->nextElementSibling()) {
            auto siblingChildren = childrenOfType<Element>(*sibling);
            for (Ref siblingChild : siblingChildren)
                invalidateIfNeeded(siblingChild.get(), nullptr);
        }
        break;
    case MatchElement::AncestorSibling: {
        // .changed ~ .a .subject
        SelectorMatchingState selectorMatchingState;
        for (RefPtr sibling = element.nextElementSibling(); sibling; sibling = sibling->nextElementSibling()) {
            selectorMatchingState.selectorFilter.popParentsUntil(element.parentElement());
            invalidateStyleForDescendants(*sibling, &selectorMatchingState);
        }
        break;
    }
    case MatchElement::ParentAnySibling:
        // :nth-last-child(even of .changed) > .subject
        for (Ref sibling : childrenOfType<Element>(*element.parentNode())) {
            auto siblingChildren = childrenOfType<Element>(sibling.get());
            for (Ref siblingChild : siblingChildren)
                invalidateIfNeeded(siblingChild.get(), nullptr);
        }
        break;
    case MatchElement::AncestorAnySibling: {
        // :nth-last-child(even of .changed) .subject
        SelectorMatchingState selectorMatchingState;
        for (Ref sibling : childrenOfType<Element>(*element.parentNode())) {
            selectorMatchingState.selectorFilter.popParentsUntil(element.parentElement());
            invalidateStyleForDescendants(sibling.get(), &selectorMatchingState);
        }
        break;
    }
    case MatchElement::HasChild: {
        // :has(> .changed)
        if (RefPtr parent = element.parentElement())
            invalidateIfNeeded(*parent, nullptr);
        break;
    }
    case MatchElement::HasDescendant: {
        // :has(.changed)
        Vector<Element*, 16> ancestors;
        for (RefPtr parent = element.parentElement(); parent; parent = parent->parentElement())
            ancestors.append(parent.get());

        SelectorMatchingState selectorMatchingState;
        selectorMatchingState.selectorFilter.parentStackReserveInitialCapacity(ancestors.size());
        for (RefPtr ancestor : ancestors | std::views::reverse) {
            invalidateIfNeeded(*ancestor, &selectorMatchingState);
            selectorMatchingState.selectorFilter.pushParent(ancestor.get());
        }
        break;
    }
    case MatchElement::HasSibling:
        // :has(~ .changed)
        if (RefPtr sibling = element.previousElementSibling()) {
            SelectorMatchingState selectorMatchingState;
            if (RefPtr parent = element.parentElement())
                selectorMatchingState.selectorFilter.pushParentInitializingIfNeeded(*parent);

            for (; sibling; sibling = sibling->previousElementSibling())
                invalidateIfNeeded(*sibling, &selectorMatchingState);
        }
        break;

    case MatchElement::HasChildParent:
        // :has(> .changed) > .subject
    case MatchElement::HasAnySibling: {
        // :has(~ :is(.changed ~ .x))
        SelectorMatchingState selectorMatchingState;
        if (RefPtr parent = element.parentElement())
            selectorMatchingState.selectorFilter.pushParentInitializingIfNeeded(*parent);
        for (Ref sibling : childrenOfType<Element>(*element.parentNode()))
            invalidateIfNeeded(sibling.get(), &selectorMatchingState);
        break;
    }
    case MatchElement::HasSiblingDescendant: {
        // :has(~ .a .changed)
        Vector<Element*, 16> elementAndAncestors;
        elementAndAncestors.append(&element);
        for (RefPtr parent = element.parentElement(); parent; parent = parent->parentElement())
            elementAndAncestors.append(parent.get());

        SelectorMatchingState selectorMatchingState;
        selectorMatchingState.selectorFilter.parentStackReserveInitialCapacity(elementAndAncestors.size());
        for (RefPtr elementOrAncestor : elementAndAncestors | std::views::reverse) {
            for (RefPtr sibling = elementOrAncestor->previousElementSibling(); sibling; sibling = sibling->previousElementSibling())
                invalidateIfNeeded(*sibling, &selectorMatchingState);

            selectorMatchingState.selectorFilter.pushParent(elementOrAncestor.get());
        }
        break;
    }
    case MatchElement::HasDescendantParent: {
        // :has(.changed) > .subject
        Vector<Element*, 16> ancestors;
        for (RefPtr parent = element.parentElement(); parent; parent = parent->parentElement())
            ancestors.append(parent.get());

        SelectorMatchingState selectorMatchingState;
        selectorMatchingState.selectorFilter.parentStackReserveInitialCapacity(ancestors.size());
        for (RefPtr ancestor : ancestors | std::views::reverse) {
            selectorMatchingState.selectorFilter.pushParent(ancestor.get());
            for (Ref ancestorChild : childrenOfType<Element>(*ancestor))
                invalidateIfNeeded(ancestorChild.get(), &selectorMatchingState);
        }
        break;
    }
    case MatchElement::HasChildAncestor: {
        // :has(> .changed) .subject
        if (CheckedPtr parent = element.parentElement()) {
            SelectorMatchingState selectorMatchingState;
            invalidateStyleForDescendants(*parent, &selectorMatchingState);
        }
        break;
    }
    case MatchElement::HasNonSubject: {
        // :has(.changed) .subject
        if (RefPtr root = hasNonSubjectInvalidationRoot(element)) {
            SelectorMatchingState selectorMatchingState;
            invalidateStyleForDescendants(*root, &selectorMatchingState);
        }
        break;
    }
    case MatchElement::HasScopeBreaking: {
        // :has(:is(.changed .a))
        SelectorMatchingState selectorMatchingState;
        invalidateStyleForDescendants(*element.document().documentElement(), &selectorMatchingState);
        break;
    }
    case MatchElement::Host:
        // :host(.changed) .subject
        invalidateInShadowTreeIfNeeded(element);
        break;
    case MatchElement::HostChild:
        // ::slotted(.changed)
        if (RefPtr host = element.shadowHost()) {
            for (Ref hostChild : childrenOfType<Element>(*host))
                invalidateIfNeeded(hostChild.get(), nullptr);
        }
        break;
    }
}

void Invalidator::invalidateShadowParts(ShadowRoot& shadowRoot)
{
    if (shadowRoot.mode() == ShadowRootMode::UserAgent)
        return;

    for (Ref descendant : descendantsOfType<Element>(shadowRoot)) {
        // FIXME: We could only invalidate part names that actually show up in rules.
        if (!descendant->partNames().isEmpty())
            descendant->invalidateStyleInternal();

        RefPtr nestedShadowRoot = descendant->shadowRoot();
        if (nestedShadowRoot && !nestedShadowRoot->partMappings().isEmpty())
            invalidateShadowParts(*nestedShadowRoot);
    }
}

void Invalidator::invalidateUserAgentParts(ShadowRoot& shadowRoot)
{
    if (shadowRoot.mode() != ShadowRootMode::UserAgent)
        return;

    for (Ref descendant : descendantsOfType<Element>(shadowRoot)) {
        auto& part = descendant->userAgentPart();
        if (!part)
            continue;
        for (auto& ruleSet : m_ruleSets) {
            if (ruleSet.ruleSet->userAgentPartRules(part))
                descendant->invalidateStyleInternal();
        }
    }
}

void Invalidator::invalidateInShadowTreeIfNeeded(Element& element)
{
    RefPtr shadowRoot = element.shadowRoot();
    if (!shadowRoot)
        return;

    if (m_ruleInformation.hasUserAgentPartRules)
        invalidateUserAgentParts(*shadowRoot);

    if (m_ruleInformation.hasHostPseudoClassRulesMatchingInShadowTree) {
        for (Ref child : childrenOfType<Element>(*shadowRoot)) {
            SelectorMatchingState selectorMatchingState;
            invalidateStyleForTree(child.get(), &selectorMatchingState);
        }
    }

#if ENABLE(VIDEO)
    if (m_ruleInformation.hasCuePseudoElementRules && element.isMediaElement())
        element.invalidateStyleForSubtreeInternal();
#endif

    // FIXME: More fine-grained invalidation for ::part()
    if (m_ruleInformation.hasPartPseudoElementRules)
        invalidateShadowParts(*shadowRoot);
}

void Invalidator::addToMatchElementRuleSets(Invalidator::MatchElementRuleSets& matchElementRuleSets, const InvalidationRuleSet& invalidationRuleSet)
{
    matchElementRuleSets.ensure(invalidationRuleSet.matchElement, [] {
        return InvalidationRuleSetVector { };
    }).iterator->value.append({ invalidationRuleSet.ruleSet.copyRef(), IsNegation::No });
}

void Invalidator::addToMatchElementRuleSetsRespectingNegation(Invalidator::MatchElementRuleSets& matchElementRuleSets, const InvalidationRuleSet& invalidationRuleSet)
{
    matchElementRuleSets.ensure(invalidationRuleSet.matchElement, [] {
        return InvalidationRuleSetVector { };
    }).iterator->value.append({ invalidationRuleSet.ruleSet.copyRef(), invalidationRuleSet.isNegation });
}

void Invalidator::invalidateWithMatchElementRuleSets(Element& element, const MatchElementRuleSets& matchElementRuleSets)
{
    SetForScope isInvalidating(element.styleResolver().ruleSets().isInvalidatingStyleWithRuleSets(), true);

    for (auto& matchElementAndRuleSet : matchElementRuleSets) {
        Invalidator invalidator(matchElementAndRuleSet.value);
        invalidator.invalidateStyleWithMatchElement(element, matchElementAndRuleSet.key);
    }
}

void Invalidator::invalidateWithScopeBreakingHasPseudoClassRuleSet(Element& element, const RuleSet* ruleSet)
{
    SetForScope isInvalidating(element.styleResolver().ruleSets().isInvalidatingStyleWithRuleSets(), true);
    Invalidator invalidator(InvalidationRuleSetVector { { ruleSet } });
    invalidator.invalidateStyleWithMatchElement(element, MatchElement::HasScopeBreaking);
}

void Invalidator::invalidateAllStyle(Scope& scope)
{
    if (RefPtr shadowRoot = scope.shadowRoot()) {
        for (Ref shadowChild : childrenOfType<Element>(*shadowRoot))
            shadowChild->invalidateStyleForSubtreeInternal();
        invalidateHostAndSlottedStyleIfNeeded(*shadowRoot);
        return;
    }

    scope.document().scheduleFullStyleRebuild();
}

void Invalidator::invalidateHostAndSlottedStyleIfNeeded(ShadowRoot& shadowRoot)
{
    Ref host = *shadowRoot.host();
    RefPtr resolver = shadowRoot.styleScope().resolverIfExists();

    if (!resolver || resolver->ruleSets().hasMatchingUserOrAuthorStyle([] (auto& style) { return !style.hostPseudoClassRules().isEmpty(); }))
        host->invalidateStyleInternal();

    if (!resolver || resolver->ruleSets().hasMatchingUserOrAuthorStyle([] (auto& style) { return !style.slottedPseudoElementRules().isEmpty(); })) {
        for (Ref shadowChild : childrenOfType<Element>(host.get()))
            shadowChild->invalidateStyleInternal();
    }
}

}
}
