// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timelines and graphics: the edit operations, and the undo isolation that is the whole reason
// they are separate documents rather than more vectors inside Project.

#include "ldoc/command.hpp"
#include "ldoc/history.hpp"
#include "ldoc/subcommand.hpp"
#include "ldoc/subdocument.hpp"
#include "ldoc/json.hpp"
#include "ldoc/workbench.hpp"

#include "ltest.hpp"

#include <string>

using namespace ldoc;

namespace {

/// A timeline with one video track holding two clips, with a gap between them:
///
///   0        100      200      300
///   [--- A ---]       [--- B ---]
struct Fixture {
    Timeline timeline;
    Id track;
    Id clip_a;
    Id clip_b;

    Fixture() {
        timeline.id = Id::generate();
        timeline.name = "Main Edit";

        Track v1;
        v1.id = Id::generate();
        v1.name = "V1";
        v1.kind = TrackKind::Video;

        Clip a;
        a.id = Id::generate();
        a.source = "/media/a.mov";
        a.name = "A";
        a.start = 0;
        a.duration = 100;
        a.source_in = 0;

        Clip b;
        b.id = Id::generate();
        b.source = "/media/b.mov";
        b.name = "B";
        b.start = 200;
        b.duration = 100;
        b.source_in = 50;

        track = v1.id;
        clip_a = a.id;
        clip_b = b.id;
        v1.clips = {a, b};
        timeline.tracks.push_back(std::move(v1));
    }
};

Clip make_clip(const char* name, Frame start, Frame duration) {
    Clip c;
    c.id = Id::generate();
    c.source = "/media/c.mov";
    c.name = name;
    c.start = start;
    c.duration = duration;
    return c;
}

} // namespace

LTEST_CASE(a_fresh_timeline_validates_and_measures_its_own_length) {
    Fixture f;
    CHECK(!f.timeline.validate().has_value());
    CHECK_EQ(f.timeline.duration(), Frame{300});
    CHECK_EQ(f.timeline.track(f.track)->clips.size(), std::size_t{2});
}

LTEST_CASE(a_clip_cannot_be_dropped_on_top_of_another) {
    Fixture f;
    // Starts at 50, which is inside A.
    const auto refused = apply(f.timeline, AddClip{f.track, make_clip("C", 50, 20)});
    REQUIRE(!refused.has_value());

    // Refused means nothing changed, not changed a little.
    CHECK_EQ(f.timeline.track(f.track)->clips.size(), std::size_t{2});
    CHECK(!f.timeline.validate().has_value());
}

LTEST_CASE(a_clip_fits_exactly_in_the_gap_between_two_others) {
    Fixture f;
    const auto placed = apply(f.timeline, AddClip{f.track, make_clip("C", 100, 100)});
    REQUIRE(placed.has_value());
    CHECK_EQ(f.timeline.track(f.track)->clips.size(), std::size_t{3});
    // Sorted on insert, so the new clip is the middle one rather than the last.
    CHECK_EQ(f.timeline.track(f.track)->clips[1].name, std::string{"C"});
    CHECK(!f.timeline.validate().has_value());
}

LTEST_CASE(a_refused_move_leaves_the_clip_exactly_where_it_was) {
    Fixture f;
    const Timeline before = f.timeline;
    // B onto A's ground.
    const auto refused = apply(f.timeline, MoveClip{f.track, f.clip_b, 50, Id{}});
    REQUIRE(!refused.has_value());
    // Byte-identical: the clip is taken out of the track to test the destination, so this is
    // the case where a half-applied edit would show up.
    CHECK(f.timeline == before);
}

