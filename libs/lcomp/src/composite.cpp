// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcomp/composite.hpp"

#include "image.hpp"
#include "lcomp/filters.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace lcomp {

bool animated_plan(const DrawPlan& plan) {
    const auto timed = [](const auto& effects) {
        return std::ranges::any_of(effects, [](const auto& e) {
            return e.enabled && (e.type == "filter.scroll" || e.type == "filter.render-delay" ||
                                 e.type == "filter.video-delay");
        });
    };
    if (timed(plan.filters))
        return true;
    for (const auto& op : plan.ops)
        if (timed(op.filters) || (op.nested && animated_plan(*op.nested)))
            return true;
    return false;
}

std::optional<int> quarter_turn(double degrees) {
    if (!std::isfinite(degrees))
        return std::nullopt;
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0)
        wrapped += 360.0;
    const double steps = std::round(wrapped / 90.0);
    if (std::abs(wrapped - steps * 90.0) > 1e-4)
        return std::nullopt;
    int quarter = static_cast<int>(steps) % 4;
    if (quarter < 0)
        quarter += 4;
    return quarter;
}

bool blend_supported(ldoc::BlendMode mode) {
    switch (mode) {
    case ldoc::BlendMode::Normal:
    case ldoc::BlendMode::LinearDodge:
    case ldoc::BlendMode::Subtract:
    case ldoc::BlendMode::Screen:
    case ldoc::BlendMode::Multiply:
    case ldoc::BlendMode::Lighten:
    case ldoc::BlendMode::Darken:
        return true;
    default:
        return false;
    }
}

bool transform_supported(const ldoc::Transform& t) {
    if (t.skew_deg.x != 0.0 || t.skew_deg.y != 0.0)
        return false;
    if (!quarter_turn(t.rotation_deg))
        return false;
    const double fields[] = {t.position.x, t.position.y,   t.scale.x,    t.scale.y,   t.pivot.x,
                             t.pivot.y,    t.rotation_deg, t.skew_deg.x, t.skew_deg.y};
    return std::ranges::all_of(fields, [](double v) { return std::isfinite(v); });
}

namespace {

/// Clockwise, in screen space where Y grows downward. `(1, 0)` becomes `(0, 1)`.
void rotate_cw(int quarter, double x, double y, double& out_x, double& out_y) {
    switch (quarter) {
    case 1:
        out_x = -y;
        out_y = x;
        return;
    case 2:
        out_x = -x;
        out_y = -y;
        return;
    case 3:
        out_x = y;
        out_y = -x;
        return;
    default:
        out_x = x;
        out_y = y;
        return;
    }
}

/// The fitted content before scale and rotation. Scale and the quarter turn are applied by
/// the callers, so a resize can solve them without a second copy of the fit rules.
struct Unscaled {
    double base_w = 0;
    double base_h = 0;
    double rel_cx = 0;
    double rel_cy = 0;
    double rel_dx = 0;
    double rel_dy = 0;
};

std::optional<Unscaled> unscaled_content(const ldoc::Node& node, double natural_w, double natural_h,
                                         bool have_natural, GapReason& why) {
    if (!transform_supported(node.transform)) {
        why = GapReason::UnsupportedTransform;
        return std::nullopt;
    }

    const bool have_box = node.layout.size.x > 0.0 && node.layout.size.y > 0.0;
    if (node.layout.fit == ldoc::FitMode::Natural && !have_natural && !have_box) {
        why = GapReason::NoSize;
        return std::nullopt;
    }
    if (!have_box && !have_natural) {
        why = GapReason::NoSize;
        return std::nullopt;
    }

    const double bx = node.transform.position.x;
    const double by = node.transform.position.y;
    const double bw = have_box ? node.layout.size.x : natural_w;
    const double bh = have_box ? node.layout.size.y : natural_h;
    if (!(bw > 0.0) || !(bh > 0.0) || !std::isfinite(bw) || !std::isfinite(bh)) {
        why = GapReason::NoSize;
        return std::nullopt;
    }

    double sw = have_natural ? natural_w : bw;
    double sh = have_natural ? natural_h : bh;
    const ldoc::CropRect& crop = node.layout.crop;
    for (double edge : {crop.left, crop.top, crop.right, crop.bottom}) {
        if (!std::isfinite(edge) || edge < 0 || edge > kMaxPreviewEdge) {
            why = GapReason::NoSize;
            return std::nullopt;
        }
    }
    sw -= crop.left + crop.right;
    sh -= crop.top + crop.bottom;
    if (!(sw > 0.0) || !(sh > 0.0) || !std::isfinite(sw) || !std::isfinite(sh)) {
        why = GapReason::NoSize;
        return std::nullopt;
    }

    double dx = bx;
    double dy = by;
    double dw = bw;
    double dh = bh;
    const double ax = node.layout.alignment.x;
    const double ay = node.layout.alignment.y;
    switch (node.layout.fit) {
    case ldoc::FitMode::Natural:
        dw = sw;
        dh = sh;
        dx = bx + crop.left;
        dy = by + crop.top;
        break;
    case ldoc::FitMode::Stretch:
        break;
    case ldoc::FitMode::Contain: {
        const double s = std::min(bw / sw, bh / sh);
        dw = sw * s;
        dh = sh * s;
        dx = bx + (bw - dw) * ax;
        dy = by + (bh - dh) * ay;
        break;
    }
    case ldoc::FitMode::Cover: {
        const double s = std::max(bw / sw, bh / sh);
        dw = sw * s;
        dh = sh * s;
        dx = bx + (bw - dw) * ax;
        dy = by + (bh - dh) * ay;
        break;
    }
    case ldoc::FitMode::FillWidth: {
        const double s = bw / sw;
        dw = bw;
        dh = sh * s;
        dx = bx;
        dy = by + (bh - dh) * ay;
        break;
    }
    case ldoc::FitMode::FillHeight: {
        const double s = bh / sh;
        dh = bh;
        dw = sw * s;
        dy = by;
        dx = bx + (bw - dw) * ax;
        break;
    }
    }

    Unscaled out;
    out.base_w = dw;
    out.base_h = dh;
    out.rel_cx = bw * node.transform.pivot.x;
    out.rel_cy = bh * node.transform.pivot.y;
    out.rel_dx = dx - bx;
    out.rel_dy = dy - by;
    return out;
}

void scaled_corners(const Unscaled& u, double sx, double sy, double& left, double& top,
                    double& right, double& bottom) {
    left = (u.rel_dx - u.rel_cx) * sx;
    top = (u.rel_dy - u.rel_cy) * sy;
    right = left + u.base_w * sx;
    bottom = top + u.base_h * sy;
}

} // namespace

