// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "llayout/dropzone.hpp"
#include "llayout/geometry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// The workspace layout tree (ADR-0015, ADR-0016).
///
/// The tree holds **relative proportions only**. No pixel geometry is ever stored, so a saved
/// layout restores correctly at any resolution and on any monitor arrangement -- which is
/// ADR-0015 rule 4, and the thing whose absence destroys OBS layouts on a single-monitor machine.
///
/// Physical size enters only as an argument: a `Viewport` (how big the window is now) and a
/// `Constraints` (how small each panel may get). Both are code- or runtime-supplied, never
/// persisted.
namespace llayout {

using NodeId = std::uint32_t;
inline constexpr NodeId kInvalidNode = 0;

enum class NodeKind : std::uint8_t {
    /// Two or more children laid out along `axis`, with `fractions` proportions.
    Split,
    /// A tabbed group of panels. The only kind that holds panels, and the only drop target.
    Tabs,
};

struct Node {
    NodeKind kind = NodeKind::Tabs;
    NodeId id = kInvalidNode;
    NodeId parent = kInvalidNode;

    // Split only.
    Axis axis = Axis::Horizontal;
    std::vector<NodeId> children;
    /// Parallel to `children`, strictly positive, sums to 1. These are the user's *intent*;
    /// `Layout::leaves` may hand out different extents to honour minimum sizes, but it never
    /// rewrites these behind the user's back.
    std::vector<double> fractions;

    // Tabs only.
    std::vector<std::string> tabs;
    std::size_t active = 0;

    [[nodiscard]] bool is_split() const { return kind == NodeKind::Split; }
    [[nodiscard]] bool is_tabs() const { return kind == NodeKind::Tabs; }
};

/// The smallest a panel may be drawn at, in physical pixels.
struct PanelMin {
    double w = 180.0;
    double h = 120.0;
};

/// Per-panel minimum sizes. Code-defined, never serialised: a layout file that could dictate
/// minimum sizes could dictate an unusable layout.
class Constraints {
public:
    Constraints() = default;

    void set_fallback(PanelMin m) { fallback_ = m; }
    void set(std::string_view panel, PanelMin m) { overrides_.insert_or_assign(std::string(panel), m); }

    [[nodiscard]] PanelMin get(std::string_view panel) const {
        const auto it = overrides_.find(panel);
        return it == overrides_.end() ? fallback_ : it->second;
    }

    /// The defaults the studio shell uses. The program canvas needs more room than a list.
    [[nodiscard]] static Constraints studio_defaults();

private:
    PanelMin fallback_{};
    std::map<std::string, PanelMin, std::less<>> overrides_;
};

struct Viewport {
    Rect bounds{};
    /// Thickness of the draggable divider between two children of a split.
    double divider_px = 6.0;
    /// Height of a tab bar, which every Tabs node pays for out of its vertical space.
    double tab_bar_px = 28.0;
    /// How close to the workspace border a drag must come to target the edge (ADR-0016 rule 6).
    double edge_px = 18.0;
    /// Share of the workspace an edge dock takes. An outer-zone dock splits its target in half;
    /// an edge dock makes a side column, which wants to be narrower than half the window.
    double edge_share = 0.22;
};

/// Where a leaf landed, for the UI to draw.
struct LeafRect {
    NodeId node = kInvalidNode;
    Rect rect{};
};

/// A draggable divider: the gap between children `index` and `index + 1` of `split`.
struct DividerHandle {
    NodeId split = kInvalidNode;
    std::size_t index = 0;
    Axis axis = Axis::Horizontal;
    Rect rect{};
};

/// What a drag at a given point would do if released.
struct DropTarget {
    /// True when this targets a workspace edge rather than a panel.
    bool is_edge = false;
    Edge edge = Edge::Left;
    NodeId node = kInvalidNode;
    Zone zone = Zone::None;
    /// The region the dragged panel would occupy, for the highlight.
    Rect preview{};
};

enum class DockStatus : std::uint8_t {
    /// The tree changed.
    Applied,
    /// The gesture was legal but asked for the state the layout is already in.
    NoOp,
    /// Refused. The tree is byte-for-byte unchanged and `reason` says why.
    Rejected,
};

struct DockResult {
    DockStatus status = DockStatus::Rejected;
    std::string reason;
    /// The Tabs node the panel ended up in, when the dock applied.
    NodeId node = kInvalidNode;

