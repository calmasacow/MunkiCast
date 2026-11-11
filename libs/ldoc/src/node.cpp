// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/node.hpp"

#include <array>
#include <utility>

namespace ldoc {
namespace {

template <typename T, std::size_t N>
std::optional<T> lookup(const std::array<std::pair<T, std::string_view>, N>& table,
                        std::string_view s) {
    for (const auto& [value, name] : table)
        if (name == s)
            return value;
    return std::nullopt;
}

template <typename T, std::size_t N>
std::string_view name_of(const std::array<std::pair<T, std::string_view>, N>& table, T v) {
    for (const auto& [value, name] : table)
        if (value == v)
            return name;
    return "unknown";
}

constexpr std::array<std::pair<NodeKind, std::string_view>, 6> kNodeKinds = {{
    {NodeKind::Group, "group"},
    {NodeKind::Source, "source"},
    {NodeKind::Adjustment, "adjustment"},
    {NodeKind::Mask, "mask"},
    {NodeKind::Reference, "reference"},
    {NodeKind::Precomp, "precomp"},
}};

constexpr std::array<std::pair<MaskKind, std::string_view>, 3> kMaskKinds = {{
    {MaskKind::Alpha, "alpha"},
    {MaskKind::Luma, "luma"},
    {MaskKind::Shape, "shape"},
}};

constexpr std::array<std::pair<ReferenceMode, std::string_view>, 2> kReferenceModes = {{
    {ReferenceMode::Live, "live"},
    {ReferenceMode::Snapshot, "snapshot"},
}};

} // namespace

std::string_view to_string(NodeKind k) { return name_of(kNodeKinds, k); }
std::optional<NodeKind> node_kind_from_string(std::string_view s) {
    // Version 1 wrote "layer" for what version 2 calls a source (ADR-0028). Accepted on the way
    // in for as long as a version 1 project can be opened at all, which is the same promise the
    // migration makes; never written out.
    if (s == "layer")
        return NodeKind::Source;
    return lookup(kNodeKinds, s);
}

std::string_view to_string(MaskKind k) { return name_of(kMaskKinds, k); }
std::optional<MaskKind> mask_kind_from_string(std::string_view s) {
    return lookup(kMaskKinds, s);
}

std::string_view to_string(ReferenceMode m) { return name_of(kReferenceModes, m); }
std::optional<ReferenceMode> reference_mode_from_string(std::string_view s) {
    return lookup(kReferenceModes, s);
}

NodeKind Node::kind() const {
    return std::visit(
        [](const auto& d) {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, GroupData>)
                return NodeKind::Group;
            else if constexpr (std::is_same_v<T, SourceData>)
                return NodeKind::Source;
            else if constexpr (std::is_same_v<T, AdjustmentData>)
                return NodeKind::Adjustment;
            else if constexpr (std::is_same_v<T, MaskData>)
                return NodeKind::Mask;
            else if constexpr (std::is_same_v<T, ReferenceData>)
                return NodeKind::Reference;
            else
                return NodeKind::Precomp;
        },
        data);
}

std::vector<Node>* Node::children() {
    auto* g = std::get_if<GroupData>(&data);
    return g ? &g->children : nullptr;
}

const std::vector<Node>* Node::children() const {
    const auto* g = std::get_if<GroupData>(&data);
    return g ? &g->children : nullptr;
}

Node make_node(NodeKind kind, std::string name) {
    Node n;
    n.id = Id::generate();
    n.name = std::move(name);
    switch (kind) {
    case NodeKind::Group:      n.data = GroupData{}; break;
    case NodeKind::Source:     n.data = SourceData{}; break;
    case NodeKind::Adjustment: n.data = AdjustmentData{}; break;
    case NodeKind::Mask:       n.data = MaskData{}; break;
    case NodeKind::Reference:  n.data = ReferenceData{}; break;
    case NodeKind::Precomp:    n.data = PrecompData{}; break;
    }
    return n;
}

void visit(Node& root, const std::function<void(Node&)>& fn) {
    fn(root);
    if (auto* kids = root.children())
        for (Node& c : *kids)
            visit(c, fn);
}

