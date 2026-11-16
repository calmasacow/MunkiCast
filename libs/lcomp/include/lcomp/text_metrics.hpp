// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QFont>
#include <QFontMetrics>
#include <QRect>
#include <QString>
#include <algorithm>
#include <lcomp/countdown.hpp>
#include <ldoc/document.hpp>

/// How big a text source draws, measured in exactly one place.
///
/// The source worker sizes its surface to the laid-out text (ADR-0070's resolution independence
/// is about position, not about rasterising type twice), and the shell has to put selection
/// handles around that same rectangle. When the two measured separately they disagreed: the
/// shell still read the old `width`/`height` settings and defaulted to 1920x1080, so the handles
/// sat in the middle of the canvas while the glyphs were somewhere else entirely. So the
/// measurement lives here and both callers include it.
///
/// Header-only, and the only Qt in `lcomp`'s public headers: nothing in the library itself
/// compiles it, so the scene walk stays Qt-free as its CMakeLists says. The two includers both
/// link Qt6::Gui already. A `QFontMetrics` needs a QGuiApplication, which both have, and neither
/// needs a GPU to ask how wide a string is.
namespace lcomp {

/// Ceiling for a surface that sizes itself to its content. A safety bound, not a layout: the
/// preview protocol refuses anything over 8192 on an edge, and a surface that hit that mid-show
/// would drop the layer rather than shrink the type. Nothing type reaches it -- 900px "LIVE",
/// the largest case measured, lays out at 1956x1010.
inline constexpr int kMaxTextSurface = 4096;

/// The font a text source's settings describe, at the size they ask for.
///
/// `setStyleName` rather than setBold/setItalic: it picks the face the family actually ships,
/// where the flags ask Qt to synthesise one from Regular when it does not.
inline QFont text_font(const ldoc::ParamMap& settings) {
    QFont font(QString::fromStdString(timer_text(settings, "font", "Sans")));
    if (const auto style = timer_text(settings, "fontStyle"); !style.empty())
        font.setStyleName(QString::fromStdString(style));
    font.setPixelSize(static_cast<int>(
        std::clamp(timer_number(settings, "size", 48), std::int64_t{1}, std::int64_t{1024})));
    return font;
}

/// Whether the digits of a ticking counter are given a common width.
///
/// A timer is the one case where the text changes by itself, and in a proportional face a "1"
/// is narrower than an "8", so "18:88" ticking to "11:11" moves the glyphs about. Levelling the
/// digits stops that -- but it is a deliberate departure from the face the operator chose, so it
/// is a checkbox and not a guess: a monospaced face needs it least and a face with real tabular
/// figures needs it not at all, and neither can be detected reliably enough to decide silently.
///
/// Everything else about how a text source looks comes from the text source. This is the only
/// thing a countdown is allowed to change, and only while it is counting: once the end message
/// is on screen there are no ticking digits, so it stops applying.
inline bool fixed_width_digits(const ldoc::ParamMap& settings, bool ended) {
    return timer_flag(settings, "countdown.enabled") && !ended &&
           timer_flag(settings, "countdown.fixedWidth");
}

/// The Qt draw flags for these settings. Horizontal alignment is justification *between* lines
/// only; where the block sits on the canvas is the node's `layout.alignment`, because a surface
/// sized to its text has no slack inside it to align against.
inline int text_flags(const ldoc::ParamMap& settings) {
    const auto align = timer_text(settings, "alignment", "Left");
    // Not Qt::TextSingleLine when wrap is off: that collapses a newline the operator typed into
    // a space. Off means "do not break lines I did not break", not "ignore the ones I did".
    return (timer_flag(settings, "wrap") ? Qt::TextWordWrap : 0) | Qt::AlignTop |
           (align == "Right"    ? Qt::AlignRight
            : align == "Centre" ? Qt::AlignHCenter
                                : Qt::AlignLeft);
}

/// The rectangle the given string occupies, origin included.
///
/// Measured left-aligned whatever the alignment is: alignment positions a line inside the
/// rectangle it is given, so measuring centred against a 4096-wide rectangle reports a rectangle
/// starting around x=2000, and sizing a surface to that put the text off it entirely. The size of
/// a string does not depend on where it is put, so the measurement must not either.
///
/// `left()`/`top()` can be non-zero -- a face with a negative left bearing has its first glyph
/// outside the box -- so a caller painting into a surface of this size offsets its origin by them.
/// The common advance given to every digit when fixed width is on, or 0 when it is off.
///
/// The widest digit in the face, so nothing is squeezed: a narrow digit is centred in the cell
/// rather than the wide one being cramped into it.
inline int digit_cell(const QFont& font) {
    const QFontMetrics metrics(font);
    int cell = 0;
    for (char digit = '0'; digit <= '9'; ++digit)
        cell = std::max(cell, metrics.horizontalAdvance(QLatin1Char(digit)));
    return cell;
}

inline QRect text_bounds(const ldoc::ParamMap& settings, const QFont& font, const QString& text,
                         bool fixed_digits = false) {
    const QFontMetrics metrics(font);
    const auto wrap_at = timer_flag(settings, "wrap")
                             ? std::clamp(timer_number(settings, "wrapAt"), std::int64_t{0},
                                          std::int64_t{kMaxTextSurface})
                             : 0;
    const int limit = wrap_at > 0 ? static_cast<int>(wrap_at) : kMaxTextSurface;
    const int measured_flags = (text_flags(settings) & ~Qt::AlignHorizontal_Mask) | Qt::AlignLeft;
    QRect needed = metrics.boundingRect(QRect(0, 0, limit, kMaxTextSurface), measured_flags, text);
    // Levelled digits lay out wider than Qt's own advances for a proportional face, so measuring
    // with Qt's alone would clip them.
    if (fixed_digits) {
        const int cell = digit_cell(font);
        int total = 0;
        for (const QChar c : text)
            total += c.isDigit() ? cell : metrics.horizontalAdvance(c);
        needed.setWidth(std::max(needed.width(), total));
    }
    return needed;
}

/// What a text source lays itself out against: the string on screen, nothing else.
///
/// A countdown used to be measured against the widest caption it would ever produce, so that a
/// ticking surface never resized. That made the surface bigger than the text at every other
/// moment, and since the node's anchor acts on the surface, the text floated inside a box that
/// was not its size -- which is why anchoring a timer appeared to do nothing at all. The timer
/// is allowed to change the string and, when asked, the width of its digits. It is not allowed
/// to change the size of the layer.
inline QString text_shown(const ldoc::ParamMap& settings, std::int64_t now = 0) {
    if (!timer_flag(settings, "countdown.enabled"))
        return QString::fromStdString(timer_text(settings, "text"));
    return QString::fromStdString(countdown(settings, now).text);
}

/// The surface a text source draws onto, and the font it draws with.
///
/// Returned together because the font can come down: a 1024px face asking for a surface wider
/// than the compositor will carry has to shrink rather than be refused mid-show. A caller that
/// only wants the size can ignore the font; a caller that paints must use the one returned here,
/// or it will paint at a size the surface was not measured for.
struct TextSurface {
    QFont font;
    QRect bounds;      ///< `size()` is the surface; `topLeft()` is the bearing offset.
    bool fixed_digits = false; ///< Whether this was laid out with levelled digit advances.
};

/// `now` is the countdown clock; it is ignored by a plain text source. The shell passes 0, which
/// measures the timer at its starting value -- the selection handles then track the published
/// frames as they tick, because the worker republishes a resized surface each time.
inline TextSurface text_surface(const ldoc::ParamMap& settings, std::int64_t now = 0) {
    const QString shown = text_shown(settings, now);
    const bool ended = timer_flag(settings, "countdown.enabled")
                           ? countdown(settings, now).ended
                           : false;
    TextSurface out{text_font(settings), {}, fixed_width_digits(settings, ended)};
    out.bounds = text_bounds(settings, out.font, shown, out.fixed_digits);
    for (int size = out.font.pixelSize();
         size > 6 &&
         (out.bounds.height() > kMaxTextSurface || out.bounds.width() > kMaxTextSurface);
         size = size * 9 / 10) {
        out.font.setPixelSize(size * 9 / 10);
        out.bounds = text_bounds(settings, out.font, shown, out.fixed_digits);
    }
    out.bounds.setWidth(std::max(1, out.bounds.width()));
    out.bounds.setHeight(std::max(1, out.bounds.height()));
    return out;
}

} // namespace lcomp