    [[nodiscard]] bool ok() const { return status != DockStatus::Rejected; }
    [[nodiscard]] bool applied() const { return status == DockStatus::Applied; }
};

class Layout {
public:
    /// An empty workspace: one Tabs node holding nothing.
    Layout();

    /// The operator's Build arrangement: scene and source lists, program canvas, countdowns,
    /// properties, output controls, mixer, and broadcast statistics.
    [[nodiscard]] static Layout default_build();

    /// The arrangement for running one: favourites instead of the full scene and source lists,
    /// program and preview, the mixer, the outputs and the statistics. Everything that exists to
    /// *change* the show is gone, because a mis-click during a show is expensive.
    [[nodiscard]] static Layout default_live();

    /// The one a fresh install starts on.
    [[nodiscard]] static Layout default_studio() { return default_build(); }

    // ---- structure -------------------------------------------------------------------------

    [[nodiscard]] NodeId root() const { return root_; }
    [[nodiscard]] const Node* find(NodeId id) const;
    [[nodiscard]] std::size_t node_count() const { return nodes_.size(); }

    /// Depth-first, children in order. Deterministic, which is what serialisation and tests need.
    [[nodiscard]] std::vector<NodeId> node_ids() const;
    [[nodiscard]] std::vector<std::string> panels() const;

    [[nodiscard]] bool has_panel(std::string_view panel) const;
    /// The Tabs node holding `panel`, or kInvalidNode.
    [[nodiscard]] NodeId node_of_panel(std::string_view panel) const;
    /// Tab index of `panel` within its group, or npos.
    [[nodiscard]] std::size_t tab_index_of(std::string_view panel) const;

    // ---- geometry --------------------------------------------------------------------------

    /// Visit every node with its rect, parents before children.
    void visit(const Viewport& vp, const Constraints& cs,
               const std::function<void(const Node&, const Rect&)>& fn) const;

    [[nodiscard]] std::vector<LeafRect> leaves(const Viewport& vp, const Constraints& cs) const;
    [[nodiscard]] std::optional<Rect> rect_of(NodeId id, const Viewport& vp,
                                              const Constraints& cs) const;
    [[nodiscard]] std::vector<DividerHandle> dividers(const Viewport& vp,
                                                      const Constraints& cs) const;

    /// The smallest `axis` extent this subtree can be drawn in. Recursive: a split along the
    /// same axis sums its children plus dividers, a split across it takes the maximum.
    [[nodiscard]] double min_extent(NodeId id, Axis axis, const Viewport& vp,
                                    const Constraints& cs) const;

    /// Whether the whole tree can be drawn inside `vp.bounds` without any panel going below its
    /// minimum. This is the test ADR-0016 rule 8 refuses a drop on.
    [[nodiscard]] bool fits(const Viewport& vp, const Constraints& cs) const;

    // ---- hit testing -----------------------------------------------------------------------

    /// What a drop at (x, y) would do. Workspace edges take precedence over panel zones, because
    /// an edge dock is the more specific gesture and there is no other way to ask for it.
    [[nodiscard]] std::optional<DropTarget> hit_test(double x, double y, const Viewport& vp,
                                                     const Constraints& cs,
                                                     const ZoneMetrics& zm = {}) const;

