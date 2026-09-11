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

#import "config.h"

#ifdef IOS6_CLASS_PREFIX_H
#define _web_drawAtPoint Rev_web_drawAtPoint
#define __web_drawAtPoint Rev__web_drawAtPoint
#define _web_drawInRect Rev_web_drawInRect
#define __web_drawInRect Rev__web_drawInRect
#define _web_sizeWithFont Rev_web_sizeWithFont
#define _web_sizeInRect Rev_web_sizeInRect
#define _web_sizeForWidth Rev_web_sizeForWidth
#define _web_stringForWidth Rev_web_stringForWidth
#endif

#import "WebNSStringExtrasIOS6.h"

#import "WebKitNSStringExtras.h"
#import "WebNSURLExtras.h"
#import <CoreText/CoreText.h>
#import <WebCore/WKGraphics.h>
#import <dlfcn.h>
#import <objc/runtime.h>
#import <cmath>
#import <wtf/RetainPtr.h>

static BOOL wordRoundingAllowed = YES;
static BOOL wordRoundingEnabled = YES;
static BOOL ascentRoundingEnabled = YES;

// What UIKit hands these methods.
//
// The 2015-era signature says GSFontRef, and this port kept that. On this OS
// UIKit calls -sizeWithFont: with a UIFont, an Objective-C object, and passing
// that to GSFontGetCTFont returns a pointer that is not a font at all - the
// crash was inside CFGetTypeID, before anything could be checked about it.
// So the argument is identified first: an object the runtime knows answers for
// its own name and size, and CoreText builds the font from those. A raw GSFont
// still goes through the conversion function, which is correct for callers that
// really do pass one.
static CTFontRef createCoreTextFontFromObject(id font)
{
    if (![font respondsToSelector:@selector(fontName)] || ![font respondsToSelector:@selector(pointSize)])
        return nullptr;

    NSString *name = [font fontName];
    CGFloat size = [font pointSize];
    if (![name isKindOfClass:[NSString class]] || !name.length || !(size > 0))
        return nullptr;

    return CTFontCreateWithName((CFStringRef)name, size, nullptr);
}

static bool isKnownObjectiveCObject(const void *pointer)
{
    if (!pointer || ((uintptr_t)pointer & 1))
        return false;
    Class candidate = object_getClass((__bridge id)pointer);
    if (!candidate)
        return false;
    const char *name = class_getName(candidate);
    if (!name || !*name)
        return false;
    return objc_lookUpClass(name) == candidate;
}

static CTFontRef coreTextFont(GSFontRef font)
{
    if (!font)
        return nullptr;

    // Cached because this runs for every string UIKit measures or draws, and
    // building a font is not cheap on this hardware.
    static NSMutableDictionary *cache;
    if (!cache)
        cache = [[NSMutableDictionary alloc] init];

    if (isKnownObjectiveCObject(font)) {
        id fontObject = (__bridge id)font;
        NSString *key = nil;
        if ([fontObject respondsToSelector:@selector(fontName)] && [fontObject respondsToSelector:@selector(pointSize)]) {
            NSString *name = [fontObject fontName];
            if ([name isKindOfClass:[NSString class]] && name.length)
                key = [NSString stringWithFormat:@"%@|%.2f", name, (double)[fontObject pointSize]];
        }
        if (key) {
            CTFontRef cached = (CTFontRef)[cache objectForKey:key];
            if (cached)
                return cached;
        }
        CTFontRef created = createCoreTextFontFromObject(fontObject);
        if (created && key) {
            [cache setObject:(id)created forKey:key];
            CFRelease(created);
            return (CTFontRef)[cache objectForKey:key];
        }
        if (created)
            CFAutorelease(created);
        return created;
    }

    static CTFontRef (*getCTFont)(GSFontRef);
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        getCTFont = (CTFontRef (*)(GSFontRef))dlsym(RTLD_DEFAULT, "GSFontGetCTFont");
    });
    if (!getCTFont)
        return nullptr;

    CTFontRef converted = getCTFont(font);
    if (!converted || CFGetTypeID(converted) != CTFontGetTypeID())
        return nullptr;
    return converted;
}