LTEST_CASE(splitting_a_clip_gives_two_clips_that_cover_the_same_media) {
    Fixture f;
    const Id right = Id::generate();
    const auto split = apply(f.timeline, SplitClip{f.track, f.clip_a, 40, right});
    REQUIRE(split.has_value());

    const Track* track = f.timeline.track(f.track);
    REQUIRE(track->clips.size() == 3);
    const Clip* left = track->clip(f.clip_a);
    const Clip* tail = track->clip(right);
    REQUIRE(left != nullptr);
    REQUIRE(tail != nullptr);

    CHECK_EQ(left->start, Frame{0});
    CHECK_EQ(left->duration, Frame{40});
    CHECK_EQ(tail->start, Frame{40});
    CHECK_EQ(tail->duration, Frame{60});
    // The cut is seamless: the second piece opens on the media frame the first one stops at.
    CHECK_EQ(tail->source_in, Frame{40});
    CHECK(!f.timeline.validate().has_value());
}

LTEST_CASE(undoing_a_split_restores_the_original_clip_exactly) {
    Fixture f;
    const Timeline before = f.timeline;

    TimelineHistory history{f.timeline};
    REQUIRE(history.run(SplitClip{f.track, f.clip_a, 40, Id::generate()}).has_value());
    CHECK_EQ(history.document().track(f.track)->clips.size(), std::size_t{3});

    REQUIRE(history.undo().has_value());
    // The inverse is a batch whose order matters: remove the new piece, then restore the
    // length. The other order overlaps and is refused.
    CHECK(history.document() == before);
}

LTEST_CASE(every_timeline_edit_round_trips_through_undo_and_redo) {
    Fixture f;
    const Timeline original = f.timeline;
    TimelineHistory history{f.timeline};

    const std::vector<TimelineCommand> edits{
        SetTimelineName{"Renamed"},
        AddClip{f.track, make_clip("C", 100, 100)},
        RemoveClip{f.track, f.clip_b},
        MoveClip{f.track, f.clip_a, 400, Id{}},
        TrimClip{f.track, f.clip_a, 10, 50, 5},
        SetClip{f.track, f.clip_a, "A renamed", -6.0},
        SetTimelineRange{Frame{10}, Frame{90}},
        SetTimelineFormat{Fps{24, 1}, 3840, 2160},
    };

    for (const TimelineCommand& edit : edits) {
        const Timeline before = history.document();
        REQUIRE(history.run(edit).has_value());
        CHECK(history.document() != before);
        REQUIRE(history.undo().has_value());
        CHECK(history.document() == before);
        REQUIRE(history.redo().has_value());
        CHECK(!history.document().validate().has_value());
    }

    // And all the way back out again, in one run.
    while (history.can_undo())
        REQUIRE(history.undo().has_value());
    CHECK(history.document() == original);
}

LTEST_CASE(a_drag_is_one_undo_step_not_one_per_frame) {
    Fixture f;
    TimelineHistory history{f.timeline};
    const std::string gesture = "clip.move:" + f.clip_b.to_string();

    for (Frame at = 201; at <= 260; ++at)
        REQUIRE(history.run(MoveClip{f.track, f.clip_b, at, Id{}}, gesture).has_value());

    CHECK_EQ(history.undo_depth(), std::size_t{1});
    REQUIRE(history.undo().has_value());
    // Back to where the drag began, not to one frame into it.
    CHECK_EQ(history.document().track(f.track)->clip(f.clip_b)->start, Frame{200});

    // Letting go and dragging again is a second step.
    history.end_gesture();
    REQUIRE(history.run(MoveClip{f.track, f.clip_b, 210, Id{}}, gesture).has_value());
    CHECK_EQ(history.undo_depth(), std::size_t{1});
}

LTEST_CASE(a_locked_track_refuses_every_edit_that_would_change_it) {
    Fixture f;
    REQUIRE(apply(f.timeline, SetTrack{f.track, "V1", true, true}).has_value());
    const Timeline before = f.timeline;

    CHECK(!apply(f.timeline, AddClip{f.track, make_clip("C", 100, 100)}).has_value());
    CHECK(!apply(f.timeline, RemoveClip{f.track, f.clip_a}).has_value());
    CHECK(!apply(f.timeline, MoveClip{f.track, f.clip_a, 500, Id{}}).has_value());
    CHECK(!apply(f.timeline, SplitClip{f.track, f.clip_a, 40, Id::generate()}).has_value());
    CHECK(f.timeline == before);
}