std::optional<DrawnFrame> drawn_frame(const ldoc::Node& node, double natural_w, double natural_h,
                                      bool have_natural, GapReason& why) {
    const std::optional<Unscaled> fitted =
        unscaled_content(node, natural_w, natural_h, have_natural, why);
    if (!fitted)
        return std::nullopt;

    const int quarter = quarter_turn(node.transform.rotation_deg).value_or(0);
    const double sx = node.transform.scale.x;
    const double sy = node.transform.scale.y;
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    scaled_corners(*fitted, sx, sy, left, top, right, bottom);

    const double xs[4] = {left, right, left, right};
    const double ys[4] = {top, top, bottom, bottom};
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    double max_x = -min_x;
    double max_y = -min_x;
    for (int i = 0; i < 4; ++i) {
        double rx = 0;
        double ry = 0;
        rotate_cw(quarter, xs[i], ys[i], rx, ry);
        min_x = std::min(min_x, rx);
        min_y = std::min(min_y, ry);
        max_x = std::max(max_x, rx);
        max_y = std::max(max_y, ry);
    }

    DrawnFrame frame;
    frame.base_width = fitted->base_w;
    frame.base_height = fitted->base_h;
    frame.rel_cx = fitted->rel_cx;
    frame.rel_cy = fitted->rel_cy;
    frame.rel_dx = fitted->rel_dx;
    frame.rel_dy = fitted->rel_dy;
    frame.x = node.transform.position.x + fitted->rel_cx + min_x;
    frame.y = node.transform.position.y + fitted->rel_cy + min_y;
    frame.width = max_x - min_x;
    frame.height = max_y - min_y;
    if (!(frame.width > 0.0) || !(frame.height > 0.0))
        return frame;

    constexpr double kBound = 1.0e7;
    if (!std::isfinite(frame.x) || !std::isfinite(frame.y) || !std::isfinite(frame.width) ||
        !std::isfinite(frame.height) || std::abs(frame.x) > kBound || std::abs(frame.y) > kBound ||
        frame.width > kBound || frame.height > kBound) {
        why = GapReason::UnsupportedTransform;
        return std::nullopt;
    }
    return frame;
}

std::optional<SolvedFrame> solve_drawn_rect(const ldoc::Node& node, double natural_w,
                                            double natural_h, bool have_natural, double x, double y,
                                            double width, double height) {
    GapReason why = GapReason::NoSize;
    const std::optional<Unscaled> fitted =
        unscaled_content(node, natural_w, natural_h, have_natural, why);
    if (!fitted || !(fitted->base_w > 0.0) || !(fitted->base_h > 0.0))
        return std::nullopt;
    const std::optional<int> quarter = quarter_turn(node.transform.rotation_deg);
    if (!quarter || !(width > 0.0) || !(height > 0.0) || !std::isfinite(x) || !std::isfinite(y))
        return std::nullopt;

    const double sign_x = node.transform.scale.x < 0.0 ? -1.0 : 1.0;
    const double sign_y = node.transform.scale.y < 0.0 ? -1.0 : 1.0;
    const bool turned = *quarter == 1 || *quarter == 3;
    const double abs_sx = turned ? height / fitted->base_w : width / fitted->base_w;
    const double abs_sy = turned ? width / fitted->base_h : height / fitted->base_h;
    if (!(abs_sx > 0.0) || !(abs_sy > 0.0) || !std::isfinite(abs_sx) || !std::isfinite(abs_sy))
        return std::nullopt;
    const double sx = sign_x * abs_sx;
    const double sy = sign_y * abs_sy;

    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    scaled_corners(*fitted, sx, sy, left, top, right, bottom);
    const double xs[4] = {left, right, left, right};
    const double ys[4] = {top, top, bottom, bottom};
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    for (int i = 0; i < 4; ++i) {
        double rx = 0;
        double ry = 0;
        rotate_cw(*quarter, xs[i], ys[i], rx, ry);
        min_x = std::min(min_x, rx);
        min_y = std::min(min_y, ry);
    }

    SolvedFrame solved;
    solved.x = x - fitted->rel_cx - min_x;
    solved.y = y - fitted->rel_cy - min_y;
    solved.scale_x = sx;
    solved.scale_y = sy;
    if (!std::isfinite(solved.x) || !std::isfinite(solved.y))
        return std::nullopt;
    return solved;
}

namespace {

/// The inverse of the curve in `Rgba::to_srgb8`. Copied rather than shared because that function
/// is local to the document library and the two have to stay inverses: the half-coverage test
/// is a literal sRGB byte, and a drift here is what it would catch.
double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// The same transfer function, for the 256 values a byte can hold.
///
/// Every blended pixel converts three channels out of sRGB, and the source bytes have 256
/// possible values -- so the `std::pow` was being asked the same 256 questions millions of times
/// a second. Measured on a 1080p frame: 110 ms of pow per composite, which was the whole of a
/// 36 ms-and-rising frame time and far more than the scaling or the copying it was blamed on.
///
/// Exact, not approximate: the table is filled from the same function, so a byte converts to the
/// identical double it did before. Nothing about the picture changes.
const std::array<double, 256>& srgb_table() {
    static const std::array<double, 256> table = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<double>(i) / 255.0);
        return t;
    }();
    return table;
}

/// Convert one already-byte channel. The only form the blit needs.
double srgb_byte(std::uint8_t v) { return srgb_table()[v]; }


bool audio_only(std::string_view type) {
    return type == "source.audio-input" || type == "source.audio-output";
}

bool effect_enabled(const ldoc::Node& node) {
    return !video_filters_supported(node.effects);
}

using Px = DrawRect;