static CTLineRef createLine(NSString *string, GSFontRef font, CGFloat letterSpacing)
{
    if (![string isKindOfClass:[NSString class]] || !string.length)
        return nullptr;

    CTFontRef coreText = coreTextFont(font);
    if (!coreText)
        return nullptr;

    // Built with CoreFoundation rather than through the Objective-C bridge: the
    // bridge on this OS goes through more machinery for every value, and this
    // runs for every string UIKit measures or draws.
    // The colour comes from the context, not from the string.
    //
    // Without this CoreText paints its own default, which is black, whatever
    // colour UIKit set before the call. The status bar showed it plainly: its
    // text is white on black, so the clock and the carrier were painted black on
    // black and simply were not there, while the battery and the signal, being
    // images, were. Filling a rectangle in their place proved the drawing landed
    // on screen - only invisible.
    CFTypeRef keys[3] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName, nullptr };
    CFTypeRef values[3] = { coreText, kCFBooleanTrue, nullptr };
    CFIndex count = 2;
    RetainPtr<CFNumberRef> kern;
    if (letterSpacing && std::isfinite(letterSpacing)) {
        double spacing = letterSpacing;
        kern = adoptCF(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &spacing));
        if (kern) {
            keys[2] = kCTKernAttributeName;
            values[2] = kern.get();
            count = 3;
        }
    }

    RetainPtr<CFDictionaryRef> attributes = adoptCF(CFDictionaryCreate(kCFAllocatorDefault, keys, values, count,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!attributes)
        return nullptr;

    RetainPtr<CFAttributedStringRef> attributed = adoptCF(CFAttributedStringCreate(kCFAllocatorDefault,
        (CFStringRef)string, attributes.get()));
    if (!attributed)
        return nullptr;

    return CTLineCreateWithAttributedString(attributed.get());
}

static CTLineRef createTruncatedLine(CTLineRef line, NSString *string, GSFontRef font, CGFloat letterSpacing, CGFloat width, WebEllipsisStyle ellipsis)
{
    if (!line)
        return nullptr;
    if (ellipsis == WebEllipsisStyleNone || ellipsis == WebEllipsisStyleClip)
        return (CTLineRef)CFRetain(line);
    if (CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) <= width)
        return (CTLineRef)CFRetain(line);

    CTLineTruncationType type = kCTLineTruncationEnd;
    if (ellipsis == WebEllipsisStyleHead)
        type = kCTLineTruncationStart;
    else if (ellipsis == WebEllipsisStyleCenter)
        type = kCTLineTruncationMiddle;

    CTLineRef token = createLine(@"…", font, letterSpacing);
    CTLineRef truncated = CTLineCreateTruncatedLine(line, width, type, token);
    if (token)
        CFRelease(token);
    return truncated ? truncated : (CTLineRef)CFRetain(line);
}

@protocol RevFontLineHeight
- (CGFloat)lineHeight;
@end

static CGFloat fontLineHeight(GSFontRef font, CTFontRef ct)
{
    if (isKnownObjectiveCObject(font)) {
        id<RevFontLineHeight> f = (id<RevFontLineHeight>)font;
        if ([f respondsToSelector:@selector(lineHeight)]) {
            CGFloat lh = [f lineHeight];
            if (lh > 0)
                return ceilf(lh);
        }
    }
    if (ct)
        return ceilf(CTFontGetAscent(ct)) + ceilf(CTFontGetDescent(ct)) + ceilf(CTFontGetLeading(ct));
    return 0;
}

