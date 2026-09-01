/*
 * Copyright (C) 2005-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution. 
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef WebNSStringExtrasIOS6_h
#define WebNSStringExtrasIOS6_h

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

typedef struct __GSFont *GSFontRef;

typedef enum {
    WebEllipsisStyleNone = 0,
    WebEllipsisStyleHead = 1,
    WebEllipsisStyleCenter = 2,
    WebEllipsisStyleTail = 3,
    WebEllipsisStyleClip = 4,
    WebEllipsisStyleWordWrap = 5,
    WebEllipsisStyleCharacterWrap = 6,
} WebEllipsisStyle;

typedef enum {
    WebTextAlignmentLeft = 0,
    WebTextAlignmentCenter = 1,
    WebTextAlignmentRight = 2,
} WebTextAlignment;

@interface NSString (WebNSStringExtrasIOS6)

+ (BOOL)_web_wordRoundingAllowed;
+ (void)_web_setWordRoundingAllowed:(BOOL)allowed;
+ (BOOL)_web_wordRoundingEnabled;
+ (void)_web_setWordRoundingEnabled:(BOOL)enabled;
+ (BOOL)_web_ascentRoundingEnabled;
+ (void)_web_setAscentRoundingEnabled:(BOOL)enabled;
+ (NSString *)_web_stringWithData:(NSData *)data textEncodingName:(NSString *)encodingName;

- (CGSize)_web_sizeWithFont:(GSFontRef)font;
- (CGSize)_web_sizeWithFont:(GSFontRef)font forWidth:(CGFloat)width ellipsis:(WebEllipsisStyle)ellipsis;
- (CGSize)_web_sizeWithFont:(GSFontRef)font forWidth:(CGFloat)width ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing;
- (CGSize)_web_sizeWithFont:(GSFontRef)font forWidth:(CGFloat)width ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing resultRange:(NSRange *)resultRange;
- (CGSize)_web_sizeForWidth:(CGFloat)width withAttributes:(NSDictionary *)attributes;
- (CGSize)_web_sizeInRect:(CGRect)rect withAttributes:(NSDictionary *)attributes;
- (CGSize)_web_sizeInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis;
- (CGSize)_web_sizeInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis lineSpacing:(int)lineSpacing;

- (CGSize)_web_drawAtPoint:(CGPoint)point withFont:(GSFontRef)font;
- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withAttributes:(NSDictionary *)attributes;
- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis;
- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing;
- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji;
- (CGSize)__web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji measureOnly:(BOOL)measureOnly;
- (CGSize)__web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut;
- (CGSize)__web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut drawUnderline:(BOOL)drawUnderline;

- (CGSize)_web_drawInRect:(CGRect)rect withAttributes:(NSDictionary *)attributes;
- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment;
- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment lineSpacing:(int)lineSpacing;
- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment lineSpacing:(int)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect;
- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment lineSpacing:(int)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly;
- (CGSize)__web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment letterSpacing:(CGFloat)letterSpacing lineSpacing:(CGFloat)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly;
- (CGSize)__web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment letterSpacing:(CGFloat)letterSpacing lineSpacing:(CGFloat)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut;
- (CGSize)__web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment letterSpacing:(CGFloat)letterSpacing lineSpacing:(CGFloat)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut drawUnderline:(BOOL)drawUnderline;

- (NSString *)_web_stringForWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji;

- (BOOL)LS_hasCaseInsensitivePrefix:(NSString *)prefix;
- (NSString *)LS_unescapedQueryValue;
- (NSString *)_web_capitalizeRFC822HeaderFieldName;
- (NSString *)_web_decodeHostNameWithRange:(NSRange)range;
- (NSString *)_web_encodeHostNameWithRange:(NSRange)range;
- (BOOL)_web_hostNameNeedsDecodingWithRange:(NSRange)range;
- (BOOL)_web_hostNameNeedsEncodingWithRange:(NSRange)range;
- (NSString *)_web_securedStringIncludingLastCharacter:(BOOL)includingLastCharacter;
- (NSString *)_web_stringByStrippingReturnCharacters;
- (NSString *)_webkit_URLFragment;
- (BOOL)_webkit_hasCaseInsensitiveSubstring:(NSString *)substring;
- (BOOL)_webkit_isFTPDirectoryURL;
- (NSDictionary *)_webkit_queryKeysAndValues;
- (NSDictionary *)queryToDict;
- (NSString *)_webkit_stringByCollapsingNonPrintingCharacters;
- (NSString *)_webkit_stringByCollapsingWhitespaceCharacters;
- (NSString *)_webkit_unescapedQueryValue;

@end

#endif // WebNSStringExtrasIOS6_h