LTEST_CASE(a_batch_that_fails_part_way_changes_nothing) {
    Fixture f;
    const Timeline before = f.timeline;
    // The first step is fine; the second overlaps A.
    const auto refused = apply(
        f.timeline, make_timeline_batch({AddClip{f.track, make_clip("C", 100, 50)},
                                         AddClip{f.track, make_clip("D", 20, 50)}},
                                        "Add two"));
    REQUIRE(!refused.has_value());
    CHECK(f.timeline == before);
}

LTEST_CASE(a_transition_needs_a_clip_butted_up_against_the_one_it_opens) {
    Fixture f;
    // B starts at 200 and A ends at 100, so there is a gap: nothing to dissolve from.
    CHECK(!apply(f.timeline,
                 SetTransition{f.track, f.clip_b, Transition{TransitionKind::CrossDissolve, 12}})
               .has_value());

    // Butt them together and it is allowed.
    REQUIRE(apply(f.timeline, MoveClip{f.track, f.clip_b, 100, Id{}}).has_value());
    const auto applied = apply(
        f.timeline, SetTransition{f.track, f.clip_b, Transition{TransitionKind::CrossDissolve, 12}});
    REQUIRE_MSG(applied.has_value(), "{}", applied.error());
    CHECK_EQ(f.timeline.track(f.track)->clip(f.clip_b)->transition.duration, Frame{12});
    CHECK(!f.timeline.validate().has_value());

    // And never longer than either clip it joins.
    CHECK(!apply(f.timeline,
                 SetTransition{f.track, f.clip_b, Transition{TransitionKind::DipToBlack, 500}})
               .has_value());
}

LTEST_CASE(solo_silences_every_audio_track_that_is_not_soloed) {
    Timeline t;
    t.id = Id::generate();
    t.name = "Mix";

    Track a;
    a.id = Id::generate();
    a.name = "A1";
    a.kind = TrackKind::Audio;
    Track b;
    b.id = Id::generate();
    b.name = "A2";
    b.kind = TrackKind::Audio;
    t.tracks = {a, b};

    // Nothing soloed: everything is heard.
    CHECK(!t.any_solo());
    CHECK(t.audible(t.tracks[0]));
    CHECK(t.audible(t.tracks[1]));

    t.tracks[0].solo = true;
    CHECK(t.any_solo());
    CHECK(t.audible(t.tracks[0]));
    CHECK(!t.audible(t.tracks[1]));

    // Two soloed together, which is how a pair is checked against each other.
    t.tracks[1].solo = true;
    CHECK(t.audible(t.tracks[0]));
    CHECK(t.audible(t.tracks[1]));

    // Mute wins over solo on the same track: it is the more explicit instruction.
    t.tracks[0].muted = true;
    CHECK(!t.audible(t.tracks[0]));
}

LTEST_CASE(track_audio_and_the_mix_level_round_trip_and_reverse) {
    Fixture f;
    const Timeline original = f.timeline;
    TimelineHistory history{f.timeline};

    REQUIRE(history.run(SetTrack{f.track, "V1", true, false, true, true, -6.0}).has_value());
    const Track* track = history.document().track(f.track);
    CHECK(track->muted);
    CHECK(track->solo);
    CHECK_EQ(track->gain_db, -6.0);

    REQUIRE(history.run(SetMasterGain{-3.0}).has_value());
    CHECK_EQ(history.document().master_gain_db, -3.0);

    while (history.can_undo())
        REQUIRE(history.undo().has_value());
    CHECK(history.document() == original);

    // And through a file, with the transition that the fixture's clips can hold.
    REQUIRE(apply(f.timeline, MoveClip{f.track, f.clip_b, 100, Id{}}).has_value());
    REQUIRE(apply(f.timeline,
                  SetTransition{f.track, f.clip_b, Transition{TransitionKind::Push, 8}})
                .has_value());
    f.timeline.master_gain_db = -4.5;
    f.timeline.tracks[0].gain_db = 2.0;
    f.timeline.tracks[0].muted = true;

    const auto read = timeline_from_json(timeline_to_json(f.timeline, 1));
    REQUIRE_MSG(read.has_value(), "{}", read.error());
    CHECK(read->document == f.timeline);
}