/// Where a node's box lands, in pixel edges, half-open.
///
/// Natural size is the source's own size after crop. The box is `layout.size` when it is set
/// and the natural size otherwise — the document's rule that a zero box means "the source's
/// natural size". Fit then places the source inside that box, and scale acts around the box's
/// pivot. Integer-aligned inputs stay on pixel edges; nothing here antialiases, so a test can
/// name a pixel and mean it.
std::optional<Px> place(const ldoc::Node& node, double natural_w, double natural_h,
                        bool have_natural, GapReason& why) {
    // Integer edges of `drawn_frame`. The overlay reads the continuous rectangle; the picture
    // reads these, and they have to be the same placement or the handles miss the pixels.
    const std::optional<DrawnFrame> frame =
        drawn_frame(node, natural_w, natural_h, have_natural, why);
    if (!frame)
        return std::nullopt;
    if (!(frame->width > 0.0) || !(frame->height > 0.0))
        return Px{};

    Px out;
    out.x0 = static_cast<int>(std::floor(frame->x));
    out.y0 = static_cast<int>(std::floor(frame->y));
    out.x1 = static_cast<int>(std::ceil(frame->x + frame->width));
    out.y1 = static_cast<int>(std::ceil(frame->y + frame->height));
    return out;
}

void refuse(DrawPlan& picture, const ldoc::Node& node, GapReason why) {
    picture.gaps.push_back(Gap{node.id, node.name, why});
}

/// Straight linear source over the sRGB8 pixel already in the buffer.
///
/// `Rgba` is stored straight, not premultiplied: `from_srgb8` divides the alpha and converts
/// the channels independently. Premultiplying happens here, for the blend, and nowhere else.
double blend_channel(double b, double s, ldoc::BlendMode mode) {
    switch (mode) {
    case ldoc::BlendMode::LinearDodge:
        return std::min(1.0, b + s);
    case ldoc::BlendMode::Subtract:
        return std::max(0.0, b - s);
    case ldoc::BlendMode::Screen:
        return b + s - b * s;
    case ldoc::BlendMode::Multiply:
        return b * s;
    case ldoc::BlendMode::Lighten:
        return std::max(b, s);
    case ldoc::BlendMode::Darken:
        return std::min(b, s);
    default:
        return s;
    }
}

void blend_linear(std::uint8_t* d, double sr, double sg, double sb, double sa,
                  ldoc::BlendMode mode) {
    if (sa >= 1.0 && mode == ldoc::BlendMode::Normal) {
        const auto bytes = ldoc::Rgba{sr, sg, sb, 1.0}.to_srgb8();
        d[0] = bytes[0];
        d[1] = bytes[1];
        d[2] = bytes[2];
        d[3] = 255;
        return;
    }
    if (sa <= 0.0)
        return;

    const double dr = srgb_byte(d[0]);
    const double dg = srgb_byte(d[1]);
    const double db = srgb_byte(d[2]);
    const double da = static_cast<double>(d[3]) / 255.0;
    if (mode != ldoc::BlendMode::Normal) {
        sr += da * (blend_channel(dr, sr, mode) - sr);
        sg += da * (blend_channel(dg, sg, mode) - sg);
        sb += da * (blend_channel(db, sb, mode) - sb);
    }
    const double inv = 1.0 - sa;
    const double or_ = sr * sa + dr * da * inv;
    const double og = sg * sa + dg * da * inv;
    const double ob = sb * sa + db * da * inv;
    const double oa = sa + da * inv;
    if (oa <= 0.0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    const auto bytes = ldoc::Rgba{or_ / oa, og / oa, ob / oa, oa}.to_srgb8();
    d[0] = bytes[0];
    d[1] = bytes[1];
    d[2] = bytes[2];
    d[3] = bytes[3];
}

void fill_rect(Picture& picture, const Px& rect, const ldoc::Rgba& colour, double opacity,
               ldoc::BlendMode mode) {
    const double sa = colour.a * std::clamp(opacity, 0.0, 1.0);
    if (sa <= 0.0 || rect.empty())
        return;
    const int x0 = std::max(0, rect.x0);
    const int y0 = std::max(0, rect.y0);
    const int x1 = std::min(picture.width, rect.x1);
    const int y1 = std::min(picture.height, rect.y1);
    for (int y = y0; y < y1; ++y) {
        std::uint8_t* row = picture.pixels.data() + static_cast<std::size_t>(y) * picture.stride;
        for (int x = x0; x < x1; ++x)
            blend_linear(row + static_cast<std::size_t>(x) * 4u, colour.r, colour.g, colour.b, sa,
                         mode);
    }
}

/// One axis of the stretched blit. `flip` reads the far end of the destination first, which
/// is how a negative scale mirrors. The straight path uses this same division.
int sample_axis(int i, int dest_n, int src_n, bool flip) {
    if (dest_n <= 0 || src_n <= 0)
        return 0;
    const int from = flip ? dest_n - 1 - i : i;
    const int sampled = static_cast<int>((static_cast<std::int64_t>(from) * src_n) / dest_n);
    return std::clamp(sampled, 0, src_n - 1);
}

/// Copy an already-sRGB buffer into `picture`. A precomp's offscreen result. Opaque pixels are
/// copied; transparent ones leave the destination, which is why the offscreen buffer starts
/// clear rather than black — an empty nested scene must not punch a hole.
///
/// `quarter` is a clockwise quarter turn of the content inside `rect`, and the flips are a
/// negative scale applied before that turn. A solid colour does not come through here: its
/// bounds are enough.
/// `swap_rb` reads the source as BGRA instead of RGBA.
///
/// A flag rather than a conversion pass, because a captured frame arrives in the compositor's
/// BGRA and most of it is about to be scaled away: swapping the whole buffer first measured
/// 5.1 ms a frame at 4K, against 1.6 ms for the scaling itself. Here it costs nothing -- the
/// sampler is already reading the three bytes one at a time.
/// The source index for every destination row or column along one axis, worked out once.
///
/// `sample_axis` does a 64-bit integer division, and calling it per pixel per axis is where this
/// blit spent 68% of its time -- two `idiv` instructions accounted for that much of every sample
/// in a perf profile, which at 1920x1080 is 4.1 million divisions a frame. The mapping only
/// depends on the destination index, so a 4K-to-1080p blit needs 3000 divisions rather than
/// four million.
///
/// Identical arithmetic, in the same order, so the picture does not change: the table is filled
/// by `sample_axis` itself.
std::vector<int> axis_map(int dest_n, int src_n, int begin, int count) {
    std::vector<int> map(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        map[static_cast<std::size_t>(i)] = sample_axis(begin + i, dest_n, src_n, false);
    return map;
}

void blit(Picture& picture, const Px& rect, const Picture& src, double opacity, int quarter,
          bool flip_x, bool flip_y, bool swap_rb = false,
          ldoc::BlendMode mode = ldoc::BlendMode::Normal) {
    if (rect.empty() || src.width <= 0 || src.height <= 0 || opacity <= 0.0)
        return;
    const int x0 = std::max(0, rect.x0);
    const int y0 = std::max(0, rect.y0);
    const int x1 = std::min(picture.width, rect.x1);
    const int y1 = std::min(picture.height, rect.y1);
    const int dw = rect.x1 - rect.x0;
    const int dh = rect.y1 - rect.y0;
    if (dw <= 0 || dh <= 0 || x1 <= x0 || y1 <= y0)
        return;
    const double covered = std::clamp(opacity, 0.0, 1.0);
    const bool plain = quarter == 0 && !flip_x && !flip_y;

    // Built for the straight path only. A quarter turn reads the other axis of the source for
    // each destination axis, so its four cases would each need their own pair; they are rare
    // enough -- a rotated source, not a scaled one -- to leave on the per-pixel path until a
    // profile says otherwise.
    std::vector<int> map_x;
    std::vector<int> map_y;
    if (plain) {
        map_x = axis_map(dw, src.width, x0 - rect.x0, x1 - x0);
        map_y = axis_map(dh, src.height, y0 - rect.y0, y1 - y0);
    }

    for (int y = y0; y < y1; ++y) {
        const int iy = y - rect.y0;
        std::uint8_t* drow = picture.pixels.data() + static_cast<std::size_t>(y) * picture.stride;
        for (int x = x0; x < x1; ++x) {
            const int ix = x - rect.x0;
            int sx = 0;
            int sy = 0;
            if (plain) {
                sx = map_x[static_cast<std::size_t>(x - x0)];
                sy = map_y[static_cast<std::size_t>(y - y0)];
            } else {
                switch (quarter) {
                case 1:
                    sx = sample_axis(iy, dh, src.width, false);
                    sy = sample_axis(ix, dw, src.height, true);
                    break;
                case 2:
                    sx = sample_axis(ix, dw, src.width, true);
                    sy = sample_axis(iy, dh, src.height, true);
                    break;
                case 3:
                    sx = sample_axis(iy, dh, src.width, true);
                    sy = sample_axis(ix, dw, src.height, false);
                    break;
                default:
                    sx = sample_axis(ix, dw, src.width, false);
                    sy = sample_axis(iy, dh, src.height, false);
                    break;
                }
                if (flip_x)
                    sx = src.width - 1 - sx;
                if (flip_y)
                    sy = src.height - 1 - sy;
            }
            const std::uint8_t* srow =
                src.read_from() + static_cast<std::size_t>(sy) * src.stride;
            const std::uint8_t* s = srow + static_cast<std::size_t>(sx) * 4u;
            std::uint8_t* d = drow + static_cast<std::size_t>(x) * 4u;
            const int r = swap_rb ? 2 : 0;
            const int b = swap_rb ? 0 : 2;
            if (s[3] == 0)
                continue;
            if (s[3] == 255 && covered >= 1.0 && mode == ldoc::BlendMode::Normal) {
                d[0] = s[r];
                d[1] = s[1];
                d[2] = s[b];
                d[3] = 255;
                continue;
            }
            const double sa = (static_cast<double>(s[3]) / 255.0) * covered;
            blend_linear(d, srgb_byte(s[r]), srgb_byte(s[1]), srgb_byte(s[b]), sa, mode);
        }
    }
}

std::optional<double> setting_number(const ldoc::ParamMap& settings, const char* key) {
    const auto it = settings.find(key);
    if (it == settings.end())
        return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&it->second))
        return static_cast<double>(*i);
    if (const auto* n = std::get_if<double>(&it->second))
        return *n;
    return std::nullopt;
}