void visit(const Node& root, const std::function<void(const Node&)>& fn) {
    fn(root);
    if (const auto* kids = root.children())
        for (const Node& c : *kids)
            visit(c, fn);
}

void visit(std::vector<Node>& roots, const std::function<void(Node&)>& fn) {
    for (Node& n : roots)
        visit(n, fn);
}

void visit(const std::vector<Node>& roots, const std::function<void(const Node&)>& fn) {
    for (const Node& n : roots)
        visit(n, fn);
}

Node* find_node(std::vector<Node>& roots, const Id& id) {
    Node* found = nullptr;
    visit(roots, [&](Node& n) {
        if (!found && n.id == id)
            found = &n;
    });
    return found;
}

const Node* find_node(const std::vector<Node>& roots, const Id& id) {
    const Node* found = nullptr;
    visit(roots, [&](const Node& n) {
        if (!found && n.id == id)
            found = &n;
    });
    return found;
}

namespace {

/// Pass one: mint a new id for every node and effect, recording the mapping.
void remint(Node& n, std::map<Id, Id>& remap) {
    const Id fresh = Id::generate();
    remap.emplace(n.id, fresh);
    n.id = fresh;
    for (Effect& e : n.effects) {
        const Id ef = Id::generate();
        remap.emplace(e.id, ef);
        e.id = ef;
    }
    if (auto* kids = n.children())
        for (Node& c : *kids)
            remint(c, remap);
}

/// Pass two: rewrite only those references whose target was reminted. A target that is not in
/// the table pointed outside the copied subtree and must be left exactly as it was.
void rewrite(Node& n, const std::map<Id, Id>& remap) {
    if (auto* ref = std::get_if<ReferenceData>(&n.data)) {
        if (const auto it = remap.find(ref->target); it != remap.end())
            ref->target = it->second;
    }
    for (Effect& e : n.effects) {
        if (e.mask) {
            if (const auto it = remap.find(*e.mask); it != remap.end())
                e.mask = it->second;
        }
    }
    if (auto* kids = n.children())
        for (Node& c : *kids)
            rewrite(c, remap);
}

} // namespace

std::vector<Node> duplicate(const std::vector<Node>& nodes, std::map<Id, Id>* remap_out) {
    std::vector<Node> copy = nodes;
    std::map<Id, Id> remap;
    // Two passes, not one. A reference can point at a node that has not been reached yet, so the
    // table has to be complete before anything is rewritten.
    for (Node& n : copy)
        remint(n, remap);
    for (Node& n : copy)
        rewrite(n, remap);
    if (remap_out)
        *remap_out = std::move(remap);
    return copy;
}

Node duplicate(const Node& node, std::map<Id, Id>* remap_out) {
    std::vector<Node> one{node};
    auto copied = duplicate(one, remap_out);
    return std::move(copied.front());
}

void collect_defined_ids(const std::vector<Node>& roots, std::vector<Id>& out) {
    visit(roots, [&out](const Node& n) {
        out.push_back(n.id);
        for (const Effect& e : n.effects)
            out.push_back(e.id);
    });
}

void collect_references(const std::vector<Node>& roots, std::vector<Reference>& out) {
    visit(roots, [&out](const Node& n) {
        if (const auto* ref = std::get_if<ReferenceData>(&n.data))
            out.push_back(Reference{ref->target, RefKind::Node, n.id});
        if (const auto* placed = std::get_if<SourceData>(&n.data)) {
            if (placed->source)
                out.push_back(Reference{*placed->source, RefKind::Source, n.id});
            if (placed->audio.bus)
                out.push_back(Reference{*placed->audio.bus, RefKind::Bus, n.id});
            for (const Send& s : placed->audio.sends)
                out.push_back(Reference{s.to, RefKind::Bus, n.id});
        }
        if (const auto* pre = std::get_if<PrecompData>(&n.data))
            out.push_back(Reference{pre->scene, RefKind::Scene, n.id});
        for (const Effect& e : n.effects) {
            if (e.mask)
                out.push_back(Reference{*e.mask, RefKind::Node, n.id});
        }
    });
}

} // namespace ldoc