// ---- graphics ---------------------------------------------------------------------------------

LTEST_CASE(a_channel_interpolates_between_its_keyframes_and_holds_at_the_ends) {
    Channel c;
    c.constant = 7.0;
    CHECK_EQ(c.value_at(0), 7.0);

    c.keys = {Keyframe{10, 0.0, false}, Keyframe{20, 100.0, false}};
    // Held before the first key and after the last, linear in between.
    CHECK_EQ(c.value_at(0), 0.0);
    CHECK_EQ(c.value_at(30), 100.0);
    CHECK_EQ(c.value_at(15), 50.0);

    // Eased, the midpoint is still the midpoint, but the quarter point is not.
    c.keys = {Keyframe{10, 0.0, true}, Keyframe{20, 100.0, true}};
    CHECK_EQ(c.value_at(15), 50.0);
    CHECK(c.value_at(12) < 25.0);
}

LTEST_CASE(a_graphic_reports_the_data_fields_its_layers_bind) {
    Graphic g;
    g.id = Id::generate();
    g.name = "Lower Third";

    Layer name;
    name.id = Id::generate();
    name.kind = LayerKind::Text;
    name.name = "Name";
    name.text = "Jane Doe";
    name.data_field = "name";

    Layer title;
    title.id = Id::generate();
    title.kind = LayerKind::Text;
    title.name = "Title";
    title.data_field = "title";

    Layer bar;
    bar.id = Id::generate();
    bar.kind = LayerKind::Rect;
    bar.name = "Bar";

    g.layers = {bar, name, title};
    const auto fields = g.data_fields();
    REQUIRE(fields.size() == 2);
    CHECK_EQ(fields[0], std::string{"name"});
    CHECK_EQ(fields[1], std::string{"title"});
    CHECK(!g.validate().has_value());
}

LTEST_CASE(every_graphic_edit_round_trips_through_undo_and_redo) {
    Graphic g;
    g.id = Id::generate();
    g.name = "Lower Third";

    Layer bar;
    bar.id = Id::generate();
    bar.name = "Bar";
    g.layers.push_back(bar);

    Layer text;
    text.id = Id::generate();
    text.kind = LayerKind::Text;
    text.name = "Name";
    g.layers.push_back(text);

    const Graphic original = g;
    GraphicHistory history{g};

    Channel animated;
    animated.keys = {Keyframe{0, -400.0, true}, Keyframe{12, 60.0, true}};

    const std::vector<GraphicCommand> edits{
        SetGraphicName{"Renamed"},
        AddLayer{[] {
                     Layer l;
                     l.id = Id::generate();
                     l.name = "Shadow";
                     return l;
                 }(),
                 0},
        RemoveLayer{bar.id},
        MoveLayer{text.id, 0},
        SetLayer{text.id, "Presenter", true, false, 0x112233, "Jane", "name", 64.0},
        SetChannel{text.id, ChannelId::X, animated},
        SetGraphicFormat{Fps{25, 1}, 1280, 720, 200},
    };

    for (const GraphicCommand& edit : edits) {
        const Graphic before = history.document();
        REQUIRE(history.run(edit).has_value());
        CHECK(history.document() != before);
        REQUIRE(history.undo().has_value());
        CHECK(history.document() == before);
        REQUIRE(history.redo().has_value());
        CHECK(!history.document().validate().has_value());
    }

    while (history.can_undo())
        REQUIRE(history.undo().has_value());
    CHECK(history.document() == original);
}

