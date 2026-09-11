/*
 * Copyright (C) 2008-2016 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Torch Mobile, Inc. http://www.torchmobile.com/
 * Copyright (C) 2010 Google, Inc. All rights reserved.
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
#include "HTMLTokenizer.h"

#include "CSSTokenizerInputStream.h"
#include "HTMLEntityParser.h"
#include "HTMLNames.h"
#include "MarkupTokenizerInlines.h"
#include <wtf/UnalignedAccess.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace WebCore {

using namespace HTMLNames;

static constexpr uint32_t broadcastByte(uint8_t value)
{
    return 0x01010101U * value;
}

static constexpr uint32_t byteLaneOnes = broadcastByte(1);
static constexpr uint32_t byteLaneHighBits = broadcastByte(0x80);

// Sets the high bit of every lane holding `broadcastValue`'s byte. Lanes following a zero lane
// can also come out set, because the subtraction borrows across lane boundaries; that only ever
// adds lanes, never drops one, which is all the scan below relies on.
ALWAYS_INLINE static uint32_t wordHasByte(uint32_t word, uint32_t broadcastValue)
{
    uint32_t difference = word ^ broadcastValue;
    return (difference - byteLaneOnes) & ~difference;
}

// Same, for lanes holding a byte below `broadcastLimit`'s byte, which must be in 1..127.
ALWAYS_INLINE static uint32_t wordHasByteBelow(uint32_t word, uint32_t broadcastLimit)
{
    return (word - broadcastLimit) & ~word;
}

// '\0', '\n' and '\r' are the only characters below 0x0E that any of the states below care
// about, so one comparison covers all three. The other bytes under that bound are treated as a
// hit as well, which merely ends the run a few characters early.
static constexpr uint32_t broadcastControlLimit = broadcastByte(0x0E);

// Scans four bytes at a time with a word predicate that is allowed to flag lanes the real
// predicate would not, and confirms each flagged word one byte at a time. Returning a shorter
// run than necessary is always safe: the characters that were not taken go back through the
// per-character state machine, which is where they would have gone anyway.
template<typename WordPredicate, typename Predicate>
ALWAYS_INLINE static std::span<const Latin1Character> findPlainTextWordAtATime(std::span<const Latin1Character> span, WordPredicate&& wordHasSpecial, Predicate&& isSpecial)
{
    size_t size = span.size();
    size_t i = 0;
    while (i + 4 <= size) {
        if (wordHasSpecial(WTF::unalignedLoad<uint32_t>(span.subspan(i, 4).data()))) [[unlikely]] {
            for (size_t candidate = i; candidate < i + 4; ++candidate) {
                if (isSpecial(span[candidate]))
                    return span.first(candidate);
            }
        }
        i += 4;
    }
    while (i < size && !isSpecial(span[i]))
        ++i;
    return span.first(i);
}

template<typename Predicate>
ALWAYS_INLINE static std::span<const Latin1Character> findPlainTextBatch(std::span<const Latin1Character> span, Predicate&& isSpecial)
{
    size_t size = span.size();
    size_t i = 0;
    while (i + 4 <= size) {
        bool s0 = isSpecial(span[i]);
        bool s1 = isSpecial(span[i + 1]);
        bool s2 = isSpecial(span[i + 2]);
        bool s3 = isSpecial(span[i + 3]);

        if (s0 | s1 | s2 | s3) [[unlikely]] {
            if (s0)
                return span.first(i);
            if (s1)
                return span.first(i + 1);
            if (s2)
                return span.first(i + 2);
            return span.first(i + 3);
        }
        i += 4;
    }
    while (i < size && !isSpecial(span[i]))
        ++i;
    return span.first(i);
}

// https://html.spec.whatwg.org/multipage/parsing.html#data-state
// In the Data state, '<' switches to TagOpenState, '&' switches to a character
// reference, and '\0' is a parse error. '\r' and '\n' require special handling
// for line counting and newline normalization in the input stream preprocessor.
// Everything else is plain text that can be emitted as-is.
ALWAYS_INLINE static bool isDataStateSpecialCharacter(Latin1Character c)
{
    return c == '<' || c == '&' || c == '\r' || c == '\n' || c == '\0';
}

ALWAYS_INLINE static uint32_t dataStateWordHasSpecialCharacter(uint32_t word)
{
    return (wordHasByteBelow(word, broadcastControlLimit)
        | wordHasByte(word, broadcastByte('<'))
        | wordHasByte(word, broadcastByte('&'))) & byteLaneHighBits;
}

// Returns the leading prefix of `span` that contains only plain-text characters
// (i.e. characters that don't require special handling in the Data state).
static std::span<const Latin1Character> findPlainTextInDataState(std::span<const Latin1Character> span)
{
    return findPlainTextWordAtATime(span, dataStateWordHasSpecialCharacter, isDataStateSpecialCharacter);
}

// https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-(double-quoted)-state
// https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-(single-quoted)-state
// In quoted attribute value states, the closing quote ends the value, '&' starts
// a character reference, and '\0' is a parse error. '\r' and '\n' require special
// handling for line counting and newline normalization. Everything else is plain
// text that can be appended to the attribute value as-is.
template<Latin1Character quoteCharacter>
ALWAYS_INLINE static bool isQuotedAttributeValueSpecialCharacter(Latin1Character c)
{
    return c == quoteCharacter || c == '&' || c == '\r' || c == '\n' || c == '\0';
}

template<Latin1Character quoteCharacter>
ALWAYS_INLINE static uint32_t quotedAttributeValueWordHasSpecialCharacter(uint32_t word)
{
    return (wordHasByteBelow(word, broadcastControlLimit)
        | wordHasByte(word, broadcastByte(quoteCharacter))
        | wordHasByte(word, broadcastByte('&'))) & byteLaneHighBits;
}

template<Latin1Character quoteCharacter>
static std::span<const Latin1Character> findPlainTextInQuotedAttributeValue(std::span<const Latin1Character> span)
{
    return findPlainTextWordAtATime(span, quotedAttributeValueWordHasSpecialCharacter<quoteCharacter>, isQuotedAttributeValueSpecialCharacter<quoteCharacter>);
}

// https://html.spec.whatwg.org/multipage/parsing.html#rawtext-state
// https://html.spec.whatwg.org/multipage/parsing.html#script-data-state
// RAWTEXT and script data have no character references, so only '<' ends a plain-text run.
ALWAYS_INLINE static bool isRawTextSpecialCharacter(Latin1Character c)
{
    return c == '<' || c == '\r' || c == '\n' || c == '\0';
}

ALWAYS_INLINE static uint32_t rawTextWordHasSpecialCharacter(uint32_t word)
{
    return (wordHasByteBelow(word, broadcastControlLimit)
        | wordHasByte(word, broadcastByte('<'))) & byteLaneHighBits;
}

static std::span<const Latin1Character> findPlainTextInRawText(std::span<const Latin1Character> span)
{
    return findPlainTextWordAtATime(span, rawTextWordHasSpecialCharacter, isRawTextSpecialCharacter);
}

// https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-state
// https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-state
// Inside a `<!--` comment in a script, '-' also has to be looked at.
ALWAYS_INLINE static bool isEscapedScriptDataSpecialCharacter(Latin1Character c)
{
    return c == '-' || c == '<' || c == '\r' || c == '\n' || c == '\0';
}

ALWAYS_INLINE static uint32_t escapedScriptDataWordHasSpecialCharacter(uint32_t word)
{
    return (wordHasByteBelow(word, broadcastControlLimit)
        | wordHasByte(word, broadcastByte('<'))
        | wordHasByte(word, broadcastByte('-'))) & byteLaneHighBits;
}

static std::span<const Latin1Character> findPlainTextInEscapedScriptData(std::span<const Latin1Character> span)
{
    return findPlainTextWordAtATime(span, escapedScriptDataWordHasSpecialCharacter, isEscapedScriptDataSpecialCharacter);
}

// https://html.spec.whatwg.org/multipage/parsing.html#comment-state
// Only '-' can begin the end of a comment.
ALWAYS_INLINE static bool isCommentSpecialCharacter(Latin1Character c)
{
    return c == '-' || c == '\r' || c == '\n' || c == '\0';
}

ALWAYS_INLINE static uint32_t commentWordHasSpecialCharacter(uint32_t word)
{
    return (wordHasByteBelow(word, broadcastControlLimit)
        | wordHasByte(word, broadcastByte('-'))) & byteLaneHighBits;
}

static std::span<const Latin1Character> findPlainTextInComment(std::span<const Latin1Character> span)
{
    return findPlainTextWordAtATime(span, commentWordHasSpecialCharacter, isCommentSpecialCharacter);
}

// https://html.spec.whatwg.org/multipage/parsing.html#tag-name-state
// A run of lowercase letters and digits needs neither case folding nor any of the delimiter
// tests, so it can go straight into the token. Everything else, uppercase included, falls back
// to the per-character path that does the lowercasing.
ALWAYS_INLINE static bool isTagNameSpecialCharacter(Latin1Character c)
{
    return !isASCIILower(c) && !isASCIIDigit(c);
}

static std::span<const Latin1Character> findPlainTextInTagName(std::span<const Latin1Character> span)
{
    return findPlainTextBatch(span, isTagNameSpecialCharacter);
}

// https://html.spec.whatwg.org/multipage/parsing.html#attribute-name-state
// As above, plus the two joiners that appear in nearly every custom and data-* attribute.
ALWAYS_INLINE static bool isAttributeNameSpecialCharacter(Latin1Character c)
{
    return !isASCIILower(c) && !isASCIIDigit(c) && c != '-' && c != '_';
}

static std::span<const Latin1Character> findPlainTextInAttributeName(std::span<const Latin1Character> span)
{
    return findPlainTextBatch(span, isAttributeNameSpecialCharacter);
}

// https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-(unquoted)-state
// The delimiters, the character-reference start, the characters the preprocessor must see, and
// the characters that merely raise a (silent) parse error.
ALWAYS_INLINE static bool isUnquotedAttributeValueSpecialCharacter(Latin1Character c)
{
    switch (c) {
    case '\0':
    case '\t':
    case '\n':
    case '\f':
    case '\r':
    case ' ':
    case '"':
    case '&':
    case '\'':
    case '<':
    case '=':
    case '>':
    case '`':
        return true;
    default:
        return false;
    }
}

static std::span<const Latin1Character> findPlainTextInUnquotedAttributeValue(std::span<const Latin1Character> span)
{
    return findPlainTextBatch(span, isUnquotedAttributeValueSpecialCharacter);
}

// Scans ahead for a run of characters that the current state would only ever copy through, and
// hands the whole run over in one call, skipping the per-character trip around the state machine
// (preprocessor peek, delimiter tests, goto).
//
// The current character has already been appended by the caller, so the scan starts at index 1
// and stops one short of the end: advancePastMultiple8 needs at least one character to remain in
// the substring to become the next current character.
//
// The scan is skipped when skipNextNewLine() is set (the current character was a '\r'): the
// preprocessor has to see the very next character itself to collapse a following '\n', and
// batching past it would drop that '\n'.
#define BATCH_PLAIN_TEXT(findPlainText, appendPlainText, state)                             \
    do {                                                                                    \
        auto batchSource = source.currentSubstringSpan8();                                  \
        if (batchSource.size() > 2 && !m_preprocessor.skipNextNewLine()) {                  \
            auto plainText = findPlainText(batchSource.subspan(1, batchSource.size() - 2)); \
            if (!plainText.empty()) {                                                       \
                appendPlainText(plainText);                                                 \
                source.advancePastMultiple8(plainText.size() + 1);                          \
                SWITCH_TO(state);                                                           \
            }                                                                               \
        }                                                                                   \
    } while (false)

static inline Latin1Character NODELETE convertASCIIAlphaToLower(char16_t character)
{
    ASSERT(isASCIIAlpha(character));
    return toASCIILowerUnchecked(character);
}

inline bool HTMLTokenizer::inEndTagBufferingState() const
{
    switch (m_state) {
    case RCDATAEndTagOpenState:
    case RCDATAEndTagNameState:
    case RAWTEXTEndTagOpenState:
    case RAWTEXTEndTagNameState:
    case ScriptDataEndTagOpenState:
    case ScriptDataEndTagNameState:
    case ScriptDataEscapedEndTagOpenState:
    case ScriptDataEscapedEndTagNameState:
        return true;
    default:
        return false;
    }
}

HTMLTokenizer::HTMLTokenizer(const HTMLParserOptions& options)
    : m_options(options)
{
}

inline void HTMLTokenizer::bufferASCIICharacter(char16_t character)
{
    ASSERT(character != kEndOfFileMarker);
    ASSERT(isASCII(character));
    if (m_shouldDiscardCharacterData) [[unlikely]] {
        m_token.makeCharacterWithoutData();
        return;
    }
    Latin1Character narrowedCharacter = character;
    m_token.appendToCharacter(narrowedCharacter);
}

inline void HTMLTokenizer::bufferCharacter(char16_t character)
{
    ASSERT(character != kEndOfFileMarker);
    if (m_shouldDiscardCharacterData) [[unlikely]] {
        m_token.makeCharacterWithoutData();
        return;
    }
    m_token.appendToCharacter(character);
}

template<typename CharacterType>
inline void HTMLTokenizer::bufferCharacters(std::span<const CharacterType> characters)
{
#if ASSERT_ENABLED
    for (auto character : characters)
        ASSERT(character != kEndOfFileMarker);
#endif
    if (m_shouldDiscardCharacterData) [[unlikely]] {
        m_token.makeCharacterWithoutData();
        return;
    }
    m_token.appendToCharacter(characters);
}

inline bool HTMLTokenizer::emitAndResumeInDataState(SegmentedString& source)
{
    saveEndTagNameIfNeeded();
    m_state = DataState;
    source.advancePastNonNewline();
    return true;
}

inline bool HTMLTokenizer::emitAndReconsumeInDataState()
{
    saveEndTagNameIfNeeded();
    m_state = DataState;
    return true;
}

inline bool HTMLTokenizer::emitEndOfFile(SegmentedString& source)
{
    m_state = DataState;
    if (haveBufferedCharacterToken())
        return true;
    source.advance();
    m_token.clear();
    m_token.makeEndOfFile();
    return true;
}

inline void HTMLTokenizer::saveEndTagNameIfNeeded()
{
    ASSERT(m_token.type() != HTMLToken::Type::Uninitialized);
    if (m_token.type() == HTMLToken::Type::StartTag)
        m_appropriateEndTagName = m_token.name();
}

inline bool HTMLTokenizer::haveBufferedCharacterToken() const
{
    return m_token.type() == HTMLToken::Type::Character;
}

inline bool HTMLTokenizer::processEntity(SegmentedString& source)
{
    auto decodedEntity = consumeHTMLEntity(source);
    if (decodedEntity.notEnoughCharacters())
        return false;
    if (decodedEntity.failed())
        bufferASCIICharacter('&');
    else
        bufferCharacters(decodedEntity.span());
    return true;
}

void HTMLTokenizer::flushBufferedEndTag()
{
    m_token.beginEndTag(m_bufferedEndTagName);
    m_bufferedEndTagName.clear();
    m_appropriateEndTagName.clear();
    m_temporaryBuffer.clear();
}

bool HTMLTokenizer::commitToPartialEndTag(SegmentedString& source, char16_t character, State state)
{
    ASSERT(source.currentCharacter() == character);
    appendToTemporaryBuffer(character);
    source.advance();

    if (haveBufferedCharacterToken()) {
        // Emit the buffered character token.
        // The next call to processToken will flush the buffered end tag and continue parsing it.
        m_state = state;
        return true;
    }

    flushBufferedEndTag();
    return false;
}

bool HTMLTokenizer::commitToCompleteEndTag(SegmentedString& source)
{
    ASSERT(source.currentCharacter() == '>');
    appendToTemporaryBuffer('>');
    source.advancePastNonNewline();

    m_state = DataState;

    if (haveBufferedCharacterToken()) {
        // Emit the character token we already have.
        // The next call to processToken will flush the buffered end tag and emit it.
        return true;
    }

    flushBufferedEndTag();
    return true;
}

bool HTMLTokenizer::processToken(SegmentedString& source)
{
    if (!m_bufferedEndTagName.isEmpty() && !inEndTagBufferingState()) {
        // We are back here after emitting a character token that came just before an end tag.
        // To continue parsing the end tag we need to move the buffered tag name into the token.
        flushBufferedEndTag();

        // If we are in the data state, the end tag is already complete and we should emit it
        // now, otherwise, we want to resume parsing the partial end tag.
        if (m_state == DataState)
            return true;
    }

    if (!m_preprocessor.peek(source, isNullCharacterSkippingState(m_state) && !m_forceNullCharacterReplacement))
        return haveBufferedCharacterToken();
    char16_t character = m_preprocessor.nextInputCharacter();

    // https://html.spec.whatwg.org/#tokenization
    switch (m_state) {

    BEGIN_STATE(DataState)
        if (character == '&')
            ADVANCE_PAST_NON_NEWLINE_TO(CharacterReferenceInDataState);
        if (character == '<') {
            if (haveBufferedCharacterToken())
                RETURN_IN_CURRENT_STATE(true);
            ADVANCE_PAST_NON_NEWLINE_TO(TagOpenState);
        }
        if (character == kEndOfFileMarker)
            return emitEndOfFile(source);
        bufferCharacter(character);
        BATCH_PLAIN_TEXT(findPlainTextInDataState, bufferCharacters, DataState);
        ADVANCE_TO(DataState);
    END_STATE()

    BEGIN_STATE(CharacterReferenceInDataState)
        if (!processEntity(source))
            RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        SWITCH_TO(DataState);
    END_STATE()

    BEGIN_STATE(RCDATAState)
        if (character == '&')
            ADVANCE_PAST_NON_NEWLINE_TO(CharacterReferenceInRCDATAState);
        if (character == '<')
            ADVANCE_PAST_NON_NEWLINE_TO(RCDATALessThanSignState);
        if (character == kEndOfFileMarker)
            RECONSUME_IN(DataState);
        bufferCharacter(character);
        // RCDATA reacts to exactly the same characters as the Data state, so it shares that scan.
        BATCH_PLAIN_TEXT(findPlainTextInDataState, bufferCharacters, RCDATAState);
        ADVANCE_TO(RCDATAState);
    END_STATE()

    BEGIN_STATE(CharacterReferenceInRCDATAState)
        if (!processEntity(source))
            RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        SWITCH_TO(RCDATAState);
    END_STATE()

    BEGIN_STATE(RAWTEXTState)
        if (character == '<')
            ADVANCE_PAST_NON_NEWLINE_TO(RAWTEXTLessThanSignState);
        if (character == kEndOfFileMarker)
            RECONSUME_IN(DataState);
        bufferCharacter(character);
        BATCH_PLAIN_TEXT(findPlainTextInRawText, bufferCharacters, RAWTEXTState);
        ADVANCE_TO(RAWTEXTState);
    END_STATE()

    BEGIN_STATE(ScriptDataState)
        if (character == '<')
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataLessThanSignState);
        if (character == kEndOfFileMarker)
            RECONSUME_IN(DataState);
        bufferCharacter(character);
        BATCH_PLAIN_TEXT(findPlainTextInRawText, bufferCharacters, ScriptDataState);
        ADVANCE_TO(ScriptDataState);
    END_STATE()

    BEGIN_STATE(PLAINTEXTState)
        if (character == kEndOfFileMarker)
            RECONSUME_IN(DataState);
        bufferCharacter(character);
        ADVANCE_TO(PLAINTEXTState);
    END_STATE()

    BEGIN_STATE(TagOpenState)
        if (character == '!')
            ADVANCE_PAST_NON_NEWLINE_TO(MarkupDeclarationOpenState);
        if (character == '/')
            ADVANCE_PAST_NON_NEWLINE_TO(EndTagOpenState);
        if (isASCIIAlpha(character)) {
            m_token.beginStartTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(TagNameState);
        }
        if (character == '?') {
            parseError();
            // The spec consumes the current character before switching
            // to the bogus comment state, but it's easier to implement
            // if we reconsume the current character.
            RECONSUME_IN(BogusCommentState);
        }
        parseError();
        bufferASCIICharacter('<');
        RECONSUME_IN(DataState);
    END_STATE()

    BEGIN_STATE(EndTagOpenState)
        if (isASCIIAlpha(character)) {
            m_token.beginEndTag(convertASCIIAlphaToLower(character));
            m_appropriateEndTagName.clear();
            ADVANCE_PAST_NON_NEWLINE_TO(TagNameState);
        }
        if (character == '>') {
            parseError();
            ADVANCE_PAST_NON_NEWLINE_TO(DataState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            bufferCharacters("</"_s);
            RECONSUME_IN(DataState);
        }
        parseError();
        RECONSUME_IN(BogusCommentState);
    END_STATE()

    BEGIN_STATE(TagNameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeAttributeNameState);
        if (character == '/')
            ADVANCE_PAST_NON_NEWLINE_TO(SelfClosingStartTagState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (m_options.usePreHTML5ParserQuirks && character == '<')
            return emitAndReconsumeInDataState();
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        m_token.appendToName(toASCIILower(character));
        BATCH_PLAIN_TEXT(findPlainTextInTagName, m_token.appendToName, TagNameState);
        ADVANCE_PAST_NON_NEWLINE_TO(TagNameState);
    END_STATE()

    BEGIN_STATE(RCDATALessThanSignState)
        if (character == '/') {
            m_temporaryBuffer.clear();
            ASSERT(m_bufferedEndTagName.isEmpty());
            ADVANCE_PAST_NON_NEWLINE_TO(RCDATAEndTagOpenState);
        }
        bufferASCIICharacter('<');
        RECONSUME_IN(RCDATAState);
    END_STATE()

    BEGIN_STATE(RCDATAEndTagOpenState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(RCDATAEndTagNameState);
        }
        bufferCharacters("</"_s);
        RECONSUME_IN(RCDATAState);
    END_STATE()

    BEGIN_STATE(RCDATAEndTagNameState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(RCDATAEndTagNameState);
        }
        if (isTokenizerWhitespace(character)) {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, character, BeforeAttributeNameState))
                    return true;
                SWITCH_TO(BeforeAttributeNameState);
            }
        } else if (character == '/') {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, '/', SelfClosingStartTagState))
                    return true;
                SWITCH_TO(SelfClosingStartTagState);
            }
        } else if (character == '>') {
            if (isAppropriateEndTag())
                return commitToCompleteEndTag(source);
        }
        bufferCharacters("</"_s);
        m_token.appendToCharacter(m_temporaryBuffer);
        m_bufferedEndTagName.clear();
        m_temporaryBuffer.clear();
        RECONSUME_IN(RCDATAState);
    END_STATE()

    BEGIN_STATE(RAWTEXTLessThanSignState)
        if (character == '/') {
            m_temporaryBuffer.clear();
            ASSERT(m_bufferedEndTagName.isEmpty());
            ADVANCE_PAST_NON_NEWLINE_TO(RAWTEXTEndTagOpenState);
        }
        bufferASCIICharacter('<');
        RECONSUME_IN(RAWTEXTState);
    END_STATE()

    BEGIN_STATE(RAWTEXTEndTagOpenState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(RAWTEXTEndTagNameState);
        }
        bufferCharacters("</"_s);
        RECONSUME_IN(RAWTEXTState);
    END_STATE()

    BEGIN_STATE(RAWTEXTEndTagNameState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(RAWTEXTEndTagNameState);
        }
        if (isTokenizerWhitespace(character)) {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, character, BeforeAttributeNameState))
                    return true;
                SWITCH_TO(BeforeAttributeNameState);
            }
        } else if (character == '/') {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, '/', SelfClosingStartTagState))
                    return true;
                SWITCH_TO(SelfClosingStartTagState);
            }
        } else if (character == '>') {
            if (isAppropriateEndTag())
                return commitToCompleteEndTag(source);
        }
        bufferCharacters("</"_s);
        m_token.appendToCharacter(m_temporaryBuffer);
        m_bufferedEndTagName.clear();
        m_temporaryBuffer.clear();
        RECONSUME_IN(RAWTEXTState);
    END_STATE()

    BEGIN_STATE(ScriptDataLessThanSignState)
        if (character == '/') {
            m_temporaryBuffer.clear();
            ASSERT(m_bufferedEndTagName.isEmpty());
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEndTagOpenState);
        }
        if (character == '!') {
            bufferCharacters("<!"_s);
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapeStartState);
        }
        bufferASCIICharacter('<');
        RECONSUME_IN(ScriptDataState);
    END_STATE()

    BEGIN_STATE(ScriptDataEndTagOpenState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEndTagNameState);
        }
        bufferCharacters("</"_s);
        RECONSUME_IN(ScriptDataState);
    END_STATE()

    BEGIN_STATE(ScriptDataEndTagNameState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEndTagNameState);
        }
        if (isTokenizerWhitespace(character)) {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, character, BeforeAttributeNameState))
                    return true;
                SWITCH_TO(BeforeAttributeNameState);
            }
        } else if (character == '/') {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, '/', SelfClosingStartTagState))
                    return true;
                SWITCH_TO(SelfClosingStartTagState);
            }
        } else if (character == '>') {
            if (isAppropriateEndTag())
                return commitToCompleteEndTag(source);
        }
        bufferCharacters("</"_s);
        m_token.appendToCharacter(m_temporaryBuffer);
        m_bufferedEndTagName.clear();
        m_temporaryBuffer.clear();
        RECONSUME_IN(ScriptDataState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapeStartState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapeStartDashState);
        } else
            RECONSUME_IN(ScriptDataState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapeStartDashState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedDashDashState);
        } else
            RECONSUME_IN(ScriptDataState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapedState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedDashState);
        }
        if (character == '<')
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedLessThanSignState);
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        bufferCharacter(character);
        BATCH_PLAIN_TEXT(findPlainTextInEscapedScriptData, bufferCharacters, ScriptDataEscapedState);
        ADVANCE_TO(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapedDashState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedDashDashState);
        }
        if (character == '<')
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedLessThanSignState);
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        bufferCharacter(character);
        ADVANCE_TO(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapedDashDashState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedDashDashState);
        }
        if (character == '<')
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedLessThanSignState);
        if (character == '>') {
            bufferASCIICharacter('>');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        bufferCharacter(character);
        ADVANCE_TO(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapedLessThanSignState)
        if (character == '/') {
            m_temporaryBuffer.clear();
            ASSERT(m_bufferedEndTagName.isEmpty());
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedEndTagOpenState);
        }
        if (isASCIIAlpha(character)) {
            bufferASCIICharacter('<');
            bufferASCIICharacter(character);
            m_temporaryBuffer.clear();
            appendToTemporaryBuffer(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapeStartState);
        }
        bufferASCIICharacter('<');
        RECONSUME_IN(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapedEndTagOpenState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedEndTagNameState);
        }
        bufferCharacters("</"_s);
        RECONSUME_IN(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataEscapedEndTagNameState)
        if (isASCIIAlpha(character)) {
            appendToTemporaryBuffer(character);
            appendToPossibleEndTag(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataEscapedEndTagNameState);
        }
        if (isTokenizerWhitespace(character)) {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, character, BeforeAttributeNameState))
                    return true;
                SWITCH_TO(BeforeAttributeNameState);
            }
        } else if (character == '/') {
            if (isAppropriateEndTag()) {
                if (commitToPartialEndTag(source, '/', SelfClosingStartTagState))
                    return true;
                SWITCH_TO(SelfClosingStartTagState);
            }
        } else if (character == '>') {
            if (isAppropriateEndTag())
                return commitToCompleteEndTag(source);
        }
        bufferCharacters("</"_s);
        m_token.appendToCharacter(m_temporaryBuffer);
        m_bufferedEndTagName.clear();
        m_temporaryBuffer.clear();
        RECONSUME_IN(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataDoubleEscapeStartState)
        if (isTokenizerWhitespace(character) || character == '/' || character == '>') {
            bufferASCIICharacter(character);
            if (temporaryBufferIs("script"_s))
                ADVANCE_TO(ScriptDataDoubleEscapedState);
            else
                ADVANCE_TO(ScriptDataEscapedState);
        }
        if (isASCIIAlpha(character)) {
            bufferASCIICharacter(character);
            appendToTemporaryBuffer(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapeStartState);
        }
        RECONSUME_IN(ScriptDataEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataDoubleEscapedState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapedDashState);
        }
        if (character == '<') {
            bufferASCIICharacter('<');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapedLessThanSignState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        bufferCharacter(character);
        BATCH_PLAIN_TEXT(findPlainTextInEscapedScriptData, bufferCharacters, ScriptDataDoubleEscapedState);
        ADVANCE_TO(ScriptDataDoubleEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataDoubleEscapedDashState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapedDashDashState);
        }
        if (character == '<') {
            bufferASCIICharacter('<');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapedLessThanSignState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        bufferCharacter(character);
        ADVANCE_TO(ScriptDataDoubleEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataDoubleEscapedDashDashState)
        if (character == '-') {
            bufferASCIICharacter('-');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapedDashDashState);
        }
        if (character == '<') {
            bufferASCIICharacter('<');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapedLessThanSignState);
        }
        if (character == '>') {
            bufferASCIICharacter('>');
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        bufferCharacter(character);
        ADVANCE_TO(ScriptDataDoubleEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataDoubleEscapedLessThanSignState)
        if (character == '/') {
            bufferASCIICharacter('/');
            m_temporaryBuffer.clear();
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapeEndState);
        }
        RECONSUME_IN(ScriptDataDoubleEscapedState);
    END_STATE()

    BEGIN_STATE(ScriptDataDoubleEscapeEndState)
        if (isTokenizerWhitespace(character) || character == '/' || character == '>') {
            bufferASCIICharacter(character);
            if (temporaryBufferIs("script"_s))
                ADVANCE_TO(ScriptDataEscapedState);
            else
                ADVANCE_TO(ScriptDataDoubleEscapedState);
        }
        if (isASCIIAlpha(character)) {
            bufferASCIICharacter(character);
            appendToTemporaryBuffer(convertASCIIAlphaToLower(character));
            ADVANCE_PAST_NON_NEWLINE_TO(ScriptDataDoubleEscapeEndState);
        }
        RECONSUME_IN(ScriptDataDoubleEscapedState);
    END_STATE()

    BEGIN_STATE(BeforeAttributeNameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeAttributeNameState);
        if (character == '/')
            ADVANCE_PAST_NON_NEWLINE_TO(SelfClosingStartTagState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (m_options.usePreHTML5ParserQuirks && character == '<')
            return emitAndReconsumeInDataState();
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        if (character == '"' || character == '\'' || character == '<' || character == '=')
            parseError();
        m_token.beginAttribute();
        m_token.appendToAttributeName(toASCIILower(character));
        BATCH_PLAIN_TEXT(findPlainTextInAttributeName, m_token.appendToAttributeName, AttributeNameState);
        ADVANCE_PAST_NON_NEWLINE_TO(AttributeNameState);
    END_STATE()

    BEGIN_STATE(AttributeNameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(AfterAttributeNameState);
        if (character == '/')
            ADVANCE_PAST_NON_NEWLINE_TO(SelfClosingStartTagState);
        if (character == '=')
            ADVANCE_PAST_NON_NEWLINE_TO(BeforeAttributeValueState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (m_options.usePreHTML5ParserQuirks && character == '<')
            return emitAndReconsumeInDataState();
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        if (character == '"' || character == '\'' || character == '<' || character == '=')
            parseError();
        m_token.appendToAttributeName(toASCIILower(character));
        BATCH_PLAIN_TEXT(findPlainTextInAttributeName, m_token.appendToAttributeName, AttributeNameState);
        ADVANCE_PAST_NON_NEWLINE_TO(AttributeNameState);
    END_STATE()

    BEGIN_STATE(AfterAttributeNameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(AfterAttributeNameState);
        if (character == '/')
            ADVANCE_PAST_NON_NEWLINE_TO(SelfClosingStartTagState);
        if (character == '=')
            ADVANCE_PAST_NON_NEWLINE_TO(BeforeAttributeValueState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (m_options.usePreHTML5ParserQuirks && character == '<')
            return emitAndReconsumeInDataState();
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        if (character == '"' || character == '\'' || character == '<')
            parseError();
        m_token.beginAttribute();
        m_token.appendToAttributeName(toASCIILower(character));
        BATCH_PLAIN_TEXT(findPlainTextInAttributeName, m_token.appendToAttributeName, AttributeNameState);
        ADVANCE_PAST_NON_NEWLINE_TO(AttributeNameState);
    END_STATE()

    BEGIN_STATE(BeforeAttributeValueState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeAttributeValueState);
        if (character == '"')
            ADVANCE_PAST_NON_NEWLINE_TO(AttributeValueDoubleQuotedState);
        if (character == '&')
            RECONSUME_IN(AttributeValueUnquotedState);
        if (character == '\'')
            ADVANCE_PAST_NON_NEWLINE_TO(AttributeValueSingleQuotedState);
        if (character == '>') {
            parseError();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        if (character == '<' || character == '=' || character == '`')
            parseError();
        m_token.appendToAttributeValue(character);
        ADVANCE_PAST_NON_NEWLINE_TO(AttributeValueUnquotedState);
    END_STATE()

    BEGIN_STATE(AttributeValueDoubleQuotedState)
        if (character == '"') {
            m_token.endAttribute();
            ADVANCE_PAST_NON_NEWLINE_TO(AfterAttributeValueQuotedState);
        }
        if (character == '&') {
            m_additionalAllowedCharacter = '"';
            ADVANCE_PAST_NON_NEWLINE_TO(CharacterReferenceInAttributeValueState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.endAttribute();
            RECONSUME_IN(DataState);
        }
        m_token.appendToAttributeValue(character);
        BATCH_PLAIN_TEXT(findPlainTextInQuotedAttributeValue<'"'>, m_token.appendToAttributeValue, AttributeValueDoubleQuotedState);
        ADVANCE_TO(AttributeValueDoubleQuotedState);
    END_STATE()

    BEGIN_STATE(AttributeValueSingleQuotedState)
        if (character == '\'') {
            m_token.endAttribute();
            ADVANCE_PAST_NON_NEWLINE_TO(AfterAttributeValueQuotedState);
        }
        if (character == '&') {
            m_additionalAllowedCharacter = '\'';
            ADVANCE_PAST_NON_NEWLINE_TO(CharacterReferenceInAttributeValueState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.endAttribute();
            RECONSUME_IN(DataState);
        }
        m_token.appendToAttributeValue(character);
        BATCH_PLAIN_TEXT(findPlainTextInQuotedAttributeValue<'\''>, m_token.appendToAttributeValue, AttributeValueSingleQuotedState);
        ADVANCE_TO(AttributeValueSingleQuotedState);
    END_STATE()

    BEGIN_STATE(AttributeValueUnquotedState)
        if (isTokenizerWhitespace(character)) {
            m_token.endAttribute();
            ADVANCE_TO(BeforeAttributeNameState);
        }
        if (character == '&') {
            m_additionalAllowedCharacter = '>';
            ADVANCE_PAST_NON_NEWLINE_TO(CharacterReferenceInAttributeValueState);
        }
        if (character == '>') {
            m_token.endAttribute();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.endAttribute();
            RECONSUME_IN(DataState);
        }
        if (character == '"' || character == '\'' || character == '<' || character == '=' || character == '`')
            parseError();
        m_token.appendToAttributeValue(character);
        BATCH_PLAIN_TEXT(findPlainTextInUnquotedAttributeValue, m_token.appendToAttributeValue, AttributeValueUnquotedState);
        ADVANCE_PAST_NON_NEWLINE_TO(AttributeValueUnquotedState);
    END_STATE()

    BEGIN_STATE(CharacterReferenceInAttributeValueState)
        auto decodedEntity = consumeHTMLEntity(source, m_additionalAllowedCharacter);
        if (decodedEntity.notEnoughCharacters())
            RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        if (decodedEntity.failed())
            m_token.appendToAttributeValue('&');
        else
            m_token.appendToAttributeValue(decodedEntity.span());
        // We're supposed to switch back to the attribute value state that
        // we were in when we were switched into this state. Rather than
        // keeping track of this explicitly, we observe that the previous
        // state can be determined by m_additionalAllowedCharacter.
        if (m_additionalAllowedCharacter == '"')
            SWITCH_TO(AttributeValueDoubleQuotedState);
        if (m_additionalAllowedCharacter == '\'')
            SWITCH_TO(AttributeValueSingleQuotedState);
        ASSERT(m_additionalAllowedCharacter == '>');
        SWITCH_TO(AttributeValueUnquotedState);
    END_STATE()

    BEGIN_STATE(AfterAttributeValueQuotedState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeAttributeNameState);
        if (character == '/')
            ADVANCE_PAST_NON_NEWLINE_TO(SelfClosingStartTagState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (m_options.usePreHTML5ParserQuirks && character == '<')
            return emitAndReconsumeInDataState();
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        parseError();
        RECONSUME_IN(BeforeAttributeNameState);
    END_STATE()

    BEGIN_STATE(SelfClosingStartTagState)
        if (character == '>') {
            m_token.setSelfClosing();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            RECONSUME_IN(DataState);
        }
        parseError();
        RECONSUME_IN(BeforeAttributeNameState);
    END_STATE()

    BEGIN_STATE(BogusCommentState)
        m_token.beginComment();
        RECONSUME_IN(ContinueBogusCommentState);
    END_STATE()

    BEGIN_STATE(ContinueBogusCommentState)
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == kEndOfFileMarker)
            return emitAndReconsumeInDataState();
        m_token.appendToComment(character);
        ADVANCE_TO(ContinueBogusCommentState);
    END_STATE()

    BEGIN_STATE(MarkupDeclarationOpenState)
        if (character == '-') {
            auto result = source.advancePast("--"_s);
            if (result == SegmentedString::DidMatch) {
                m_token.beginComment();
                SWITCH_TO(CommentStartState);
            }
            if (result == SegmentedString::NotEnoughCharacters)
                RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        } else if (isASCIIAlphaCaselessEqual(character, 'd')) {
            auto result = source.advancePastLettersIgnoringASCIICase("doctype"_s);
            if (result == SegmentedString::DidMatch)
                SWITCH_TO(DOCTYPEState);
            if (result == SegmentedString::NotEnoughCharacters)
                RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        } else if (character == '[' && shouldAllowCDATA()) {
            auto result = source.advancePast("[CDATA["_s);
            if (result == SegmentedString::DidMatch)
                SWITCH_TO(CDATASectionState);
            if (result == SegmentedString::NotEnoughCharacters)
                RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        }
        parseError();
        RECONSUME_IN(BogusCommentState);
    END_STATE()

    BEGIN_STATE(CommentStartState)
        if (character == '-')
            ADVANCE_PAST_NON_NEWLINE_TO(CommentStartDashState);
        if (character == '>') {
            parseError();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToComment(character);
        ADVANCE_TO(CommentState);
    END_STATE()

    BEGIN_STATE(CommentStartDashState)
        if (character == '-')
            ADVANCE_PAST_NON_NEWLINE_TO(CommentEndState);
        if (character == '>') {
            parseError();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToComment('-');
        m_token.appendToComment(character);
        ADVANCE_TO(CommentState);
    END_STATE()

    BEGIN_STATE(CommentState)
        if (character == '-')
            ADVANCE_PAST_NON_NEWLINE_TO(CommentEndDashState);
        if (character == kEndOfFileMarker) {
            parseError();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToComment(character);
        BATCH_PLAIN_TEXT(findPlainTextInComment, m_token.appendToComment, CommentState);
        ADVANCE_TO(CommentState);
    END_STATE()

    BEGIN_STATE(CommentEndDashState)
        if (character == '-')
            ADVANCE_PAST_NON_NEWLINE_TO(CommentEndState);
        if (character == kEndOfFileMarker) {
            parseError();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToComment('-');
        m_token.appendToComment(character);
        ADVANCE_TO(CommentState);
    END_STATE()

    BEGIN_STATE(CommentEndState)
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == '!') {
            parseError();
            ADVANCE_PAST_NON_NEWLINE_TO(CommentEndBangState);
        }
        if (character == '-') {
            parseError();
            m_token.appendToComment('-');
            ADVANCE_PAST_NON_NEWLINE_TO(CommentEndState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.appendToComment("--"_s);
        m_token.appendToComment(character);
        ADVANCE_TO(CommentState);
    END_STATE()

    BEGIN_STATE(CommentEndBangState)
        if (character == '-') {
            m_token.appendToComment("--!"_s);
            ADVANCE_PAST_NON_NEWLINE_TO(CommentEndDashState);
        }
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == kEndOfFileMarker) {
            parseError();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToComment("--!"_s);
        m_token.appendToComment(character);
        ADVANCE_TO(CommentState);
    END_STATE()

    BEGIN_STATE(DOCTYPEState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeDOCTYPENameState);
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.beginDOCTYPE();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        RECONSUME_IN(BeforeDOCTYPENameState);
    END_STATE()

    BEGIN_STATE(BeforeDOCTYPENameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeDOCTYPENameState);
        if (character == '>') {
            parseError();
            m_token.beginDOCTYPE();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.beginDOCTYPE();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        m_token.beginDOCTYPE(toASCIILower(character));
        ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPENameState);
    END_STATE()

    BEGIN_STATE(DOCTYPENameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(AfterDOCTYPENameState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToName(toASCIILower(character));
        ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPENameState);
    END_STATE()

    BEGIN_STATE(AfterDOCTYPENameState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(AfterDOCTYPENameState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        if (isASCIIAlphaCaselessEqual(character, 'p')) {
            auto result = source.advancePastLettersIgnoringASCIICase("public"_s);
            if (result == SegmentedString::DidMatch)
                SWITCH_TO(AfterDOCTYPEPublicKeywordState);
            if (result == SegmentedString::NotEnoughCharacters)
                RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        } else if (isASCIIAlphaCaselessEqual(character, 's')) {
            auto result = source.advancePastLettersIgnoringASCIICase("system"_s);
            if (result == SegmentedString::DidMatch)
                SWITCH_TO(AfterDOCTYPESystemKeywordState);
            if (result == SegmentedString::NotEnoughCharacters)
                RETURN_IN_CURRENT_STATE(haveBufferedCharacterToken());
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(AfterDOCTYPEPublicKeywordState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeDOCTYPEPublicIdentifierState);
        if (character == '"') {
            parseError();
            m_token.setPublicIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPEPublicIdentifierDoubleQuotedState);
        }
        if (character == '\'') {
            parseError();
            m_token.setPublicIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPEPublicIdentifierSingleQuotedState);
        }
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(BeforeDOCTYPEPublicIdentifierState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeDOCTYPEPublicIdentifierState);
        if (character == '"') {
            m_token.setPublicIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPEPublicIdentifierDoubleQuotedState);
        }
        if (character == '\'') {
            m_token.setPublicIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPEPublicIdentifierSingleQuotedState);
        }
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(DOCTYPEPublicIdentifierDoubleQuotedState)
        if (character == '"')
            ADVANCE_PAST_NON_NEWLINE_TO(AfterDOCTYPEPublicIdentifierState);
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToPublicIdentifier(character);
        ADVANCE_TO(DOCTYPEPublicIdentifierDoubleQuotedState);
    END_STATE()

    BEGIN_STATE(DOCTYPEPublicIdentifierSingleQuotedState)
        if (character == '\'')
            ADVANCE_PAST_NON_NEWLINE_TO(AfterDOCTYPEPublicIdentifierState);
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToPublicIdentifier(character);
        ADVANCE_TO(DOCTYPEPublicIdentifierSingleQuotedState);
    END_STATE()

    BEGIN_STATE(AfterDOCTYPEPublicIdentifierState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BetweenDOCTYPEPublicAndSystemIdentifiersState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == '"') {
            parseError();
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierDoubleQuotedState);
        }
        if (character == '\'') {
            parseError();
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierSingleQuotedState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(BetweenDOCTYPEPublicAndSystemIdentifiersState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BetweenDOCTYPEPublicAndSystemIdentifiersState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == '"') {
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierDoubleQuotedState);
        }
        if (character == '\'') {
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierSingleQuotedState);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(AfterDOCTYPESystemKeywordState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeDOCTYPESystemIdentifierState);
        if (character == '"') {
            parseError();
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierDoubleQuotedState);
        }
        if (character == '\'') {
            parseError();
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierSingleQuotedState);
        }
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(BeforeDOCTYPESystemIdentifierState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(BeforeDOCTYPESystemIdentifierState);
        if (character == '"') {
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierDoubleQuotedState);
        }
        if (character == '\'') {
            m_token.setSystemIdentifierToEmptyString();
            ADVANCE_PAST_NON_NEWLINE_TO(DOCTYPESystemIdentifierSingleQuotedState);
        }
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        m_token.setForceQuirks();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(DOCTYPESystemIdentifierDoubleQuotedState)
        if (character == '"')
            ADVANCE_PAST_NON_NEWLINE_TO(AfterDOCTYPESystemIdentifierState);
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToSystemIdentifier(character);
        ADVANCE_TO(DOCTYPESystemIdentifierDoubleQuotedState);
    END_STATE()

    BEGIN_STATE(DOCTYPESystemIdentifierSingleQuotedState)
        if (character == '\'')
            ADVANCE_PAST_NON_NEWLINE_TO(AfterDOCTYPESystemIdentifierState);
        if (character == '>') {
            parseError();
            m_token.setForceQuirks();
            return emitAndResumeInDataState(source);
        }
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        m_token.appendToSystemIdentifier(character);
        ADVANCE_TO(DOCTYPESystemIdentifierSingleQuotedState);
    END_STATE()

    BEGIN_STATE(AfterDOCTYPESystemIdentifierState)
        if (isTokenizerWhitespace(character))
            ADVANCE_TO(AfterDOCTYPESystemIdentifierState);
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == kEndOfFileMarker) {
            parseError();
            m_token.setForceQuirks();
            return emitAndReconsumeInDataState();
        }
        parseError();
        ADVANCE_PAST_NON_NEWLINE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(BogusDOCTYPEState)
        if (character == '>')
            return emitAndResumeInDataState(source);
        if (character == kEndOfFileMarker)
            return emitAndReconsumeInDataState();
        ADVANCE_TO(BogusDOCTYPEState);
    END_STATE()

    BEGIN_STATE(CDATASectionState)
        if (character == ']')
            ADVANCE_PAST_NON_NEWLINE_TO(CDATASectionRightSquareBracketState);
        if (character == kEndOfFileMarker)
            RECONSUME_IN(DataState);
        bufferCharacter(character);
        ADVANCE_TO(CDATASectionState);
    END_STATE()

    BEGIN_STATE(CDATASectionRightSquareBracketState)
        if (character == ']')
            ADVANCE_PAST_NON_NEWLINE_TO(CDATASectionDoubleRightSquareBracketState);
        bufferASCIICharacter(']');
        RECONSUME_IN(CDATASectionState);
    END_STATE()

    BEGIN_STATE(CDATASectionDoubleRightSquareBracketState)
        if (character == ']') {
            bufferCharacter(character);
            ADVANCE_TO(CDATASectionDoubleRightSquareBracketState);
        }
        if (character == '>')
            ADVANCE_PAST_NON_NEWLINE_TO(DataState);
        bufferCharacters("]]"_s);
        RECONSUME_IN(CDATASectionState);
    END_STATE()

    }

    ASSERT_NOT_REACHED();
    return false;
}

#undef BATCH_PLAIN_TEXT

String HTMLTokenizer::bufferedCharacters() const
{
    // FIXME: Add an assert about m_state.
    return makeString("</"_s, m_temporaryBuffer);
}

void HTMLTokenizer::updateStateFor(const AtomString& tagName)
{
    if (textareaTag->hasLocalName(tagName) || titleTag->hasLocalName(tagName))
        m_state = RCDATAState;
    else if (plaintextTag->hasLocalName(tagName))
        m_state = PLAINTEXTState;
    else if (scriptTag->hasLocalName(tagName))
        m_state = ScriptDataState;
    else if (styleTag->hasLocalName(tagName)
        || iframeTag->hasLocalName(tagName)
        || xmpTag->hasLocalName(tagName)
        || noembedTag->hasLocalName(tagName)
        || noframesTag->hasLocalName(tagName)
        || (noscriptTag->hasLocalName(tagName) && m_options.scriptingFlag))
        m_state = RAWTEXTState;
}

void HTMLTokenizer::updateStateFor(std::span<const char16_t> tagName)
{
    // xmp, title, style, script, iframe, noembed, textarea, noframes, noscript, plaintext.
    if (tagName.size() < 3 || tagName.size() > 9)
        return;
    switch (tagName.front()) {
    case 'i':
    case 'n':
    case 'p':
    case 's':
    case 't':
    case 'x':
        break;
    default:
        return;
    }
    updateStateFor(AtomString::lookUp(tagName));
}

inline void HTMLTokenizer::appendToTemporaryBuffer(char16_t character)
{
    ASSERT(isASCII(character));
    m_temporaryBuffer.append(character);
}

inline bool HTMLTokenizer::temporaryBufferIs(ASCIILiteral expectedString)
{
    if (m_temporaryBuffer.size() != expectedString.length())
        return false;
    return equal(m_temporaryBuffer.span().data(), expectedString.span8());
}

inline void HTMLTokenizer::appendToPossibleEndTag(char16_t character)
{
    ASSERT(isASCII(character));
    m_bufferedEndTagName.append(character);
}

inline bool HTMLTokenizer::isAppropriateEndTag() const
{
    if (m_bufferedEndTagName.size() != m_appropriateEndTagName.size())
        return false;
    return equal(m_bufferedEndTagName.span().data(), m_appropriateEndTagName.span());
}

inline void HTMLTokenizer::parseError()
{
}

}