static CGSize lineSize(CTLineRef line, GSFontRef font)
{
    if (!line)
        return CGSizeZero;
    CGFloat ascent = 0, descent = 0, leading = 0;
    double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CGFloat height = fontLineHeight(font, coreTextFont(font));
    if (height < 1) {
        if (ascentRoundingEnabled)
            ascent = ceilf(ascent);
        height = ascent + ceilf(descent);
    }
    return CGSizeMake(wordRoundingEnabled ? ceilf(width) : width, height);
}

static CGSize drawLine(CTLineRef line, CGPoint point, GSFontRef font, BOOL measureOnly, BOOL drawUnderline)
{
    CGSize size = lineSize(line, font);
    CGContextRef context = WKGetCurrentGraphicsContext();

    static int trace = -1;
    if (trace < 0)
        trace = access("/tmp/native-text-trace", F_OK) == 0 ? 1 : 0;
    if (trace) {
        static FILE* log;
        if (!log) {
            log = fopen("/tmp/native-text.log", "w");
            if (log)
                setvbuf(log, nullptr, _IOLBF, 0);
        }
        if (log) {
            CGAffineTransform ctm = context ? CGContextGetCTM(context) : CGAffineTransformIdentity;
            CGRect clip = context ? CGContextGetClipBoundingBox(context) : CGRectZero;
            CGFloat traceAscent = 0, traceDescent = 0;
            if (line)
                CTLineGetTypographicBounds(line, &traceAscent, &traceDescent, nullptr);
            fprintf(log, "drawLine: measureOnly %d at %.1f,%.1f size %.1fx%.1f ascent %.1f descent %.1f ctm [%.2f %.2f %.2f %.2f %.1f %.1f] clip %.0f,%.0f %.0fx%.0f\n",
                (int)measureOnly, point.x, point.y, size.width, size.height, traceAscent, traceDescent,
                ctm.a, ctm.b, ctm.c, ctm.d, ctm.tx, ctm.ty,
                clip.origin.x, clip.origin.y, clip.size.width, clip.size.height);
        }
    }

    if (measureOnly || !context || !line)
        return size;

    // The point is the baseline, not the top of the line.
    //
    // Adding the ascent moved every string down by its own height. It is visible
    // in the status bar: UIKit draws the clock at y=15 in a bar 20 tall, and with
    // the ascent of a 14-point line added the baseline landed at 26 - below the
    // bar, so the clock and the carrier name were simply not there, while the
    // battery and the signal, which are images, were. Inside the application the
    // same shift left button titles clipped to their last two rows of pixels.
    // The point is the baseline. The two families of callers disagree about
    // that, and each is corrected where it belongs: -drawAtPoint: is given a
    // baseline by UIKit and is passed through, while -drawInRect: is given the
    // top of a rectangle and adds the ascent itself before calling here.
    //
    // Getting it wrong is visible in the status bar and was, both ways round:
    // adding the ascent here dropped the clock and the carrier out of a bar 20
    // points tall, and not adding it in the rectangle path lifted the battery
    // percentage off the top of its own.
    CGContextSaveGState(context);
    CGContextSetTextMatrix(context, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(context, point.x, point.y);
    CTLineDraw(line, context);

    if (drawUnderline) {
        CGFloat y = point.y + 1;
        CGContextMoveToPoint(context, point.x, y);
        CGContextAddLineToPoint(context, point.x + size.width, y);
        CGContextStrokePath(context);
    }

    CGContextRestoreGState(context);
    return size;
}

static CGFloat alignedX(CGRect rect, CGSize size, WebTextAlignment alignment)
{
    if (alignment == WebTextAlignmentCenter)
        return rect.origin.x + (rect.size.width - size.width) / 2;
    if (alignment == WebTextAlignmentRight)
        return rect.origin.x + rect.size.width - size.width;
    return rect.origin.x;
}

static NSString *stringOfLine(CTLineRef line, NSString *fallback)
{
    if (!line)
        return fallback;
    CFRange range = CTLineGetStringRange(line);
    if (range.location < 0 || range.length <= 0 || (NSUInteger)(range.location + range.length) > fallback.length)
        return fallback;
    return [fallback substringWithRange:NSMakeRange(range.location, range.length)];
}

static GSFontRef fontFromAttributes(NSDictionary *attributes)
{
    id font = [attributes objectForKey:@"NSFont"];
    if (!font)
        font = [attributes objectForKey:(id)kCTFontAttributeName];
    if (!font)
        return nullptr;
    if ([font respondsToSelector:@selector(_font)])
        return (GSFontRef)[font performSelector:@selector(_font)];
    return (GSFontRef)font;
}

static void wrapParagraph(NSString *paragraph, GSFontRef font, CGFloat letterSpacing, CGFloat width, NSMutableArray *lines)
{
    if (!paragraph.length) {
        [lines addObject:@""];
        return;
    }

    NSUInteger start = 0;
    while (start < paragraph.length) {
        NSString *remainder = [paragraph substringFromIndex:start];
        CTLineRef line = createLine(remainder, font, letterSpacing);
        if (!line) {
            [lines addObject:remainder];
            return;
        }
        CFIndex fits = CTLineGetStringIndexForPosition(line, CGPointMake(width, 0));
        if (line)
            CFRelease(line);

        if (fits <= 0 || (NSUInteger)fits >= remainder.length) {
            [lines addObject:remainder];
            return;
        }

        NSRange space = [remainder rangeOfString:@" " options:NSBackwardsSearch range:NSMakeRange(0, fits)];
        NSUInteger take = space.location != NSNotFound ? space.location + 1 : (NSUInteger)fits;
        [lines addObject:[[remainder substringToIndex:take] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]];
        start += take;
    }
}


@implementation NSString (WebNSStringExtrasIOS6)

+ (BOOL)_web_wordRoundingAllowed
{
    return wordRoundingAllowed;
}

+ (void)_web_setWordRoundingAllowed:(BOOL)allowed
{
    wordRoundingAllowed = allowed;
}

+ (BOOL)_web_wordRoundingEnabled
{
    return wordRoundingEnabled;
}

+ (void)_web_setWordRoundingEnabled:(BOOL)enabled
{
    wordRoundingEnabled = enabled;
}

+ (BOOL)_web_ascentRoundingEnabled
{
    return ascentRoundingEnabled;
}

+ (void)_web_setAscentRoundingEnabled:(BOOL)enabled
{
    ascentRoundingEnabled = enabled;
}

+ (NSString *)_web_stringWithData:(NSData *)data textEncodingName:(NSString *)encodingName
{
    if (!data)
        return nil;
    CFStringEncoding encoding = kCFStringEncodingUTF8;
    if (encodingName.length) {
        CFStringEncoding named = CFStringConvertIANACharSetNameToEncoding((CFStringRef)encodingName);
        if (named != kCFStringEncodingInvalidId)
            encoding = named;
    }
    return [(NSString *)CFStringCreateWithBytes(nullptr, (const UInt8 *)data.bytes, data.length, encoding, true) autorelease];
}

- (CGSize)__web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut drawUnderline:(BOOL)drawUnderline
{
    UNUSED_PARAM(includeEmoji);

    CTLineRef line = createLine(self, font, letterSpacing);
    if (!line) {
        if (renderedStringOut)
            *renderedStringOut = @"";
        return CGSizeZero;
    }

    CTLineRef shown = createTruncatedLine(line, self, font, letterSpacing, width, ellipsis);
    if (line)
        CFRelease(line);

    if (renderedStringOut)
        *renderedStringOut = stringOfLine(shown, self);

    CGSize size = drawLine(shown, point, font, measureOnly, drawUnderline);
    if (shown)
        CFRelease(shown);
    return size;
}

- (CGSize)__web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut
{
    return [self __web_drawAtPoint:point forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:includeEmoji measureOnly:measureOnly renderedStringOut:renderedStringOut drawUnderline:NO];
}

- (CGSize)__web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji measureOnly:(BOOL)measureOnly
{
    return [self __web_drawAtPoint:point forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:includeEmoji measureOnly:measureOnly renderedStringOut:nullptr drawUnderline:NO];
}

- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji
{
    return [self __web_drawAtPoint:point forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:includeEmoji measureOnly:NO];
}

- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing
{
    return [self __web_drawAtPoint:point forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:YES measureOnly:NO];
}

- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis
{
    return [self __web_drawAtPoint:point forWidth:width withFont:font ellipsis:ellipsis letterSpacing:0 includeEmoji:YES measureOnly:NO];
}

- (CGSize)_web_drawAtPoint:(CGPoint)point withFont:(GSFontRef)font
{
    return [self __web_drawAtPoint:point forWidth:CGFLOAT_MAX withFont:font ellipsis:WebEllipsisStyleNone letterSpacing:0 includeEmoji:YES measureOnly:NO];
}

- (CGSize)_web_drawAtPoint:(CGPoint)point forWidth:(CGFloat)width withAttributes:(NSDictionary *)attributes
{
    return [self __web_drawAtPoint:point forWidth:width withFont:fontFromAttributes(attributes) ellipsis:WebEllipsisStyleTail letterSpacing:0 includeEmoji:YES measureOnly:NO];
}

- (CGSize)_web_sizeWithFont:(GSFontRef)font forWidth:(CGFloat)width ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing resultRange:(NSRange *)resultRange
{
    if (resultRange)
        *resultRange = NSMakeRange(0, self.length);
    return [self __web_drawAtPoint:CGPointZero forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:YES measureOnly:YES];
}

- (CGSize)_web_sizeWithFont:(GSFontRef)font forWidth:(CGFloat)width ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing
{
    return [self __web_drawAtPoint:CGPointZero forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:YES measureOnly:YES];
}

- (CGSize)_web_sizeWithFont:(GSFontRef)font forWidth:(CGFloat)width ellipsis:(WebEllipsisStyle)ellipsis
{
    return [self __web_drawAtPoint:CGPointZero forWidth:width withFont:font ellipsis:ellipsis letterSpacing:0 includeEmoji:YES measureOnly:YES];
}

- (CGSize)_web_sizeWithFont:(GSFontRef)font
{
    return [self __web_drawAtPoint:CGPointZero forWidth:CGFLOAT_MAX withFont:font ellipsis:WebEllipsisStyleNone letterSpacing:0 includeEmoji:YES measureOnly:YES];
}

- (CGSize)_web_sizeForWidth:(CGFloat)width withAttributes:(NSDictionary *)attributes
{
    return [self __web_drawAtPoint:CGPointZero forWidth:width withFont:fontFromAttributes(attributes) ellipsis:WebEllipsisStyleTail letterSpacing:0 includeEmoji:YES measureOnly:YES];
}

- (NSString *)_web_stringForWidth:(CGFloat)width withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis letterSpacing:(CGFloat)letterSpacing includeEmoji:(BOOL)includeEmoji
{
    NSString *rendered = self;
    [self __web_drawAtPoint:CGPointZero forWidth:width withFont:font ellipsis:ellipsis letterSpacing:letterSpacing includeEmoji:includeEmoji measureOnly:YES renderedStringOut:&rendered];
    return rendered;
}

- (CGSize)__web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment letterSpacing:(CGFloat)letterSpacing lineSpacing:(CGFloat)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut drawUnderline:(BOOL)drawUnderline
{
    UNUSED_PARAM(includeEmoji);

    CTFontRef coreText = coreTextFont(font);
    if (!coreText || !self.length) {
        if (renderedStringOut)
            *renderedStringOut = @"";
        if (truncationRect)
            *truncationRect = CGRectZero;
        return CGSizeZero;
    }

    CGFloat step = fontLineHeight(font, coreText) + lineSpacing;

    NSArray *paragraphs = [self componentsSeparatedByString:@"\n"];
    NSMutableArray *lines = [NSMutableArray array];
    for (NSString *paragraph in paragraphs)
        wrapParagraph(paragraph, font, letterSpacing, rect.size.width, lines);

    NSUInteger maximumLines = step > 0 ? (NSUInteger)floorf(rect.size.height / step) : lines.count;
    if (!maximumLines)
        maximumLines = 1;
    BOOL truncating = lines.count > maximumLines;
    if (truncating)
        [lines removeObjectsInRange:NSMakeRange(maximumLines, lines.count - maximumLines)];

    NSMutableString *rendered = renderedStringOut ? [NSMutableString string] : nil;
    CGSize total = CGSizeZero;
    CGFloat y = rect.origin.y;

    for (NSUInteger index = 0; index < lines.count; index++) {
        NSString *text = [lines objectAtIndex:index];
        CTLineRef line = createLine(text, font, letterSpacing);
        if (!line)
            continue;
        BOOL lastLine = index + 1 == lines.count;
        CTLineRef shown = (truncating && lastLine)
            ? createTruncatedLine(line, text, font, letterSpacing, rect.size.width, ellipsis == WebEllipsisStyleNone ? WebEllipsisStyleTail : ellipsis)
            : (CTLineRef)CFRetain(line);
        if (line)
            CFRelease(line);

        CGSize size = lineSize(shown, font);
        CGFloat lineAscent = 0;
        CTLineGetTypographicBounds(shown, &lineAscent, nullptr, nullptr);
        CGPoint origin = CGPointMake(alignedX(rect, size, alignment), y + ceilf(lineAscent));
        drawLine(shown, origin, font, measureOnly, drawUnderline);

        if (rendered) {
            if (rendered.length)
                [rendered appendString:@"\n"];
            [rendered appendString:stringOfLine(shown, text)];
        }
        if (truncationRect && truncating && lastLine)
            *truncationRect = CGRectMake(origin.x, y, size.width, step);

        total.width = MAX(total.width, size.width);
        total.height = y + step - rect.origin.y;
        y += step;
        if (shown)
        CFRelease(shown);
    }

    if (renderedStringOut)
        *renderedStringOut = rendered;
    if (truncationRect && !truncating)
        *truncationRect = CGRectZero;
    return total;
}

- (CGSize)__web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment letterSpacing:(CGFloat)letterSpacing lineSpacing:(CGFloat)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly renderedStringOut:(NSString **)renderedStringOut
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:alignment letterSpacing:letterSpacing lineSpacing:lineSpacing includeEmoji:includeEmoji truncationRect:truncationRect measureOnly:measureOnly renderedStringOut:renderedStringOut drawUnderline:NO];
}