struct CanvasTarget {
    DrawPlan& picture;
    bool& drew;
    /// Scenes currently being drawn. A precomp that names one of them is a cycle, not a loop.
    std::vector<ldoc::Id>& stack;
    /// Where captured frames come from, or empty when this caller has no device. A screen source
    /// is a gap either way; with a frame source it is a gap only until a frame arrives.
    const FrameSource* frames = nullptr;
};

DrawOp operation(const ldoc::Node& node, const Px& rect, double opacity) {
    DrawOp op;
    op.rect = rect;
    op.crop = {static_cast<int>(node.layout.crop.left), static_cast<int>(node.layout.crop.top),
               static_cast<int>(node.layout.crop.right), static_cast<int>(node.layout.crop.bottom)};
    op.opacity = opacity;
    op.blend = node.blend.mode;
    op.quarter = quarter_turn(node.transform.rotation_deg).value_or(0);
    op.flip_x = node.transform.scale.x < 0.0;
    op.flip_y = node.transform.scale.y < 0.0;
    return op;
}

void paint_scene(const ldoc::Project& project, const ldoc::Scene& scene, double opacity,
                 CanvasTarget& target);

void append_filtered(const ldoc::Node& node, DrawOp op, int width, int height, CanvasTarget& target,
                     std::span<const ldoc::Effect> shared = {}) {
    op.filters.insert(op.filters.end(), shared.begin(), shared.end());
    op.filters.insert(op.filters.end(), node.effects.begin(), node.effects.end());
    std::erase_if(op.filters, [](const auto& e) {
        const auto* info = filter_info(e.type);
        return !e.enabled || (info && info->audio);
    });
    if (!op.filters.empty()) {
        const auto size = filter_size(op.filters, width, height);
        GapReason why = GapReason::NoSize;
        const auto rect = size ? place(node, size->x, size->y, true, why) : std::nullopt;
        if (!rect) {
            refuse(target.picture, node, GapReason::UnsupportedEffect);
            return;
        }
        op.rect = *rect;
        op.image.width = width;
        op.image.height = height;
    }
    target.drew = true;
    target.picture.ops.push_back(std::move(op));
}

// Immutable decoded stills share pixels across plans. Bound retention by count and bytes;
// metadata changes create a new identity, invalidating GPU uploads without changing old plans.
struct ImageCache {
    std::string path;
    FileStamp stamp;
    std::shared_ptr<const DecodedImage> image;
};