LTEST_CASE(keyframes_out_of_order_are_refused_before_they_enter_the_document) {
    Graphic g;
    g.id = Id::generate();
    g.name = "G";
    Layer l;
    l.id = Id::generate();
    l.name = "L";
    g.layers.push_back(l);
    const Graphic before = g;

    Channel backwards;
    backwards.keys = {Keyframe{20, 0.0, true}, Keyframe{10, 1.0, true}};
    CHECK(!apply(g, SetChannel{l.id, ChannelId::X, backwards}).has_value());

    Channel doubled;
    doubled.keys = {Keyframe{10, 0.0, true}, Keyframe{10, 1.0, true}};
    CHECK(!apply(g, SetChannel{l.id, ChannelId::X, doubled}).has_value());

    CHECK(g == before);
}

// ---- the isolation guarantee --------------------------------------------------------------------

LTEST_CASE(undo_on_one_document_never_reaches_another) {
    // The operator's own words: edit on Live, switch to Edit, edit there, come back to Live and
    // undo. What goes back has to be the last Live edit, not the timeline edit made in between.
    Project project;
    project.name = "Show";
    Canvas main;
    main.id = Id::generate();
    main.name = "Main";
    // A second canvas, because canvases[0] is Main and Main cannot be renamed (ADR-0027).
    Canvas canvas;
    canvas.id = Id::generate();
    canvas.name = "Vertical";
    project.canvases = {main, canvas};

    DocumentHistory show{project};
    Workbench bench;

    Fixture f;
    TimelineHistory& edit = bench.open_timeline(f.timeline);

    // On Live: rename the show.
    REQUIRE(show.run(SetProjectName{"Morning Show"}).has_value());
    // On Edit: move a clip.
    REQUIRE(edit.run(MoveClip{f.track, f.clip_b, 500, Id{}}).has_value());
    // Back on Live: rename a canvas.
    Canvas renamed = canvas;
    renamed.name = "Portrait";
    REQUIRE(show.run(SetCanvas{renamed}).has_value());

    // Undo with Live focused. The canvas rename goes back -- not the clip move, which is the
    // most recent edit in wall-clock order.
    REQUIRE(show.undo().has_value());
    CHECK_EQ(show.project().canvases[1].name, std::string{"Vertical"});
    CHECK_EQ(show.project().name, std::string{"Morning Show"});
    // The timeline is untouched by the show's undo.
    CHECK_EQ(edit.document().track(f.track)->clip(f.clip_b)->start, Frame{500});

    // Now undo with the timeline focused: its own last edit, and the show stays put.
    REQUIRE(edit.undo().has_value());
    CHECK_EQ(edit.document().track(f.track)->clip(f.clip_b)->start, Frame{200});
    CHECK_EQ(show.project().name, std::string{"Morning Show"});
}

LTEST_CASE(two_timelines_keep_separate_stacks_and_separate_positions) {
    Workbench bench;
    Fixture one;
    Fixture two;
    two.timeline.id = Id::generate();
    two.timeline.name = "Second";

    TimelineHistory& a = bench.open_timeline(one.timeline);
    TimelineHistory& b = bench.open_timeline(two.timeline);

    REQUIRE(a.run(SetTimelineName{"A edited"}).has_value());
    REQUIRE(b.run(SetTimelineName{"B edited"}).has_value());
    REQUIRE(b.run(MoveClip{two.track, two.clip_b, 400, Id{}}).has_value());

    // Undoing everything on B leaves A's stack exactly as deep as it was.
    CHECK_EQ(b.undo_depth(), std::size_t{2});
    CHECK_EQ(a.undo_depth(), std::size_t{1});
    while (b.can_undo())
        REQUIRE(b.undo().has_value());
    CHECK_EQ(a.undo_depth(), std::size_t{1});
    CHECK_EQ(a.document().name, std::string{"A edited"});
}

