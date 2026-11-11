// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llayout/layout.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_set>

namespace llayout {
namespace {

constexpr double kFractionEpsilon = 1e-6;
constexpr int kNormalizePasses = 32;

std::ptrdiff_t off(std::size_t i) { return static_cast<std::ptrdiff_t>(i); }

double sum_of(const std::vector<double>& v) {
    double s = 0.0;
    for (const double x : v)
        s += x;
    return s;
}

/// Force a fraction vector to be strictly positive and sum to 1. Degenerate input becomes an
/// even split, which is wrong but never infinite or negative.
void renormalize(std::vector<double>& f) {
    if (f.empty())
        return;
    for (double& x : f) {
        if (!(x > 0.0) || !std::isfinite(x))
            x = kFractionEpsilon;
    }
    const double s = sum_of(f);
    if (!(s > 0.0) || !std::isfinite(s)) {
        const double even = 1.0 / static_cast<double>(f.size());
        std::fill(f.begin(), f.end(), even);
        return;
    }
    for (double& x : f)
        x /= s;
}

} // namespace

Constraints Constraints::studio_defaults() {
    Constraints cs;
    cs.set_fallback(PanelMin{180.0, 110.0});
    // The program canvas is the one panel where a too-small size is not merely ugly: an operator
    // who cannot see the frame cannot run the show.
    cs.set("program", PanelMin{320.0, 200.0});
    // The mixer keeps the small minimum it started with, and the panel adapts instead: it drops
    // the routing line, then each strip's kind, feed count and pan, before it will squeeze a
    // fader. Raising this to what a full strip wants (160) was tried and reverted -- it stopped
    // a drop above the mixer from splitting that row in half, because the engine then had to
    // water-fill against the minimum. A panel's minimum is a constraint on everything docked
    // near it, not just on itself.
    cs.set("audio-mixer", PanelMin{260.0, 130.0});
    cs.set("controls", PanelMin{180.0, 130.0});
    cs.set("outputs", PanelMin{220.0, 130.0});
    // The canvas rows are one line of name plus one of geometry, and the footer is the same
    // four buttons the outputs panel has -- so it asks for the same width and a little less
    // height than outputs, whose rows can expand.
    cs.set("canvas", PanelMin{220.0, 110.0});
    // The five-column output table wants about 340px, and the panel folds it into two lines per
    // output below that rather than demanding the space. Asking for 300 here was tried and
    // reverted: it does not fit the default layout's right-hand column, which is 18% of the
    // window, and the engine then water-fills against it and the whole layout stops scaling.
    cs.set("broadcast-stats", PanelMin{200.0, 160.0});
    return cs;
}

// ---- construction ---------------------------------------------------------------------------

Layout::Layout() {
    Node n;
    n.kind = NodeKind::Tabs;
    root_ = alloc(std::move(n));
}

Layout Layout::default_build() {
    Layout l;

    auto tabs = [&l](std::initializer_list<const char*> names, std::size_t active = 0) {
        Node n;
        n.kind = NodeKind::Tabs;
        for (const char* s : names)
            n.tabs.emplace_back(s);
        n.active = active < n.tabs.size() ? active : 0;
        return l.alloc(std::move(n));
    };
    auto split = [&l](Axis axis, std::vector<NodeId> kids, std::vector<double> fr) {
        Node n;
        n.kind = NodeKind::Split;
        n.axis = axis;
        n.children = std::move(kids);
        n.fractions = std::move(fr);
        return l.alloc(std::move(n));
    };

    // This is the operator's own arrangement, recorded 2026-10-22 and shipped as the default
    // so that a fresh config, or Reset Docks, lands here rather than somewhere they have to
    // rebuild by hand. Change it only when they ask.
    const NodeId scenes = tabs({"scenes", "favourites"});
    const NodeId sources = tabs({"sources"});
    const NodeId program = tabs({"program"});
    const NodeId inspect = tabs({"properties", "info", "log", "countdowns"}, 3);
    const NodeId outputs = tabs({"outputs"});
    const NodeId controls = tabs({"controls"});
    const NodeId mixer = tabs({"audio-mixer"});
    const NodeId stats = tabs({"broadcast-stats"});

    const NodeId left = split(Axis::Vertical, {scenes, sources}, {0.5, 0.5});
    const NodeId monitors = split(Axis::Horizontal, {mixer, stats}, {0.5, 0.5});
    const NodeId centre = split(Axis::Vertical, {program, monitors}, {0.657, 0.343});
    const NodeId right = split(Axis::Vertical, {inspect, outputs, controls},
                               {0.343, 0.405, 0.252});
    const NodeId root = split(Axis::Horizontal, {left, centre, right},
                              {0.147, 0.673, 0.180});

    // Drop the empty root the default constructor made; this tree replaces it.
    l.erase_subtree(l.root_);
    l.root_ = root;
    l.normalize();
    return l;
}

Layout Layout::default_live() {
    Layout l;

    auto tabs = [&l](std::initializer_list<const char*> names) {
        Node n;
        n.kind = NodeKind::Tabs;
        for (const char* s : names)
            n.tabs.emplace_back(s);
        return l.alloc(std::move(n));
    };
    auto split = [&l](Axis axis, std::vector<NodeId> kids, std::vector<double> fr) {
        Node n;
        n.kind = NodeKind::Split;
        n.axis = axis;
        n.children = std::move(kids);
        n.fractions = std::move(fr);
        return l.alloc(std::move(n));
    };

    // Favourites rather than the scene and source lists: during a show the useful set is small
    // and known in advance, and every row that is not going on air is a row to mis-click.
    const NodeId favourites = tabs({"favourites"});
    const NodeId program = tabs({"program"});
    // Canvas sits beside Outputs: the operator's choice, and the two panels share a footer
    // shape -- + - gear, with the gear opening a manager window rather than editing in place.
    const NodeId controls = tabs({"controls", "outputs", "canvas"});
    const NodeId mixer = tabs({"audio-mixer"});
    const NodeId stats = tabs({"broadcast-stats"});

    const NodeId upper = split(Axis::Horizontal, {favourites, program, controls},
                               {0.16, 0.64, 0.20});
    const NodeId lower = split(Axis::Horizontal, {mixer, stats}, {0.58, 0.42});
    const NodeId root = split(Axis::Vertical, {upper, lower}, {0.74, 0.26});

    l.erase_subtree(l.root_);
    l.root_ = root;
    l.normalize();
    return l;
}

NodeId Layout::alloc(Node n) {
    const NodeId id = next_id_++;
    n.id = id;
    nodes_.emplace(id, std::move(n));
    return id;
}

const Node* Layout::find(NodeId id) const {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

Node* Layout::mutable_find(NodeId id) {
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : &it->second;
}

// ---- structure ------------------------------------------------------------------------------

void Layout::collect(NodeId id, std::vector<NodeId>& out) const {
    const Node* n = find(id);
    if (!n)
        return;
    out.push_back(id);
    if (n->is_split()) {
        for (const NodeId c : n->children)
            collect(c, out);
    }
}

std::vector<NodeId> Layout::node_ids() const {
    std::vector<NodeId> out;
    out.reserve(nodes_.size());
    collect(root_, out);
    return out;
}

std::vector<std::string> Layout::panels() const {
    std::vector<std::string> out;
    for (const NodeId id : node_ids()) {
        const Node* n = find(id);
        if (n && n->is_tabs())
            out.insert(out.end(), n->tabs.begin(), n->tabs.end());
    }
    return out;
}

NodeId Layout::node_of_panel(std::string_view panel) const {
    for (const NodeId id : node_ids()) {
        const Node* n = find(id);
        if (!n || !n->is_tabs())
            continue;
        if (std::find(n->tabs.begin(), n->tabs.end(), panel) != n->tabs.end())
            return id;
    }
    return kInvalidNode;
}

bool Layout::has_panel(std::string_view panel) const {
    return node_of_panel(panel) != kInvalidNode;
}

std::size_t Layout::tab_index_of(std::string_view panel) const {
    const NodeId id = node_of_panel(panel);
    const Node* n = find(id);
    if (!n)
        return std::string::npos;
    const auto it = std::find(n->tabs.begin(), n->tabs.end(), panel);
    if (it == n->tabs.end())
        return std::string::npos;
    return static_cast<std::size_t>(it - n->tabs.begin());
}

// ---- geometry -------------------------------------------------------------------------------

double Layout::min_extent(NodeId id, Axis axis, const Viewport& vp, const Constraints& cs) const {
    const Node* n = find(id);
    if (!n)
        return 0.0;

    if (n->is_tabs()) {
        if (n->tabs.empty())
            return 0.0;
        double m = 0.0;
        for (const std::string& p : n->tabs) {
            const PanelMin pm = cs.get(p);
            m = std::max(m, axis == Axis::Horizontal ? pm.w : pm.h);
        }
        // A tab bar is not free. Every group pays for one out of its vertical space, whether it
        // has one tab or six, because the bar is where the drag handle lives.
        if (axis == Axis::Vertical)
            m += vp.tab_bar_px;
        return m;
    }

    const std::size_t k = n->children.size();
    if (k == 0)
        return 0.0;

    if (n->axis == axis) {
        double s = vp.divider_px * static_cast<double>(k - 1);
        for (const NodeId c : n->children)
            s += min_extent(c, axis, vp, cs);
        return s;
    }
    double m = 0.0;
    for (const NodeId c : n->children)
        m = std::max(m, min_extent(c, axis, vp, cs));
    return m;
}

bool Layout::fits(const Viewport& vp, const Constraints& cs) const {
    // An unknown viewport imposes no constraint. Before the window has a size, every layout is
    // feasible; refusing drops at that point would just make the engine untestable.
    if (vp.bounds.empty())
        return true;
    return min_extent(root_, Axis::Horizontal, vp, cs) <= vp.bounds.w + kFractionEpsilon &&
           min_extent(root_, Axis::Vertical, vp, cs) <= vp.bounds.h + kFractionEpsilon;
}

std::vector<double> Layout::distribute(const Node& split, double avail,
                                       const std::vector<double>& mins) const {
    const std::size_t k = split.children.size();
    std::vector<double> out(k, 0.0);
    if (k == 0)
        return out;
    if (avail < 0.0)
        avail = 0.0;

    const double min_sum = sum_of(mins);
    if (min_sum >= avail) {
        // Cannot honour every minimum. Hand out space in proportion to the minima so the result
        // is at least deterministic and ordered. A dock that would land here was refused by
        // fits(); arriving here means the *window* shrank, which we cannot refuse.
        if (min_sum <= 0.0) {
            std::fill(out.begin(), out.end(), avail / static_cast<double>(k));
        } else {
            for (std::size_t i = 0; i < k; ++i)
                out[i] = avail * mins[i] / min_sum;
        }
        return out;
    }

    // Water-filling: hand out by fraction, pin anyone who would fall below their minimum, and
    // redistribute what is left among the rest. At most k rounds, and k is small.
    std::vector<bool> pinned(k, false);
    double remaining = avail;
    for (std::size_t round = 0; round < k; ++round) {
        double weight = 0.0;
        for (std::size_t i = 0; i < k; ++i)
            if (!pinned[i])
                weight += split.fractions[i];
        if (!(weight > 0.0))
            break;

        std::size_t worst = k;
        double worst_deficit = 0.0;
        for (std::size_t i = 0; i < k; ++i) {
            if (pinned[i])
                continue;
            const double share = remaining * split.fractions[i] / weight;
            const double deficit = mins[i] - share;
            if (deficit > worst_deficit) {
                worst_deficit = deficit;
                worst = i;
            }
        }
        if (worst == k)
            break;
        pinned[worst] = true;
        out[worst] = mins[worst];
        remaining -= mins[worst];
    }

    double weight = 0.0;
    std::size_t free_count = 0;
    for (std::size_t i = 0; i < k; ++i) {
        if (!pinned[i]) {
            weight += split.fractions[i];
            ++free_count;
        }
    }
    for (std::size_t i = 0; i < k; ++i) {
        if (pinned[i])
            continue;
        out[i] = (weight > 0.0) ? remaining * split.fractions[i] / weight
                                : remaining / static_cast<double>(free_count);
    }
    return out;
}

void Layout::visit_from(NodeId id, const Rect& r, const Viewport& vp, const Constraints& cs,
                        const std::function<void(const Node&, const Rect&)>& fn) const {
    const Node* n = find(id);
    if (!n)
        return;
    fn(*n, r);
    if (!n->is_split())
        return;

    const std::size_t k = n->children.size();
    if (k == 0)
        return;

    const double avail =
        extent_along(r, n->axis) - vp.divider_px * static_cast<double>(k - 1);

    std::vector<double> mins;
    mins.reserve(k);
    for (const NodeId c : n->children)
        mins.push_back(min_extent(c, n->axis, vp, cs));

    const std::vector<double> ext = distribute(*n, avail, mins);

    double pos = origin_along(r, n->axis);
    for (std::size_t i = 0; i < k; ++i) {
        Rect cr = r;
        if (n->axis == Axis::Horizontal) {
            cr.x = pos;
            cr.w = ext[i];
        } else {
            cr.y = pos;
            cr.h = ext[i];
        }
        visit_from(n->children[i], cr, vp, cs, fn);
        pos += ext[i] + vp.divider_px;
    }
}

void Layout::visit(const Viewport& vp, const Constraints& cs,
                   const std::function<void(const Node&, const Rect&)>& fn) const {
    visit_from(root_, vp.bounds, vp, cs, fn);
}

std::vector<LeafRect> Layout::leaves(const Viewport& vp, const Constraints& cs) const {
    std::vector<LeafRect> out;
    visit(vp, cs, [&out](const Node& n, const Rect& r) {
        if (n.is_tabs())
            out.push_back(LeafRect{n.id, r});
    });
    return out;
}

std::optional<Rect> Layout::rect_of(NodeId id, const Viewport& vp, const Constraints& cs) const {
    std::optional<Rect> found;
    visit(vp, cs, [&](const Node& n, const Rect& r) {
        if (n.id == id)
            found = r;
    });
    return found;
}

std::vector<DividerHandle> Layout::dividers(const Viewport& vp, const Constraints& cs) const {
    std::unordered_map<NodeId, Rect> rects;
    std::vector<NodeId> splits;
    visit(vp, cs, [&](const Node& n, const Rect& r) {
        rects.emplace(n.id, r);
        if (n.is_split())
            splits.push_back(n.id);
    });

    std::vector<DividerHandle> out;
    for (const NodeId sid : splits) {
        const Node* s = find(sid);
        if (!s)
            continue;
        for (std::size_t i = 0; i + 1 < s->children.size(); ++i) {
            const auto a = rects.find(s->children[i]);
            const auto b = rects.find(s->children[i + 1]);
            if (a == rects.end() || b == rects.end())
                continue;
            Rect h;
            if (s->axis == Axis::Horizontal) {
                h = Rect{a->second.right(), a->second.y, b->second.x - a->second.right(),
                         a->second.h};
            } else {
                h = Rect{a->second.x, a->second.bottom(), a->second.w,
                         b->second.y - a->second.bottom()};
            }
            out.push_back(DividerHandle{sid, i, s->axis, h});
        }
    }
    return out;
}

// ---- hit testing ----------------------------------------------------------------------------

std::optional<DropTarget> Layout::hit_test(double x, double y, const Viewport& vp,
                                           const Constraints& cs, const ZoneMetrics& zm) const {
    const Rect& b = vp.bounds;
    if (b.empty() || !b.contains(x, y))
        return std::nullopt;

    // Workspace edges win. A panel zone can always be reached by aiming slightly inward; an edge
    // dock has nowhere else to live, and without it there is no gesture that creates a
    // full-height column beside an existing row (ADR-0016 rule 6).
    auto edge_target = [&b, &vp](Edge e) {
        DropTarget t;
        t.is_edge = true;
        t.edge = e;
        const double sw = b.w * vp.edge_share;
        const double sh = b.h * vp.edge_share;
        switch (e) {
        case Edge::Left:   t.preview = Rect{b.x, b.y, sw, b.h}; break;
        case Edge::Right:  t.preview = Rect{b.right() - sw, b.y, sw, b.h}; break;
        case Edge::Top:    t.preview = Rect{b.x, b.y, b.w, sh}; break;
        case Edge::Bottom: t.preview = Rect{b.x, b.bottom() - sh, b.w, sh}; break;
        }
        return t;
    };

    if (x - b.x < vp.edge_px)
        return edge_target(Edge::Left);
    if (b.right() - x < vp.edge_px)
        return edge_target(Edge::Right);
    if (y - b.y < vp.edge_px)
        return edge_target(Edge::Top);
    if (b.bottom() - y < vp.edge_px)
        return edge_target(Edge::Bottom);

    for (const LeafRect& lf : leaves(vp, cs)) {
        if (!lf.rect.contains(x, y))
            continue;
        DropTarget t;
        t.node = lf.node;
        t.zone = hit_test_zone(lf.rect, x, y, zm);
        t.preview = zone_preview(lf.rect, t.zone);
        return t;
    }
    // Over a divider: no drop target, which is correct. Dividers are for resizing.
    return std::nullopt;
}

std::optional<NodeId> Layout::neighbour(NodeId from, Edge direction, const Viewport& vp,
                                        const Constraints& cs) const {
    const auto origin = rect_of(from, vp, cs);
    if (!origin || origin->empty())
        return std::nullopt;

    const bool horizontal = axis_of(direction) == Axis::Horizontal;
    const double tolerance = vp.divider_px + 1.0;

    std::optional<NodeId> best;
    double best_gap = 0.0;
    double best_offset = 0.0;

    for (const LeafRect& lf : leaves(vp, cs)) {
        if (lf.node == from || lf.rect.empty())
            continue;

        // It has to be on the right side, and it has to actually be beside us: a panel in the
        // far corner is not the one to the left, however far left it is.
        double gap = 0.0;
        bool overlaps = false;
        if (horizontal) {
            gap = direction == Edge::Left ? origin->x - lf.rect.right()
                                          : lf.rect.x - origin->right();
            overlaps = lf.rect.y < origin->bottom() - 1.0 && origin->y < lf.rect.bottom() - 1.0;
        } else {
            gap = direction == Edge::Top ? origin->y - lf.rect.bottom()
                                         : lf.rect.y - origin->bottom();
            overlaps = lf.rect.x < origin->right() - 1.0 && origin->x < lf.rect.right() - 1.0;
        }
        if (gap < -tolerance || !overlaps)
            continue;

        // Nearest wins; among equally near ones, the best aligned, so repeatedly moving in one
        // direction walks a predictable path rather than wandering between rows.
        const double offset =
            horizontal
                ? std::abs((lf.rect.y + lf.rect.h * 0.5) - (origin->y + origin->h * 0.5))
                : std::abs((lf.rect.x + lf.rect.w * 0.5) - (origin->x + origin->w * 0.5));
        if (!best || gap < best_gap - 1.0 ||
            (std::abs(gap - best_gap) <= 1.0 && offset < best_offset)) {
            best = lf.node;
            best_gap = gap;
            best_offset = offset;
        }
    }
    return best;
}

std::optional<DividerHandle> Layout::hit_test_divider(double x, double y, const Viewport& vp,
                                                      const Constraints& cs,
                                                      double slop_px) const {
    for (const DividerHandle& d : dividers(vp, cs)) {
        Rect r = d.rect;
        if (d.axis == Axis::Horizontal) {
            r.x -= slop_px;
            r.w += slop_px * 2.0;
        } else {
            r.y -= slop_px;
            r.h += slop_px * 2.0;
        }
        if (r.contains(x, y))
            return d;
    }
    return std::nullopt;
}

// ---- mutation -------------------------------------------------------------------------------

void Layout::erase_subtree(NodeId id) {
    const Node* n = find(id);
    if (!n)
        return;
    const std::vector<NodeId> kids = n->children;
    nodes_.erase(id);
    for (const NodeId c : kids)
        erase_subtree(c);
}

void Layout::replace_in_parent(NodeId target, NodeId child) {
    Node* t = mutable_find(target);
    if (!t)
        return;
    const NodeId pid = t->parent;
    Node* c = mutable_find(child);
    if (!c)
        return;

    if (pid == kInvalidNode || target == root_) {
        root_ = child;
        c->parent = kInvalidNode;
        return;
    }
    Node* p = mutable_find(pid);
    if (!p) {
        root_ = child;
        c->parent = kInvalidNode;
        return;
    }
    for (NodeId& slot : p->children) {
        if (slot == target) {
            slot = child;
            break;
        }
    }
    c->parent = pid;
}

void Layout::detach_panel(std::string_view panel) {
    const NodeId id = node_of_panel(panel);
    Node* n = mutable_find(id);
    if (!n)
        return;
    const auto it = std::find(n->tabs.begin(), n->tabs.end(), panel);
    if (it == n->tabs.end())
        return;
    const std::size_t idx = static_cast<std::size_t>(it - n->tabs.begin());
    n->tabs.erase(it);
    if (n->active >= n->tabs.size() && !n->tabs.empty())
        n->active = n->tabs.size() - 1;
    else if (n->active > idx)
        --n->active;
    // normalize() prunes the group if that emptied it.
}

NodeId Layout::insert_beside(NodeId target, Axis axis, bool leading, double share,
                             std::string_view panel) {
    Node leaf;
    leaf.kind = NodeKind::Tabs;
    leaf.tabs.emplace_back(panel);
    const NodeId lid = alloc(std::move(leaf));

    const Node* t = find(target);
    const NodeId pid = t ? t->parent : kInvalidNode;
    Node* p = mutable_find(pid);

    if (p && p->is_split() && p->axis == axis) {
        // Same axis as the parent: take half of the target's own slot rather than nesting a new
        // split inside it. Without this, repeated docking grows the tree without bound and the
        // dividers stop lining up with each other.
        const auto cit = std::find(p->children.begin(), p->children.end(), target);
        if (cit != p->children.end()) {
            const std::size_t idx = static_cast<std::size_t>(cit - p->children.begin());
            const double f = p->fractions[idx];
            const std::size_t at = leading ? idx : idx + 1;
            p->children.insert(p->children.begin() + off(at), lid);
            p->fractions.insert(p->fractions.begin() + off(at), 0.0);
            const std::size_t tat = leading ? idx + 1 : idx;
            p->fractions[at] = f * share;
            p->fractions[tat] = f * (1.0 - share);
            Node* l = mutable_find(lid);
            if (l)
                l->parent = pid;
            return lid;
        }
    }

    Node s;
    s.kind = NodeKind::Split;
    s.axis = axis;
    if (leading) {
        s.children = {lid, target};
        s.fractions = {share, 1.0 - share};
    } else {
        s.children = {target, lid};
        s.fractions = {1.0 - share, share};
    }
    const NodeId sid = alloc(std::move(s));

    replace_in_parent(target, sid);
    if (Node* tn = mutable_find(target))
        tn->parent = sid;
    if (Node* ln = mutable_find(lid))
        ln->parent = sid;
    return lid;
}

void Layout::normalize() {
    if (root_ == kInvalidNode || nodes_.find(root_) == nodes_.end()) {
        nodes_.clear();
        next_id_ = 1;
        Node n;
        n.kind = NodeKind::Tabs;
        root_ = alloc(std::move(n));
        return;
    }

    for (int pass = 0; pass < kNormalizePasses; ++pass) {
        bool changed = false;

        // Reachability and parent links, in one DFS.
        std::vector<NodeId> order;
        order.reserve(nodes_.size());
        collect(root_, order);
        if (Node* r = mutable_find(root_))
            r->parent = kInvalidNode;
        for (const NodeId id : order) {
            const Node* n = find(id);
            if (!n)
                continue;
            for (const NodeId c : n->children)
                if (Node* cn = mutable_find(c))
                    cn->parent = id;
        }

        if (order.size() != nodes_.size()) {
            const std::unordered_set<NodeId> live(order.begin(), order.end());
            for (auto it = nodes_.begin(); it != nodes_.end();)
                it = live.count(it->first) ? std::next(it) : nodes_.erase(it);
            changed = true;
        }

        // Children before parents, so a nested split is already canonical when its parent looks
        // at it.
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            Node* n = mutable_find(*it);
            if (!n)
                continue;

            if (n->is_tabs()) {
                if (!n->tabs.empty() && n->active >= n->tabs.size()) {
                    n->active = n->tabs.size() - 1;
                    changed = true;
                }
                continue;
            }

            std::vector<NodeId> kids;
            std::vector<double> frs;
            kids.reserve(n->children.size());
            frs.reserve(n->children.size());

            for (std::size_t i = 0; i < n->children.size(); ++i) {
                const NodeId cid = n->children[i];
                const double f = i < n->fractions.size() ? n->fractions[i] : 0.0;
                Node* c = mutable_find(cid);
                if (!c) {
                    changed = true;
                    continue;
                }
                if (c->is_tabs()) {
                    if (c->tabs.empty()) {
                        erase_subtree(cid);
                        changed = true;
                        continue;
                    }
                    kids.push_back(cid);
                    frs.push_back(f);
                    continue;
                }
                // Split child.
                if (c->children.empty()) {
                    erase_subtree(cid);
                    changed = true;
                    continue;
                }
                if (c->children.size() == 1) {
                    const NodeId g = c->children[0];
                    nodes_.erase(cid);
                    kids.push_back(g);
                    frs.push_back(f);
                    changed = true;
                    continue;
                }
                if (c->axis == n->axis) {
                    // Copy before erasing: the child's vectors die with it.
                    const std::vector<NodeId> gk = c->children;
                    std::vector<double> gf = c->fractions;
                    renormalize(gf);
                    nodes_.erase(cid);
                    for (std::size_t j = 0; j < gk.size(); ++j) {
                        kids.push_back(gk[j]);
                        frs.push_back(f * (j < gf.size() ? gf[j] : 0.0));
                    }
                    changed = true;
                    continue;
                }
                kids.push_back(cid);
                frs.push_back(f);
            }

            if (kids.size() != n->children.size())
                changed = true;
            n->children = std::move(kids);
            n->fractions = std::move(frs);

            if (n->children.empty()) {
                n->kind = NodeKind::Tabs;
                n->fractions.clear();
                n->tabs.clear();
                n->active = 0;
                changed = true;
                continue;
            }
            const std::vector<double> before = n->fractions;
            renormalize(n->fractions);
            if (before != n->fractions)
                changed = true;
        }

        // A split with one child is not a split. The parent pass collapses these, except at the
        // root, which has no parent to do it.
        if (Node* r = mutable_find(root_); r && r->is_split() && r->children.size() == 1) {
            const NodeId only = r->children[0];
            nodes_.erase(root_);
            root_ = only;
            if (Node* o = mutable_find(only))
                o->parent = kInvalidNode;
            changed = true;
        }

        if (!changed)
            break;
    }
}

DockResult Layout::dock(std::string_view panel, NodeId target, Zone zone, const Viewport& vp,
                        const Constraints& cs) {
    if (panel.empty())
        return DockResult{DockStatus::Rejected, "panel id is empty", kInvalidNode};
    if (zone == Zone::None)
        return DockResult{DockStatus::Rejected, "no drop zone under the cursor", kInvalidNode};

    const Node* t = find(target);
    if (!t)
        return DockResult{DockStatus::Rejected, "drop target does not exist", kInvalidNode};
    if (!t->is_tabs())
        return DockResult{DockStatus::Rejected, "drop target is not a panel group", kInvalidNode};

    // Already in this group. Nothing moves, but the drop does bring the tab to the front, which
    // is a change the user can see and will expect to be able to undo -- so it is Applied, not a
    // no-op. Reporting it as a no-op was a bug: the random-gesture test caught the engine saying
    // "nothing happened" about a layout it had just altered.
    if (zone == Zone::Center && node_of_panel(panel) == target) {
        const bool already_shown = !t->tabs.empty() && t->tabs[t->active] == panel;
        activate(panel);
        return DockResult{already_shown ? DockStatus::NoOp : DockStatus::Applied, {}, target};
    }

    // Everything below runs on a copy. A refused dock must leave the live layout untouched
    // (ADR-0016 rule 8), and a half-applied tree mutation is the worst bug this file can have.
    Layout next = *this;
    next.detach_panel(panel);
    next.normalize();

    if (!next.find(target)) {
        // The target was this panel's own group and removing the panel dissolved it, so the
        // gesture is asking to split a panel away from itself. Legal to attempt, nothing to do.
        return DockResult{DockStatus::NoOp, {}, node_of_panel(panel)};
    }

    NodeId landed = kInvalidNode;
    if (zone == Zone::Center) {
        Node* tn = next.mutable_find(target);
        tn->tabs.emplace_back(panel);
        tn->active = tn->tabs.size() - 1;
        landed = target;
    } else {
        landed = next.insert_beside(target, axis_of(zone), is_leading(zone), 0.5, panel);
    }
    next.normalize();

    if (const auto bad = next.validate())
        return DockResult{DockStatus::Rejected, std::format("layout invariant violated: {}", *bad),
                          kInvalidNode};
    if (!next.fits(vp, cs))
        return DockResult{DockStatus::Rejected,
                          "the result would push a panel below its minimum size", kInvalidNode};

    *this = std::move(next);
    return DockResult{DockStatus::Applied, {}, landed};
}

DockResult Layout::dock_edge(std::string_view panel, Edge edge, const Viewport& vp,
                             const Constraints& cs) {
    if (panel.empty())
        return DockResult{DockStatus::Rejected, "panel id is empty", kInvalidNode};

    Layout next = *this;
    next.detach_panel(panel);
    next.normalize();

    const double share = std::clamp(vp.edge_share, 0.05, 0.95);
    const NodeId landed =
        next.insert_beside(next.root(), axis_of(edge), is_leading(edge), share, panel);
    next.normalize();

    if (const auto bad = next.validate())
        return DockResult{DockStatus::Rejected, std::format("layout invariant violated: {}", *bad),
                          kInvalidNode};
    if (!next.fits(vp, cs))
        return DockResult{DockStatus::Rejected,
                          "the result would push a panel below its minimum size", kInvalidNode};

    *this = std::move(next);
    return DockResult{DockStatus::Applied, {}, landed};
}

DockResult Layout::dock(std::string_view panel, const DropTarget& target, const Viewport& vp,
                        const Constraints& cs) {
    return target.is_edge ? dock_edge(panel, target.edge, vp, cs)
                          : dock(panel, target.node, target.zone, vp, cs);
}

bool Layout::close_panel(std::string_view panel) {
    if (!has_panel(panel))
        return false;
    detach_panel(panel);
    normalize();
    return true;
}

bool Layout::activate(std::string_view panel) {
    const NodeId id = node_of_panel(panel);
    Node* n = mutable_find(id);
    if (!n)
        return false;
    const auto it = std::find(n->tabs.begin(), n->tabs.end(), panel);
    if (it == n->tabs.end())
        return false;
    n->active = static_cast<std::size_t>(it - n->tabs.begin());
    return true;
}

bool Layout::move_divider(NodeId split, std::size_t index, double delta_px, const Viewport& vp,
                          const Constraints& cs) {
    Node* s = mutable_find(split);
    if (!s || !s->is_split())
        return false;
    const std::size_t k = s->children.size();
    if (index + 1 >= k)
        return false;

    const auto r = rect_of(split, vp, cs);
    if (!r)
        return false;
    const double avail = extent_along(*r, s->axis) - vp.divider_px * static_cast<double>(k - 1);
    if (!(avail > 0.0))
        return false;

    std::vector<double> mins;
    mins.reserve(k);
    for (const NodeId c : s->children)
        mins.push_back(min_extent(c, s->axis, vp, cs));

    const std::vector<double> ext = distribute(*s, avail, mins);
    const double a = ext[index];
    const double total = a + ext[index + 1];
    if (!(total > 0.0))
        return false;

    const double lo = mins[index];
    const double hi = total - mins[index + 1];
    if (hi < lo)
        return false; // The pair is already at its combined minimum; nothing can move.

    const double na = std::clamp(a + delta_px, lo, hi);
    if (std::abs(na - a) < 1e-9)
        return false;

    const double combined = s->fractions[index] + s->fractions[index + 1];
    s->fractions[index] = combined * na / total;
    s->fractions[index + 1] = combined - s->fractions[index];
    return true;
}

// ---- integrity ------------------------------------------------------------------------------

std::optional<std::string> Layout::validate() const {
    if (root_ == kInvalidNode)
        return "root is unset";
    if (!find(root_))
        return std::format("root {} is not in the node table", root_);

    std::unordered_set<NodeId> seen;
    std::vector<NodeId> stack{root_};
    std::unordered_set<std::string> panel_set;

    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        if (!seen.insert(id).second)
            return std::format("node {} is reachable more than once (cycle or shared child)", id);

        const Node* n = find(id);
        if (!n)
            return std::format("node {} is referenced but missing", id);
        if (n->id != id)
            return std::format("node {} carries id {}", id, n->id);

        const NodeId expect_parent =
            (id == root_) ? kInvalidNode : n->parent;
        if (id == root_ && n->parent != kInvalidNode)
            return std::format("root {} has parent {}", id, n->parent);
        (void)expect_parent;

        if (n->is_split()) {
            if (n->children.size() < 2)
                return std::format("split {} has {} child(ren); a split needs at least two", id,
                                   n->children.size());
            if (n->fractions.size() != n->children.size())
                return std::format("split {} has {} children but {} fractions", id,
                                   n->children.size(), n->fractions.size());
            double s = 0.0;
            for (const double f : n->fractions) {
                if (!(f > 0.0) || !std::isfinite(f))
                    return std::format("split {} has a non-positive fraction", id);
                s += f;
            }
            if (std::abs(s - 1.0) > 1e-6)
                return std::format("split {} fractions sum to {}, not 1", id, s);
            if (!n->tabs.empty())
                return std::format("split {} also carries panels", id);
            for (const NodeId c : n->children) {
                const Node* cn = find(c);
                if (!cn)
                    return std::format("split {} references missing child {}", id, c);
                if (cn->parent != id)
                    return std::format("node {} has parent {} but is a child of {}", c,
                                       cn->parent, id);
                stack.push_back(c);
            }
        } else {
            if (n->tabs.empty() && id != root_)
                return std::format("tab group {} is empty and is not the root", id);
            if (!n->tabs.empty() && n->active >= n->tabs.size())
                return std::format("tab group {} has active index {} of {} tabs", id, n->active,
                                   n->tabs.size());
            if (!n->children.empty())
                return std::format("tab group {} also has children", id);
            for (const std::string& p : n->tabs) {
                if (p.empty())
                    return std::format("tab group {} holds an empty panel id", id);
                if (!panel_set.insert(p).second)
                    return std::format("panel '{}' appears more than once in the tree", p);
            }
        }
    }