- (CGSize)__web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment letterSpacing:(CGFloat)letterSpacing lineSpacing:(CGFloat)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:alignment letterSpacing:letterSpacing lineSpacing:lineSpacing includeEmoji:includeEmoji truncationRect:truncationRect measureOnly:measureOnly renderedStringOut:nullptr drawUnderline:NO];
}

- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment lineSpacing:(int)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect measureOnly:(BOOL)measureOnly
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:alignment letterSpacing:0 lineSpacing:lineSpacing includeEmoji:includeEmoji truncationRect:truncationRect measureOnly:measureOnly];
}

- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment lineSpacing:(int)lineSpacing includeEmoji:(BOOL)includeEmoji truncationRect:(CGRect *)truncationRect
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:alignment letterSpacing:0 lineSpacing:lineSpacing includeEmoji:includeEmoji truncationRect:truncationRect measureOnly:NO];
}

- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment lineSpacing:(int)lineSpacing
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:alignment letterSpacing:0 lineSpacing:lineSpacing includeEmoji:YES truncationRect:nullptr measureOnly:NO];
}

- (CGSize)_web_drawInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis alignment:(WebTextAlignment)alignment
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:alignment letterSpacing:0 lineSpacing:0 includeEmoji:YES truncationRect:nullptr measureOnly:NO];
}