std::shared_ptr<const DecodedImage> decode_image_cached(const std::string& path) {
    static std::vector<ImageCache> cache;
    const auto stamp = file_stamp(path);
    const auto found = std::ranges::find(cache, path, &ImageCache::path);
    if (stamp && found != cache.end() && found->stamp == *stamp) {
        const auto image = found->image;
        std::rotate(found, std::next(found), cache.end());
        return image;
    }
    if (found != cache.end())
        cache.erase(found);
    auto decoded = decode_image(path);
    if (!decoded)
        return {};
    auto image = std::make_shared<const DecodedImage>(std::move(*decoded));
    if (stamp) {
        std::size_t bytes = image->pixels.size();
        for (const auto& entry : cache)
            bytes += entry.image->pixels.size();
        while (!cache.empty() && (cache.size() >= 16 || bytes > 128ull * 1024 * 1024)) {
            bytes -= cache.front().image->pixels.size();
            cache.erase(cache.begin());
        }
        if (bytes <= 128ull * 1024 * 1024)
            cache.push_back({path, *stamp, image});
    }
    return image;
}

/// The file named by the image source, at its decoded size. A missing or unreadable file is a
/// gap. The decode is cached, keyed on the file's path, mtime and size.
void paint_image(const ldoc::Source& src, const ldoc::Node& node, double opacity,
                 CanvasTarget& target) {
    const auto it = src.settings.find("file");
    const auto* path = it == src.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    if (path == nullptr || path->empty()) {
        refuse(target.picture, node, GapReason::Unreadable);
        return;
    }
    const auto image = decode_image_cached(*path);
    if (!image) {
        refuse(target.picture, node, GapReason::Unreadable);
        return;
    }
    GapReason why = GapReason::NoSize;
    const std::optional<Px> rect = place(node, static_cast<double>(image->width),
                                         static_cast<double>(image->height), true, why);
    if (!rect) {
        refuse(target.picture, node, why);
        return;
    }
    Picture sampled;
    sampled.width = image->width;
    sampled.height = image->height;
    sampled.stride = static_cast<std::uint32_t>(image->width) * 4u;
    sampled.borrowed = image->pixels.data();
    sampled.owner = image;
    target.drew = true;
    DrawOp op = operation(node, *rect, opacity);
    op.image = std::move(sampled);
    append_filtered(node, std::move(op), image->width, image->height, target, src.effects);
}

/// The latest frame of the screen this source names, placed with the same fit as an image.
///
/// No frame is a gap, not an error and not a black rectangle: ADR-0013 makes capture
/// damage-driven, so a screen nobody has touched legitimately sends nothing, and ADR-0040's rule
/// is that an absence belongs in the caption rather than in the pixels.
void paint_screen(const ldoc::Source& src, const ldoc::Node& node, double opacity,
                  CanvasTarget& target) {
    if (target.frames == nullptr || !*target.frames) {
        // No device behind this walk at all -- a unit test, or the UI's own stand-in. The type is
        // drawable in principle, but not by this caller, which is what UnsupportedType means.
        refuse(target.picture, node, GapReason::UnsupportedType);
        return;
    }
    const CapturedPicture frame = (*target.frames)(src);
    if (frame.empty() || frame.width <= 0 || frame.height <= 0 || frame.width > kMaxPreviewEdge ||
        frame.height > kMaxPreviewEdge) {
        refuse(target.picture, node, GapReason::NoFrame);
        return;
    }
    // A buffer that does not hold the pixels it claims is a bug on the device side, and drawing
    // from it would read past the end. Refused as NoFrame rather than trusted.
    const auto pixels = static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    const auto need = frame.yuv == 0 ? pixels * 4u : frame.yuv == 1 ? pixels * 2u : pixels * 3u / 2u;
    if (!frame.device_image && frame.size < need) {
        refuse(target.picture, node, GapReason::NoFrame);
        return;
    }
    int width = frame.width, height = frame.height;
    if (src.type == "source.media" || src.type == "source.playlist") {
        const auto w = setting_number(src.settings, "width").value_or(1920);
        const auto h = setting_number(src.settings, "height").value_or(1080);
        if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 4096 || h > 4096) {
            refuse(target.picture, node, GapReason::NoSize);
            return;
        }
        width = static_cast<int>(w);
        height = static_cast<int>(h);
    }
    GapReason why = GapReason::NoSize;
    const std::optional<Px> rect =
        place(node, static_cast<double>(width), static_cast<double>(height), true, why);
    if (!rect) {
        refuse(target.picture, node, why);
        return;
    }
    // Borrowed for the length of this call. The frame source holds its lock across it, so the
    // capture thread cannot replace the buffer underneath -- and nothing here copies it.
    Picture sampled;
    sampled.width = frame.width;
    sampled.height = frame.height;
    sampled.stride = static_cast<std::uint32_t>(frame.width) * 4u;
    sampled.borrowed = frame.pixels;
    target.drew = true;
    DrawOp op = operation(node, *rect, opacity);
    op.image = std::move(sampled);
    op.bgra = frame.bgra;
    op.yuv = frame.yuv;
    op.yuv_matrix = frame.yuv_matrix;
    op.device_image = frame.device_image;
    op.source_key = src.id;
    op.frame_owner = frame.owner;
    if (width != frame.width || height != frame.height) {
        // Media keeps its authored output box, but uploads decoded pixels once.
        // Both executors resize/pad before filters and placement use that box.
        const int fit_w = std::min(width, height * frame.width / frame.height);
        const int fit_h = std::min(height, width * frame.height / frame.width);
        if (fit_w < 1 || fit_h < 1) {
            refuse(target.picture, node, GapReason::NoSize);
            return;
        }
        DrawOp image;
        image.rect = {(width - fit_w) / 2, (height - fit_h) / 2, (width - fit_w) / 2 + fit_w,
                      (height - fit_h) / 2 + fit_h};
        image.image = std::move(op.image);
        image.bgra = op.bgra;
        image.yuv = op.yuv;
        image.yuv_matrix = op.yuv_matrix;
        image.device_image = op.device_image;
        ldoc::Effect resize;
        resize.type = "filter.scale";
        resize.params["width"] = static_cast<std::int64_t>(fit_w);
        resize.params["height"] = static_cast<std::int64_t>(fit_h);
        resize.params["sampling"] = std::string("bilinear");
        if (fit_w == width && fit_h == height) {
            // No letterbox: resize directly before source/node filters instead
            // of drawing another full-size isolated surface.
            op.image = std::move(image.image);
            op.filters.push_back(std::move(resize));
            append_filtered(node, std::move(op), frame.width, frame.height, target, src.effects);
            return;
        }
        image.filters.push_back(std::move(resize));
        op.nested = std::make_shared<DrawPlan>();
        op.nested->width = width;
        op.nested->height = height;
        op.nested->status = PictureStatus::Complete;
        op.nested->ops.push_back(std::move(image));
        op.device_image = nullptr;
        op.bgra = false;
        op.yuv = 0;
    }
    append_filtered(node, std::move(op), width, height, target, src.effects);
}

