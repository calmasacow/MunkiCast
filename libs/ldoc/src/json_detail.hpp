// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// Shared internals of the JSON layer.
///
/// Private to `libs/ldoc/src`. It exists because commands carry pieces of the document -- a
/// whole Node, a Transform, an effect's parameters -- and a command has to serialise for the
/// journal and for the wire. Re-deriving those encoders in a second file would give two
/// definitions of the format that could drift; one of them would be the one a project was
/// written with and the other the one it was replayed with.

#include "ldoc/document.hpp"

#include <json/json.h>

#include <optional>
#include <string>
#include <string_view>

namespace ldoc::detail {

using Jv = Json::Value;

// ---- writing --------------------------------------------------------------------------------

Jv enc(const Id& id);
Jv enc(const Vec2& v);
Jv enc(const Rgba& c);
Jv enc(const Fps& f);
Jv enc(const ParamValue& v);
Jv enc(const ParamMap& m);
Jv enc(const Transform& t);
Jv enc(const CropRect& c);
Jv enc(const Layout& l);
Jv enc(const Blend& b);
Jv enc(const Effect& e);
Jv enc(const Send& s);
Jv enc(const AudioRouting& a);
Jv enc(const Deck& d);
Jv enc(const Node& n);
Jv enc_scene(const Scene& scene);
Jv enc(const Canvas& c);
Jv enc(const Source& s);
Jv enc(const Bus& b);
Jv enc(const Output& o);
Jv enc(const Encode& e);
Jv enc(const AudioEncode& e);
Jv enc(const SceneLink& l);
Jv enc(const Settings& s);

std::string write(const Jv& root);

// ---- reading --------------------------------------------------------------------------------

/// Accumulates the first error with a path to it. Every getter is a no-op once an error has been
/// recorded, which keeps the decoders free of error plumbing.
class Rd {
public:
    [[nodiscard]] bool ok() const { return err_.empty(); }
    [[nodiscard]] const std::string& error() const { return err_; }

    void fail(std::string_view path, std::string_view msg);

    bool boolean(const Jv& o, const char* key, std::string_view path, bool def);
    double number(const Jv& o, const char* key, std::string_view path, double def);
    std::int64_t integer(const Jv& o, const char* key, std::string_view path, std::int64_t def);
    std::int32_t integer32(const Jv& o, const char* key, std::string_view path, std::int32_t def);
    std::string text(const Jv& o, const char* key, std::string_view path, std::string def = {});
    [[nodiscard]] std::optional<Id> parse_id(std::string_view value, std::string_view path,
                                             const char* key);
    Id id(const Jv& o, const char* key, std::string_view path);
    std::optional<Id> id_opt(const Jv& o, const char* key, std::string_view path);
    const Jv* object(const Jv& o, const char* key, std::string_view path);
    const Jv* array(const Jv& o, const char* key, std::string_view path);

    /// Looks a named value up in an enum table, failing rather than silently defaulting: a blend
    /// mode nobody recognises must not quietly become Normal.
    template <typename T, typename F>
    T enumerated(const Jv& o, const char* key, std::string_view path, F from_string, T def) {
        const Jv& v = o[key];
        if (v.isNull())
            return def;
        if (!v.isString()) {
            fail(path, std::string{"'"} + key + "' is not a string");
            return def;
        }
        const auto parsed = from_string(v.asString());
        if (!parsed) {
            fail(path, std::string{"'"} + key + "' is '" + v.asString() +
                           "', which this build does not know");
            return def;
        }
        return *parsed;
    }

    /// Counted, not read: version 1's per-canvas deltas are gone (ADR-0027) and the reader's job
    /// is to say how many it threw away, not to keep them.
    std::size_t dropped_canvas_overrides = 0;

private:
    std::string err_;
};

Vec2 dec_vec2(Rd& rd, const Jv& o, std::string_view path);
Vec2 dec_vec2_at(Rd& rd, const Jv& parent, const char* key, std::string_view path, Vec2 def);
Rgba dec_rgba(Rd& rd, const Jv& o, std::string_view path);
ParamValue dec_param(Rd& rd, const Jv& v, std::string_view path);
ParamMap dec_params(Rd& rd, const Jv& parent, const char* key, std::string_view path);
Transform dec_transform(Rd& rd, const Jv& parent, const char* key, std::string_view path);
Layout dec_layout(Rd& rd, const Jv& parent, const char* key, std::string_view path);
Blend dec_blend_obj(Rd& rd, const Jv& o, std::string_view path);
Blend dec_blend(Rd& rd, const Jv& parent, const char* key, std::string_view path);
Effect dec_effect(Rd& rd, const Jv& o, std::string_view path);
std::vector<Effect> dec_effects(Rd& rd, const Jv& parent, const char* key, std::string_view path);
Send dec_send(Rd& rd, const Jv& o, std::string_view path);
AudioRouting dec_audio_routing(Rd& rd, const Jv& o, std::string_view path);
std::vector<Send> dec_sends(Rd& rd, const Jv& parent, const char* key, std::string_view path);
Node dec_node(Rd& rd, const Jv& o, std::string_view path);
Scene dec_scene(Rd& rd, const Jv& o, std::string_view path);
SceneLink dec_scene_link(Rd& rd, const Jv& o, std::string_view path);
Canvas dec_canvas(Rd& rd, const Jv& o, std::string_view path);
Source dec_source(Rd& rd, const Jv& o, std::string_view path);
Bus dec_bus(Rd& rd, const Jv& o, std::string_view path);
Deck dec_deck(Rd& rd, const Jv& o, std::string_view path);
Output dec_output(Rd& rd, const Jv& o, std::string_view path);
Encode dec_encode(Rd& rd, const Jv& o, std::string_view path);
AudioEncode dec_audio_encode(Rd& rd, const Jv& o, std::string_view path);
EncoderConfig dec_encoder_config(Rd& rd, const Jv& parent, const char* key,
                                 std::string_view path);
Settings dec_settings(Rd& rd, const Jv& o, std::string_view path);

std::optional<Jv> parse_text(std::string_view text, std::string& error);

} // namespace ldoc::detail