    /// The panel group immediately in `direction` from `from`, or nothing at the workspace edge.
    ///
    /// Geometric rather than structural: the neighbour to the left is whatever is drawn to the
    /// left, not whatever happens to be a sibling in the tree. A tree-walking answer would skip
    /// past a panel that is visibly adjacent just because it lives in a different branch, which
    /// is not what anyone means by "the panel to the left".
    ///
    /// This is what lets the whole docking gesture be driven from the keyboard. ADR-0016's model
    /// is entirely pointer-based, and an operator who cannot aim a pointer precisely could not
    /// rearrange the workspace at all.
    [[nodiscard]] std::optional<NodeId> neighbour(NodeId from, Edge direction, const Viewport& vp,
                                                  const Constraints& cs) const;

    /// The divider under (x, y), widened by `slop_px` so a 6 px divider is grabbable.
    [[nodiscard]] std::optional<DividerHandle> hit_test_divider(double x, double y,
                                                                const Viewport& vp,
                                                                const Constraints& cs,
                                                                double slop_px = 3.0) const;

    // ---- mutation --------------------------------------------------------------------------

    /// Dock `panel` into `target`'s space at `zone`. `panel` may already be in the tree (a move)
    /// or not (opening a closed panel). Either the whole gesture applies or nothing does.
    DockResult dock(std::string_view panel, NodeId target, Zone zone, const Viewport& vp,
                    const Constraints& cs);

    /// Dock `panel` against a workspace edge, becoming a full-height column or full-width row.
    DockResult dock_edge(std::string_view panel, Edge edge, const Viewport& vp,
                         const Constraints& cs);

    /// Convenience: apply whatever `hit_test` returned.
    DockResult dock(std::string_view panel, const DropTarget& target, const Viewport& vp,
                    const Constraints& cs);

    /// Remove `panel`, collapsing whatever that empties. False if it was not present.
    bool close_panel(std::string_view panel);

    /// Make `panel` the visible tab of its group. False if it is not present.
    bool activate(std::string_view panel);

    /// Drag the divider between children `index` and `index + 1` of `split` by `delta_px`,
    /// clamped so neither side goes below its minimum. Returns false if nothing moved.
    bool move_divider(NodeId split, std::size_t index, double delta_px, const Viewport& vp,
                      const Constraints& cs);

    // ---- integrity -------------------------------------------------------------------------

    /// nullopt when every invariant holds; otherwise a description of the first violation.
    /// Called by the tests after every mutation, and on load.
    [[nodiscard]] std::optional<std::string> validate() const;

    /// Rebuild parent links and canonicalise the tree. Public because the deserialiser builds a
    /// node set directly and needs to finish the job.
    void normalize();

    friend bool operator==(const Layout& a, const Layout& b);

    // ---- deserialiser support --------------------------------------------------------------

    /// Replace the whole tree. Returns false (leaving the layout untouched) if the node set does
    /// not form a single tree rooted at `root`.
    bool adopt(std::unordered_map<NodeId, Node> nodes, NodeId root);

private:
    NodeId alloc(Node n);
    Node* mutable_find(NodeId id);

    /// Remove `panel` wherever it is, pruning emptied nodes. No-op if absent.
    void detach_panel(std::string_view panel);
    /// Put `child` in the slot `target` occupies (root, or a particular index of its parent).
    void replace_in_parent(NodeId target, NodeId child);
    /// Delete a subtree.
    void erase_subtree(NodeId id);
    /// Insert a new Tabs node holding `panel` adjacent to `target` along `axis`.
    NodeId insert_beside(NodeId target, Axis axis, bool leading, double share,
                         std::string_view panel);

    void collect(NodeId id, std::vector<NodeId>& out) const;
    /// Extents for the children of a split, honouring minima by water-filling.
    std::vector<double> distribute(const Node& split, double avail,
                                   const std::vector<double>& mins) const;
    void visit_from(NodeId id, const Rect& r, const Viewport& vp, const Constraints& cs,
                    const std::function<void(const Node&, const Rect&)>& fn) const;

    std::unordered_map<NodeId, Node> nodes_;
    NodeId root_ = kInvalidNode;
    NodeId next_id_ = 1;
};

} // namespace llayout