void paint_node(const ldoc::Project& project, const ldoc::Node& node, double parent_opacity,
                CanvasTarget& target) {
    if (!node.visible || parent_opacity <= 0.0)
        return;
    const double opacity = parent_opacity * std::clamp(node.blend.opacity, 0.0, 1.0);
    if (opacity <= 0.0)
        return;

    if (!blend_supported(node.blend.mode) || node.blend.clip_to_below ||
        (node.kind() == ldoc::NodeKind::Group && node.blend.pass_through_group &&
         node.blend.mode != ldoc::BlendMode::Normal)) {
        refuse(target.picture, node, GapReason::UnsupportedBlend);
        return;
    }
    if (effect_enabled(node)) {
        refuse(target.picture, node, GapReason::UnsupportedEffect);
        return;
    }
    if (!transform_supported(node.transform)) {
        refuse(target.picture, node, GapReason::UnsupportedTransform);
        return;
    }

    if (const auto* group = std::get_if<ldoc::GroupData>(&node.data)) {
        const auto size = group->size.value_or(ldoc::Vec2{
            static_cast<double>(target.picture.width), static_cast<double>(target.picture.height)});
        if (!std::isfinite(size.x) || !std::isfinite(size.y) || size.x <= 0 || size.y <= 0 ||
            size.x > 8192 || size.y > 8192 || size.x * size.y > 33554432) {
            refuse(target.picture, node, GapReason::NoSize);
            return;
        }
        GapReason why = GapReason::NoSize;
        const auto rect = place(node, size.x, size.y, true, why);
        if (!rect) { refuse(target.picture, node, why); return; }
        const bool identity = !node.layout.crop.any() && rect->x0 == 0 && rect->y0 == 0 && rect->x1 == target.picture.width &&
            rect->y1 == target.picture.height && quarter_turn(node.transform.rotation_deg) == 0 &&
            node.transform.scale.x > 0 && node.transform.scale.y > 0;
        if (group->size || !identity || group->isolate || !node.effects.empty() ||
            node.blend.mode != ldoc::BlendMode::Normal) {
            DrawPlan inner;
            inner.width = static_cast<int>(std::ceil(size.x));
            inner.height = static_cast<int>(std::ceil(size.y));
            bool drew = false;
            CanvasTarget nested{inner, drew, target.stack, target.frames};
            for (auto it = group->children.rbegin(); it != group->children.rend(); ++it)
                paint_node(project, *it, 1.0, nested);
            for (const auto& gap : inner.gaps) target.picture.gaps.push_back(gap);
            auto op = operation(node, *rect, opacity);
            op.nested = std::make_shared<DrawPlan>(std::move(inner));
            append_filtered(node, std::move(op), static_cast<int>(std::ceil(size.x)),
                            static_cast<int>(std::ceil(size.y)), target);
            target.drew = target.drew || drew;
            return;
        }
        // Children are listed topmost first. Walk them back to front so the top is drawn last.
        for (auto it = group->children.rbegin(); it != group->children.rend(); ++it)
            paint_node(project, *it, opacity, target);
        return;
    }

    if (const auto* source = std::get_if<ldoc::SourceData>(&node.data)) {
        if (!source->source) {
            refuse(target.picture, node, GapReason::NoSource);
            return;
        }
        const ldoc::Source* src = project.source(*source->source);
        if (!src) {
            refuse(target.picture, node, GapReason::NoSource);
            return;
        }
        const auto resolved = src->type == "source.clone"
                                  ? ldoc::resolve_source(project, *src)
                                  : std::optional<ldoc::Source>{};
        if (src->type == "source.clone" && !resolved) {
            refuse(target.picture, node, GapReason::NoSource);
            return;
        }
        if (resolved)
            src = &*resolved;
        if (audio_only(src->type))
            return;
        if (!video_filters_supported(src->effects)) {
            // Filters do not deactivate their input worker or its audio capture.
            if (target.frames && *target.frames &&
                (src->type == "source.text" || src->type == "source.media" ||
                 src->type == "source.playlist" || src->type == "source.slideshow" ||
                 src->type == "source.browser" || src->type == "source.blackmagic"))
                (*target.frames)(*src);
            refuse(target.picture, node, GapReason::UnsupportedEffect);
            return;
        }
        if (src->type == "source.image") {
            paint_image(*src, node, opacity, target);
            return;
        }
        if (src->type == "source.screen" || src->type == "source.v4l2" ||
            src->type == "source.window" || src->type == "source.game" ||
            src->type == "source.text" || src->type == "source.media" ||
            src->type == "source.slideshow" || src->type == "source.playlist" || src->type == "source.browser" || src->type == "source.blackmagic") {
            paint_screen(*src, node, opacity, target);
            return;
        }
        if (src->type != "source.color") {
            refuse(target.picture, node, GapReason::UnsupportedType);
            return;
        }
        const auto colour_it = src->settings.find("color");
        const ldoc::Rgba* colour = colour_it == src->settings.end()
                                       ? nullptr
                                       : std::get_if<ldoc::Rgba>(&colour_it->second);
        if (!colour) {
            refuse(target.picture, node, GapReason::NoColour);
            return;
        }
        const auto width = setting_number(src->settings, "width");
        const auto height = setting_number(src->settings, "height");
        const bool have_natural = width && height && *width > 0.0 && *height > 0.0;
        GapReason why = GapReason::NoSize;
        const std::optional<Px> rect = place(node, have_natural ? *width : 0.0,
                                             have_natural ? *height : 0.0, have_natural, why);
        if (!rect) {
            refuse(target.picture, node, why);
            return;
        }
        target.drew = true;
        DrawOp op = operation(node, *rect, opacity);
        op.colour = *colour;
        append_filtered(
            node, std::move(op), have_natural ? static_cast<int>(*width) : rect->x1 - rect->x0,
            have_natural ? static_cast<int>(*height) : rect->y1 - rect->y0, target, src->effects);
        return;
    }

    if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data)) {
        if (std::ranges::find(target.stack, pre->scene) != target.stack.end()) {
            refuse(target.picture, node, GapReason::Cycle);
            return;
        }
        const ldoc::Scene* inner = project.scene(pre->scene);
        if (!inner) {
            refuse(target.picture, node, GapReason::MissingScene);
            return;
        }
        // (0, 0) means the canvas this precomp is being drawn into. That is what PrecompData
        // says, and a second opinion here would be a second size.
        int pw = target.picture.width;
        int ph = target.picture.height;
        if (pre->size.x > 0.0 && pre->size.y > 0.0) {
            if (pre->size.x > kMaxPreviewEdge || pre->size.y > kMaxPreviewEdge) {
                refuse(target.picture, node, GapReason::NoSize);
                return;
            }
            pw = static_cast<int>(std::lround(pre->size.x));
            ph = static_cast<int>(std::lround(pre->size.y));
        }
        if (pw <= 0 || ph <= 0 || pw > kMaxPreviewEdge || ph > kMaxPreviewEdge) {
            refuse(target.picture, node, GapReason::NoSize);
            return;
        }
        DrawPlan offscreen;
        offscreen.width = pw;
        offscreen.height = ph;

        // A local, not `target.drew`. The inner scene's emptiness is not the outer scene's, and
        // the reference cannot be reseated — assigning through it would wipe a node already
        // drawn behind this precomp.
        bool nested_drew = false;
        target.stack.push_back(inner->id);
        CanvasTarget nested{offscreen, nested_drew, target.stack, target.frames};
        paint_scene(project, *inner, 1.0, nested);
        target.stack.pop_back();
        for (Gap& gap : offscreen.gaps)
            target.picture.gaps.push_back(std::move(gap));

        GapReason why = GapReason::NoSize;
        const std::optional<Px> rect =
            place(node, static_cast<double>(pw), static_cast<double>(ph), true, why);
        if (!rect) {
            refuse(target.picture, node, why);
            return;
        }
        target.drew = true;
        DrawOp op = operation(node, *rect, opacity);
        // Normal source-over is associative. An unchanged, unfiltered scene needs
        // no isolation surface unless one of its children blends with that surface.
        if (node.effects.empty() && offscreen.filters.empty() && op.opacity == 1 &&
            op.blend == ldoc::BlendMode::Normal && op.quarter == 0 && !op.flip_x && !op.flip_y &&
            op.crop == std::array<int, 4>{} && rect->x0 == 0 && rect->y0 == 0 && rect->x1 == pw &&
            rect->y1 == ph && pw == target.picture.width && ph == target.picture.height &&
            std::ranges::all_of(offscreen.ops, [](const auto& child) {
                return child.blend == ldoc::BlendMode::Normal;
            })) {
            for (auto& child : offscreen.ops)
                target.picture.ops.push_back(std::move(child));
            return;
        }
        op.nested = std::make_shared<DrawPlan>(std::move(offscreen));
        append_filtered(node, std::move(op), pw, ph, target);
        return;
    }

    // Adjustment, mask, reference: each changes the picture in a way this composite does not
    // do. A reference in particular must not draw the target twice and call it the same.
    refuse(target.picture, node, GapReason::UnsupportedType);
}