LTEST_CASE(reopening_an_open_document_keeps_its_undo_stack) {
    Workbench bench;
    Fixture f;
    bench.open_timeline(f.timeline).run(SetTimelineName{"Edited"}).value();

    // Whoever opens it second gets the editor that is already there, not a fresh one: silently
    // discarding the first caller's work is the bug this guards.
    TimelineHistory& again = bench.open_timeline(f.timeline);
    CHECK_EQ(again.undo_depth(), std::size_t{1});
    CHECK_EQ(again.document().name, std::string{"Edited"});

    // Closing it is what drops the stack, and that is the only thing that does.
    bench.close_timeline(f.timeline.id);
    CHECK(bench.timeline(f.timeline.id) == nullptr);
    CHECK_EQ(bench.open_timeline(f.timeline).undo_depth(), std::size_t{0});
}

LTEST_CASE(the_workbench_reports_which_documents_have_unsaved_work) {
    Workbench bench;
    Fixture f;
    Graphic g;
    g.id = Id::generate();
    g.name = "Lower Third";

    TimelineHistory& timeline = bench.open_timeline(f.timeline);
    bench.open_graphic(g);
    CHECK(bench.dirty_documents().empty());

    REQUIRE(timeline.run(SetTimelineName{"Edited"}).has_value());
    REQUIRE(bench.dirty_documents().size() == 1);
    CHECK_EQ(bench.dirty_documents()[0], f.timeline.id);

    // An undo back to the saved state still leaves the file needing a write.
    REQUIRE(timeline.undo().has_value());
    CHECK_EQ(bench.dirty_documents().size(), std::size_t{1});

    timeline.note_saved();
    CHECK(bench.dirty_documents().empty());
}

// ---- files --------------------------------------------------------------------------------

LTEST_CASE(a_timeline_round_trips_through_its_own_file) {
    Fixture f;
    f.timeline.markers.push_back(Marker{Id::generate(), 120, "Chorus", 0x00AAFF});
    f.timeline.in_point = 10;
    f.timeline.out_point = 250;
    f.timeline.playhead = 42;

    const std::string text = timeline_to_json(f.timeline, 7);
    const auto read = timeline_from_json(text);
    REQUIRE_MSG(read.has_value(), "{}", read.error());
    CHECK_EQ(read->generation, std::uint64_t{7});
    CHECK(read->document == f.timeline);
}

LTEST_CASE(a_graphic_round_trips_through_its_own_file) {
    Graphic g;
    g.id = Id::generate();
    g.name = "Lower Third";
    g.fps = Fps{25, 1};
    g.duration = 200;

    Layer l;
    l.id = Id::generate();
    l.kind = LayerKind::Text;
    l.name = "Name";
    l.text = "Jane Doe";
    l.data_field = "name";
    l.font_size = 64.0;
    l.color = 0x102030;
    // One animated channel and one constant, so both shapes are exercised.
    l.x.keys = {Keyframe{0, -400.0, true}, Keyframe{12, 60.0, false}};
    l.opacity.constant = 0.5;
    g.layers.push_back(std::move(l));

    const std::string text = graphic_to_json(g, 3);
    const auto read = graphic_from_json(text);
    REQUIRE_MSG(read.has_value(), "{}", read.error());
    CHECK_EQ(read->generation, std::uint64_t{3});
    CHECK(read->document == g);
}

LTEST_CASE(a_corrupt_timeline_file_is_refused_rather_than_half_read) {
    // Clips that overlap parse as JSON but are not a timeline any edit operation can work on,
    // so the reader refuses the file instead of handing back a document that breaks later.
    Fixture f;
    f.timeline.tracks[0].clips[1].start = 50; // on top of clip A
    const std::string text = timeline_to_json(f.timeline, 1);
    CHECK(!timeline_from_json(text).has_value());

    CHECK(!timeline_from_json("{").has_value());
    CHECK(!timeline_from_json("[]").has_value());
    CHECK(!timeline_from_json("{\"name\": \"no version\"}").has_value());
}

LTEST_MAIN("subdocument")