    if (seen.size() != nodes_.size())
        return std::format("{} node(s) are not reachable from the root",
                           nodes_.size() - seen.size());
    return std::nullopt;
}

bool Layout::adopt(std::unordered_map<NodeId, Node> nodes, NodeId root) {
    if (nodes.find(root) == nodes.end())
        return false;

    // Pre-check that the node set is a tree before letting it near the live layout: every child
    // reference resolves, nothing is reached twice, nothing is orphaned.
    std::unordered_set<NodeId> seen;
    std::vector<NodeId> stack{root};
    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();
        if (!seen.insert(id).second)
            return false;
        const auto it = nodes.find(id);
        if (it == nodes.end())
            return false;
        for (const NodeId c : it->second.children) {
            if (nodes.find(c) == nodes.end())
                return false;
            stack.push_back(c);
        }
    }
    if (seen.size() != nodes.size())
        return false;

    Layout next;
    next.nodes_ = std::move(nodes);
    next.root_ = root;
    NodeId max_id = 0;
    for (const auto& [id, n] : next.nodes_) {
        max_id = std::max(max_id, id);
        if (n.id != id)
            return false;
    }
    next.next_id_ = max_id + 1;
    next.normalize();
    if (next.validate())
        return false;

    *this = std::move(next);
    return true;
}

bool operator==(const Layout& a, const Layout& b) {
    // Structural equality, ignoring node ids: two layouts that lay out identically are equal,
    // which is what a serialisation round-trip needs to assert.
    const std::function<bool(NodeId, NodeId)> same = [&](NodeId ia, NodeId ib) {
        const Node* na = a.find(ia);
        const Node* nb = b.find(ib);
        if (!na || !nb)
            return na == nb;
        if (na->kind != nb->kind)
            return false;
        if (na->is_tabs())
            return na->tabs == nb->tabs && na->active == nb->active;
        if (na->axis != nb->axis || na->children.size() != nb->children.size())
            return false;
        for (std::size_t i = 0; i < na->children.size(); ++i) {
            if (std::abs(na->fractions[i] - nb->fractions[i]) > 1e-9)
                return false;
            if (!same(na->children[i], nb->children[i]))
                return false;
        }
        return true;
    };
    return same(a.root(), b.root());
}

} // namespace llayout