void paint_scene(const ldoc::Project& project, const ldoc::Scene& scene, double opacity,
                 CanvasTarget& target) {
    const auto first = target.picture.ops.size();
    const bool previously_drew = target.drew;
    for (auto it = scene.roots.rbegin(); it != scene.roots.rend(); ++it)
        paint_node(project, *it, opacity, target);
    if (!video_filters_supported(scene.effects)) {
        target.picture.ops.resize(first);
        target.drew = previously_drew;
        target.picture.gaps.push_back({scene.id, scene.name, GapReason::UnsupportedEffect});
    } else if (std::ranges::any_of(scene.effects, [](const auto& e) { return e.enabled; })) {
        const auto size = filter_size(scene.effects, target.picture.width, target.picture.height);
        if (!size) {
            target.picture.ops.resize(first);
            target.drew = previously_drew;
            target.picture.gaps.push_back({scene.id, scene.name, GapReason::UnsupportedEffect});
            return;
        }
        DrawPlan inner;
        inner.width = target.picture.width;
        inner.height = target.picture.height;
        inner.filters = scene.effects;
        inner.ops.assign(std::make_move_iterator(target.picture.ops.begin() +
                                                 static_cast<std::ptrdiff_t>(first)),
                         std::make_move_iterator(target.picture.ops.end()));
        target.picture.ops.resize(first);
        DrawOp op;
        op.rect = {0, 0, static_cast<int>(size->x), static_cast<int>(size->y)};
        op.nested = std::make_shared<DrawPlan>(std::move(inner));
        target.picture.ops.push_back(std::move(op));
    }
}

void fill_opaque_black(Picture& picture) {
    const std::size_t n = picture.pixels.size();
    for (std::size_t i = 3; i < n; i += 4)
        picture.pixels[i] = 255;
}

} // namespace

Picture composite_canvas(const ldoc::Project& project, const ldoc::Id& canvas_id) {
    return composite_canvas(project, canvas_id, FrameSource{});
}