- (CGSize)_web_drawInRect:(CGRect)rect withAttributes:(NSDictionary *)attributes
{
    return [self __web_drawInRect:rect withFont:fontFromAttributes(attributes) ellipsis:WebEllipsisStyleTail alignment:WebTextAlignmentLeft letterSpacing:0 lineSpacing:0 includeEmoji:YES truncationRect:nullptr measureOnly:NO];
}

- (CGSize)_web_sizeInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis lineSpacing:(int)lineSpacing
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:WebTextAlignmentLeft letterSpacing:0 lineSpacing:lineSpacing includeEmoji:YES truncationRect:nullptr measureOnly:YES];
}

- (CGSize)_web_sizeInRect:(CGRect)rect withFont:(GSFontRef)font ellipsis:(WebEllipsisStyle)ellipsis
{
    return [self __web_drawInRect:rect withFont:font ellipsis:ellipsis alignment:WebTextAlignmentLeft letterSpacing:0 lineSpacing:0 includeEmoji:YES truncationRect:nullptr measureOnly:YES];
}

- (CGSize)_web_sizeInRect:(CGRect)rect withAttributes:(NSDictionary *)attributes
{
    return [self __web_drawInRect:rect withFont:fontFromAttributes(attributes) ellipsis:WebEllipsisStyleTail alignment:WebTextAlignmentLeft letterSpacing:0 lineSpacing:0 includeEmoji:YES truncationRect:nullptr measureOnly:YES];
}