DrawPlan plan_canvas(const ldoc::Project& project, const ldoc::Id& canvas_id,
                     const FrameSource& frames) {
    DrawPlan picture;
    const ldoc::Canvas* canvas = project.canvas(canvas_id);
    if (!canvas) {
        picture.status = PictureStatus::Unsupported;
        return picture;
    }
    picture.width = canvas->width;
    picture.height = canvas->height;
    if (canvas->width <= 0 || canvas->height <= 0 || canvas->width > kMaxPreviewEdge ||
        canvas->height > kMaxPreviewEdge) {
        picture.status = PictureStatus::Unsupported;
        return picture;
    }
    const auto bytes =
        static_cast<std::uint64_t>(canvas->width) * static_cast<std::uint64_t>(canvas->height) * 4u;
    constexpr std::uint64_t kMaxBytes = 128ull * 1024ull * 1024ull;
    if (bytes > kMaxBytes ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        picture.status = PictureStatus::Unsupported;
        return picture;
    }

    const ldoc::Deck* deck = nullptr;
    for (const ldoc::Deck& candidate : project.decks) {
        if (candidate.canvas == canvas_id) {
            deck = &candidate;
            break;
        }
    }
    if (!deck || !deck->program_scene) {
        picture.status = PictureStatus::NoProgram;
        return picture;
    }
    const ldoc::Scene* scene = project.scene(*deck->program_scene);
    if (!scene) {
        picture.status = PictureStatus::NoProgram;
        return picture;
    }

    bool drew = false;
    std::vector<ldoc::Id> stack;
    stack.push_back(scene->id);
    CanvasTarget target{picture, drew, stack, &frames};
    paint_scene(project, *scene, 1.0, target);

    if (!picture.gaps.empty())
        picture.status = PictureStatus::Partial;
    else if (!drew)
        picture.status = PictureStatus::Empty;
    else
        picture.status = PictureStatus::Complete;
    const auto now =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
    const auto stamp = [&](auto&& self, DrawPlan& p) -> void {
        p.time_ns = now;
        for (auto& op : p.ops)
            if (op.nested)
                self(self, *op.nested);
    };
    stamp(stamp, picture);
    return picture;
}

Picture execute_cpu(const DrawPlan& plan, bool transparent) {
    Picture picture;
    picture.width = plan.width;
    picture.height = plan.height;
    picture.status = plan.status;
    picture.gaps = plan.gaps;
    const auto add_gaps = [&](const auto& gaps) {
        for (const auto& gap : gaps)
            if (std::ranges::none_of(picture.gaps, [&](const auto& existing) {
                    return existing.node == gap.node && existing.reason == gap.reason;
                }))
                picture.gaps.push_back(gap);
        if (!gaps.empty())
            picture.status = PictureStatus::Partial;
    };
    if (plan.status == PictureStatus::Unsupported)
        return picture;
    picture.stride = static_cast<std::uint32_t>(plan.width) * 4u;
    picture.pixels.assign(
        static_cast<std::size_t>(picture.stride) * static_cast<std::size_t>(plan.height), 0);
    if (!transparent)
        fill_opaque_black(picture);
    for (const DrawOp& op : plan.ops) {
        if (!op.filters.empty()) {
            DrawOp raw = op;
            raw.filters.clear();
            raw.opacity = 1;
            raw.blend = ldoc::BlendMode::Normal;
            raw.crop = {};
            raw.quarter = 0;
            raw.flip_x = raw.flip_y = false;
            const int w = op.nested ? op.nested->width : op.image.width;
            const int h = op.nested ? op.nested->height : op.image.height;
            raw.rect = {0, 0, w, h};
            DrawPlan local;
            local.time_ns = plan.time_ns;
            local.width = w;
            local.height = h;
            local.ops.push_back(std::move(raw));
            local.filters = op.filters;
            Picture filtered = execute_cpu(local, true);
            add_gaps(filtered.gaps);
            Picture view;
            view.width = filtered.width - op.crop[0] - op.crop[2];
            view.height = filtered.height - op.crop[1] - op.crop[3];
            view.stride = filtered.stride;
            if (view.width > 0 && view.height > 0) {
                view.borrowed = filtered.read_from() +
                                static_cast<std::size_t>(op.crop[1]) * filtered.stride +
                                static_cast<std::size_t>(op.crop[0]) * 4;
                blit(picture, op.rect, view, op.opacity, op.quarter, op.flip_x, op.flip_y, false,
                     op.blend);
            }
            continue;
        }
        if (op.device_image)
            throw std::invalid_argument("CPU executor requires host pixels, not a device image");
        if (op.colour) {
            fill_rect(picture, op.rect, *op.colour, op.opacity, op.blend);
        } else {
            const Picture nested = op.nested ? execute_cpu(*op.nested, true) : Picture{};
            add_gaps(nested.gaps);
            const Picture& source = op.nested ? nested : op.image;
            Picture view;
            view.width = source.width - op.crop[0] - op.crop[2];
            view.height = source.height - op.crop[1] - op.crop[3];
            view.stride = source.stride;
            if (source.read_from() && view.width > 0 && view.height > 0) {
                view.borrowed = source.read_from() + static_cast<std::size_t>(op.crop[1]) *
                    source.stride + static_cast<std::size_t>(op.crop[0]) * 4;
                blit(picture, op.rect, view, op.opacity, op.quarter, op.flip_x, op.flip_y, op.bgra,
                     op.blend);
            }
        }
    }
    for (const auto& effect : plan.filters)
        if (effect.enabled && (!filter_info(effect.type) || !filter_info(effect.type)->audio)) {
            const auto filter = resolve_filter(effect, picture.width, picture.height, plan.time_ns);
            if (filter.kind != 0)
                picture = filter_picture(picture, filter);
        }
    return picture;
}

Picture composite_canvas(const ldoc::Project& project, const ldoc::Id& canvas,
                         const FrameSource& frames) {
    return execute_cpu(plan_canvas(project, canvas, frames));
}

Picture preview_size(Picture picture, std::uint32_t max_edge) {
    const int edge = std::max(picture.width, picture.height);
    if (!max_edge || edge <= 0 || max_edge >= static_cast<std::uint32_t>(edge) ||
        picture.pixels.empty())
        return picture;
    Picture out;
    out.width =
        std::max(1, static_cast<int>(static_cast<std::int64_t>(picture.width) * max_edge / edge));
    out.height =
        std::max(1, static_cast<int>(static_cast<std::int64_t>(picture.height) * max_edge / edge));
    out.stride = static_cast<std::uint32_t>(out.width) * 4u;
    out.status = picture.status;
    out.gaps = std::move(picture.gaps);
    out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height));
    blit(out, {0, 0, out.width, out.height}, picture, 1, 0, false, false);
    return out;
}

} // namespace lcomp