- (BOOL)LS_hasCaseInsensitivePrefix:(NSString *)prefix
{
    return [self _webkit_hasCaseInsensitivePrefix:prefix];
}

- (BOOL)_webkit_hasCaseInsensitiveSubstring:(NSString *)substring
{
    if (!substring)
        return NO;
    return [self rangeOfString:substring options:NSCaseInsensitiveSearch].location != NSNotFound;
}

- (NSString *)_webkit_unescapedQueryValue
{
    NSString *unescaped = [[self stringByReplacingOccurrencesOfString:@"+" withString:@" "] stringByReplacingPercentEscapesUsingEncoding:NSUTF8StringEncoding];
    return unescaped ? unescaped : self;
}

- (NSString *)LS_unescapedQueryValue
{
    return [self _webkit_unescapedQueryValue];
}

- (NSDictionary *)_webkit_queryKeysAndValues
{
    NSRange question = [self rangeOfString:@"?"];
    NSString *query = question.location == NSNotFound ? self : [self substringFromIndex:question.location + 1];
    NSMutableDictionary *pairs = [NSMutableDictionary dictionary];
    for (NSString *pair in [query componentsSeparatedByString:@"&"]) {
        NSRange equals = [pair rangeOfString:@"="];
        if (equals.location == NSNotFound || !equals.location)
            continue;
        NSString *key = [[pair substringToIndex:equals.location] _webkit_unescapedQueryValue];
        NSString *value = [[pair substringFromIndex:equals.location + 1] _webkit_unescapedQueryValue];
        [pairs setObject:value forKey:key];
    }
    return pairs;
}

- (NSDictionary *)queryToDict
{
    return [self _webkit_queryKeysAndValues];
}

- (NSString *)_webkit_URLFragment
{
    NSRange hash = [self rangeOfString:@"#"];
    if (hash.location == NSNotFound)
        return nil;
    return [self substringFromIndex:hash.location + 1];
}

- (BOOL)_webkit_isFTPDirectoryURL
{
    if (![self _webkit_hasCaseInsensitivePrefix:@"ftp:"])
        return NO;
    return [self hasSuffix:@"/"];
}

- (NSString *)_webkit_stringByCollapsingWhitespaceCharacters
{
    NSMutableString *result = [NSMutableString stringWithCapacity:self.length];
    NSCharacterSet *whitespace = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    BOOL pendingSpace = NO;
    for (NSUInteger index = 0; index < self.length; index++) {
        unichar character = [self characterAtIndex:index];
        if ([whitespace characterIsMember:character]) {
            pendingSpace = result.length > 0;
            continue;
        }
        if (pendingSpace) {
            [result appendString:@" "];
            pendingSpace = NO;
        }
        [result appendFormat:@"%C", character];
    }
    return result;
}

- (NSString *)_webkit_stringByCollapsingNonPrintingCharacters
{
    NSMutableString *result = [NSMutableString stringWithCapacity:self.length];
    BOOL pendingSpace = NO;
    for (NSUInteger index = 0; index < self.length; index++) {
        unichar character = [self characterAtIndex:index];
        if (character < 0x20 || character == 0x7F) {
            pendingSpace = result.length > 0;
            continue;
        }
        if (pendingSpace) {
            [result appendString:@" "];
            pendingSpace = NO;
        }
        [result appendFormat:@"%C", character];
    }
    return result;
}

- (NSString *)_web_stringByStrippingReturnCharacters
{
    return [[self componentsSeparatedByCharactersInSet:[NSCharacterSet newlineCharacterSet]] componentsJoinedByString:@""];
}

- (NSString *)_web_capitalizeRFC822HeaderFieldName
{
    NSMutableString *result = [NSMutableString stringWithCapacity:self.length];
    BOOL startOfWord = YES;
    for (NSUInteger index = 0; index < self.length; index++) {
        unichar character = [self characterAtIndex:index];
        if (startOfWord && character >= 'a' && character <= 'z')
            character = character - 'a' + 'A';
        else if (!startOfWord && character >= 'A' && character <= 'Z')
            character = character - 'A' + 'a';
        [result appendFormat:@"%C", character];
        startOfWord = character == '-';
    }
    return result;
}

- (NSString *)_web_securedStringIncludingLastCharacter:(BOOL)includingLastCharacter
{
    if (!self.length)
        return self;
    NSUInteger hidden = includingLastCharacter ? self.length : self.length - 1;
    NSMutableString *result = [NSMutableString stringWithCapacity:self.length];
    for (NSUInteger index = 0; index < hidden; index++)
        [result appendString:@"\u2022"];
    if (!includingLastCharacter)
        [result appendString:[self substringFromIndex:self.length - 1]];
    return result;
}

- (NSString *)_web_decodeHostNameWithRange:(NSRange)range
{
    if (!NSMaxRange(range) || NSMaxRange(range) > self.length)
        return nil;
    NSString *host = [[self substringWithRange:range] _web_decodeHostName];
    if (!host)
        return nil;
    NSMutableString *result = [[self mutableCopy] autorelease];
    [result replaceCharactersInRange:range withString:host];
    return result;
}

- (NSString *)_web_encodeHostNameWithRange:(NSRange)range
{
    if (!NSMaxRange(range) || NSMaxRange(range) > self.length)
        return nil;
    NSString *host = [[self substringWithRange:range] _web_encodeHostName];
    if (!host)
        return nil;
    NSMutableString *result = [[self mutableCopy] autorelease];
    [result replaceCharactersInRange:range withString:host];
    return result;
}

- (BOOL)_web_hostNameNeedsDecodingWithRange:(NSRange)range
{
    if (!NSMaxRange(range) || NSMaxRange(range) > self.length)
        return NO;
    NSString *host = [self substringWithRange:range];
    NSString *decoded = [host _web_decodeHostName];
    return decoded && ![decoded isEqualToString:host];
}

- (BOOL)_web_hostNameNeedsEncodingWithRange:(NSRange)range
{
    if (!NSMaxRange(range) || NSMaxRange(range) > self.length)
        return NO;
    NSString *host = [self substringWithRange:range];
    NSString *encoded = [host _web_encodeHostName];
    return encoded && ![encoded isEqualToString:host];
}

@end
