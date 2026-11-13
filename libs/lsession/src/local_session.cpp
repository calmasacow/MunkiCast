// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lsession/local_session.hpp"

#include <algorithm>
#include <format>
#include <sstream>
#include <utility>

namespace lsession {

namespace {

// A take changes only what is on air: a deck's program or preview. On-air state is not part of
// the show -- it is never saved, never journaled, not undoable, and every open starts on the
// emergency scene. A cue that has to create a deck for a linked canvas is structural, so a
// batch containing anything else stays an ordinary edit.
bool is_take(const ldoc::Command& command) {
    if (const auto* batch = std::get_if<ldoc::Batch>(&command)) {
        if (!batch->body || batch->body->commands.empty()) return false;
        return std::ranges::all_of(batch->body->commands, [](const auto& c) { return is_take(c); });
    }
    return std::holds_alternative<ldoc::SetDeckProgram>(command) ||
           std::holds_alternative<ldoc::SetDeckPreview>(command);
}

// A deck edit carries the whole deck, program and preview included. Journal it without them so
// on-air state never reaches disk by that route either.
ldoc::Command without_on_air(ldoc::Command command) {
    const auto strip = [](ldoc::Command& c) {
        if (auto* set = std::get_if<ldoc::SetDeck>(&c)) {
            set->value.program_scene.reset();
            set->value.preview_scene.reset();
        }
        if (auto* add = std::get_if<ldoc::AddDeck>(&c)) {
            add->value.program_scene.reset();
            add->value.preview_scene.reset();
        }
    };
    if (auto* batch = std::get_if<ldoc::Batch>(&command); batch && batch->body) {
        auto steps = batch->body->commands;
        std::erase_if(steps, [](const auto& c) {
            return std::holds_alternative<ldoc::SetDeckProgram>(c) ||
                   std::holds_alternative<ldoc::SetDeckPreview>(c);
        });
        for (auto& step : steps) strip(step);
        return ldoc::make_batch(std::move(steps), batch->description);
    }
    strip(command);
    return command;
}

// Expand cues before history and journaling, so replay and undo restore explicit deck states
// without firing cues again. Generated deck identities are recorded in the expanded command.
Command with_scene_links(const Project& project, const Command& command) {
    if (const auto* batch = std::get_if<ldoc::Batch>(&command); batch && batch->body) {
        auto staged = project;
        std::vector<Command> steps;
        for (const auto& step : batch->body->commands) {
            auto expanded = with_scene_links(staged, step);
            if (!ldoc::apply(staged, expanded)) return command;
            if (const auto* linked = std::get_if<ldoc::Batch>(&expanded); linked && linked->body)
                steps.insert(steps.end(), linked->body->commands.begin(), linked->body->commands.end());
            else
                steps.push_back(std::move(expanded));
        }
        return ldoc::make_batch(std::move(steps), batch->description);
    }
    const ldoc::Deck* source = nullptr;
    std::optional<ldoc::Id> scene_id;
    if (const auto* set = std::get_if<ldoc::SetDeckProgram>(&command)) {
        for (const auto& deck : project.decks)
            if (deck.id == set->deck)
                source = &deck;
        scene_id = set->scene;
    } else if (const auto* add = std::get_if<ldoc::AddDeck>(&command)) {
        source = &add->value;
        scene_id = source->program_scene;
    }
    const auto* scene = scene_id ? project.scene(*scene_id) : nullptr;
    if (!source || !scene || project.canvases.empty() ||
        source->canvas != project.canvases.front().id || scene->canvas != source->canvas)
        return command;

    std::vector<Command> steps{command};
    std::size_t next_deck = project.decks.size() +
                            (std::holds_alternative<ldoc::AddDeck>(command) ? 1 : 0);
    for (const auto& link : project.scene_links) {
        if (link.from != scene->id)
            continue;
        const auto* target = project.scene(link.to);
        const auto* canvas = target ? project.canvas(target->canvas) : nullptr;
        if (!canvas || canvas->id == source->canvas || canvas->emergency_scene == link.to)
            continue;
        const auto deck = std::find_if(project.decks.begin(), project.decks.end(),
                                      [&](const auto& d) { return d.canvas == canvas->id; });
        if (deck != project.decks.end()) {
            if (deck->program_scene != link.to)
                steps.push_back(ldoc::SetDeckProgram{deck->id, link.to});
        } else {
            ldoc::Deck created;
            created.id = ldoc::Id::generate();
            created.name = canvas->name;
            created.canvas = canvas->id;
            created.program_scene = link.to;
            steps.push_back(ldoc::AddDeck{std::move(created), next_deck++});
        }
    }
    return steps.size() == 1 ? command
                             : ldoc::make_batch(std::move(steps), ldoc::describe(command));
}

} // namespace

LocalSession::LocalSession(Project p) : history_(std::move(p)) {}


std::expected<std::unique_ptr<LocalSession>, std::string> LocalSession::open(
    const std::filesystem::path& dir) {
    auto opened = ldoc::open_project(dir);
    if (!opened)
        return std::unexpected(opened.error());

    if (opened->read_only)
        return std::unexpected("Project uses a newer schema; this build cannot open it for editing.");

    std::unique_ptr<LocalSession> s{new LocalSession{std::move(opened->project)}};
    s->lock_ = std::move(opened->lock);
    s->dir_ = dir;
    opened->problems.resize(opened->problems.size() - opened->validation_problem_count);
    s->open_problems_ = std::move(opened->problems);

    // Journal entries replayed on open are real changes that the project file does not yet hold,
    // so the document is dirty before the operator has touched anything. Saying otherwise would
    // let them close it and lose exactly the work that was just recovered.
    s->dirty_ = opened->recovered_commands > 0;

    // A journal that cannot be opened is a warning, not a refusal to open the project. The
    // operator can still work; they just cannot be promised that unsaved work survives a crash,
    // and that is worth saying loudly rather than making the project unopenable.
    auto journal = ldoc::Journal::open(ldoc::journal_path(dir));
    if (journal) {
        s->journal_ = std::move(*journal);
    } else {
        s->open_problems_.push_back(
            Problem{ldoc::Severity::Warning,
                    std::format("the recovery journal could not be opened ({}), so changes made "
                                "now will be lost if the application stops before you save",
                                journal.error()),
                    ldoc::Id::nil()});
    }
    return s;
}

std::expected<std::unique_ptr<LocalSession>, std::string> LocalSession::create(
    const Project& p, const std::filesystem::path& dir) {
    if (const auto r = ldoc::create_project(p, dir); !r)
        return std::unexpected(r.error());
    return open(dir);
}

std::unique_ptr<LocalSession> LocalSession::in_memory(Project p) {
    return std::unique_ptr<LocalSession>{new LocalSession{std::move(p)}};
}

const Project& LocalSession::project() const { return history_.project(); }

std::vector<Problem> LocalSession::problems() const {
    std::vector<Problem> out = open_problems_;
    for (Problem& p : ldoc::validate(history_.project()))
        out.push_back(std::move(p));
    // File availability belongs to this machine, not the document's structural validity.
    for (const auto& source : project().sources) {
        const bool list = source.type == "source.playlist" || source.type == "source.slideshow";
        const char* key = source.type == "source.media" ? "localFile"
                          : source.type == "source.image" ? "file"
                          : list ? "files" : nullptr;
        if (!key)
            continue;
        const auto setting = source.settings.find(key);
        const auto* value = setting == source.settings.end()
                                ? nullptr : std::get_if<std::string>(&setting->second);
        if (!value || value->empty()) {
            out.push_back({ldoc::Severity::Error,
                           std::format("No media file selected for source '{}'", source.name),
                           source.id});
            continue;
        }
        const auto check = [&](const std::string& path) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec))
                out.push_back({ldoc::Severity::Error,
                               std::format("Media file missing or inaccessible for source '{}': {}",
                                           source.name, path), source.id});
        };
        if (list) {
            std::istringstream paths{*value};
            for (std::string path; std::getline(paths, path);)
                if (!path.empty())
                    check(path);
        } else {
            check(*value);
        }
    }
    return out;
}

std::string LocalSession::undo_description() const { return history_.undo_description(); }
std::string LocalSession::redo_description() const { return history_.redo_description(); }

void LocalSession::notify(Change::Kind kind, std::optional<Command> command) {
    const Change change{kind, std::move(command), revision_};
    // Over a copy: a listener may unsubscribe itself, and several will once the UI has panels.
    const auto snapshot = listeners_;
    for (const auto& [token, listener] : snapshot) {
        (void)token;
        if (listener)
            listener(change);
    }
}

void LocalSession::notice(ldoc::Severity severity, const std::string& text) {
    if (notice_handler_)
        notice_handler_(severity, text);
}

void LocalSession::journal(const Command& command) {
    if (!journal_.is_open())
        return; // an in-memory document has nowhere to record to, and that is not a failure
    if (const auto r = journal_.record(without_on_air(command), history_.project().generation); !r) {
        notice(ldoc::Severity::Error,
               std::format("this change was made but could not be written to the recovery "
                           "journal, so it would be lost if the application stopped now: {}",
                           r.error()));
    }
}

std::expected<std::string, std::string> LocalSession::run(const Command& command) {
    return run(command, std::string_view{});
}

std::expected<std::string, std::string> LocalSession::run(const Command& command,
                                                          std::string_view gesture) {
    const Command expanded = with_scene_links(project(), command);
    if (is_take(expanded)) {
        if (auto r = history_.replay(expanded); !r)
            return std::unexpected(r.error());
        ++revision_;
        notify(Change::Kind::Applied, expanded);
        return ldoc::describe(expanded);
    }
    auto description =
        gesture.empty() ? history_.run(expanded) : history_.run(expanded, gesture);
    if (!description)
        return std::unexpected(description.error());

    // Journaled and notified per command even when the undo stack collapsed them. The journal
    // has to match what was applied, and a listener that only saw the last value of a drag
    // would draw a fader that jumped rather than followed.
    journal(expanded);
    dirty_ = true;
    edit_revision_ = ++revision_;
    notify(Change::Kind::Applied, expanded);
    return *description;
}

void LocalSession::end_gesture() {
    history_.end_gesture();
}

std::expected<void, std::string> LocalSession::undo() {
    auto applied = history_.undo();
    if (!applied)
        return std::unexpected(applied.error());

    // An undo is journaled like anything else. It is a change the project file does not hold
    // yet, and a crash straight after one should not bring the undone work back.
    journal(*applied);
    dirty_ = true;
    edit_revision_ = ++revision_;
    notify(Change::Kind::Undone, *applied);
    return {};
}

std::expected<void, std::string> LocalSession::redo() {
    auto applied = history_.redo();
    if (!applied)
        return std::unexpected(applied.error());
    journal(*applied);
    dirty_ = true;
    edit_revision_ = ++revision_;
    notify(Change::Kind::Redone, *applied);
    return {};
}

std::expected<Project, std::string> LocalSession::begin_save() {
    if (dir_.empty())
        return std::unexpected("this project has not been given a location yet");
    Project copy = history_.project();
    ldoc::clear_on_air(copy);
    // From here, new commands are journaled against the generation this save will produce, into
    // a fresh file; what was journaled so far waits in the rotated file until the save commits.
    if (journal_.is_open()) {
        if (const auto r = journal_.rotate(ldoc::saving_journal_path(dir_)); !r)
            return std::unexpected(r.error());
    }
    history_.note_saved(copy.generation + 1);
    return copy;
}

void LocalSession::finish_save(const std::expected<std::uint64_t, std::string>& result,
                               std::uint64_t revision_at_snapshot) {
    if (!result) {
        // The rotated journal still holds that work and is replayed on the next open. Put the
        // generation back so entries made meanwhile still match what is on disk.
        history_.note_saved(history_.project().generation - 1);
        return;
    }
    // Take the new generation but keep the undo stack: saving is not something the operator did
    // to the document and must not throw away what they can undo.
    history_.note_saved(*result);
    std::error_code ec;
    std::filesystem::remove(ldoc::saving_journal_path(dir_), ec);
    // Clean only if nothing was edited after the snapshot; those edits are in the new journal.
    if (edit_revision_ <= revision_at_snapshot)
        dirty_ = false;
    ++revision_;
    notify(Change::Kind::Saved, std::nullopt);
}

std::expected<void, std::string> LocalSession::save() {
    wait_for_save();
    const std::uint64_t at = revision_;
    auto copy = begin_save();
    if (!copy)
        return std::unexpected(copy.error());
    const auto r = ldoc::save_project(*copy, dir_);
    finish_save(r ? std::expected<std::uint64_t, std::string>{copy->generation}
                  : std::expected<std::uint64_t, std::string>{std::unexpected(r.error())},
                at);
    if (!r)
        return std::unexpected(r.error());
    return {};
}

void LocalSession::save_async(Post post,
                              std::function<void(std::expected<void, std::string>)> done) {
    if (saving_) {
        // Coalesce: one more save after the one in flight, with the latest callbacks.
        save_again_ = true;
        again_post_ = std::move(post);
        again_done_ = std::move(done);
        return;
    }
    const std::uint64_t at = revision_;
    auto copy = begin_save();
    if (!copy) {
        if (done)
            done(std::unexpected(copy.error()));
        return;
    }
    saving_ = true;
    save_at_ = at;
    save_done_ = std::move(done);
    auto outcome = std::make_shared<std::optional<std::expected<std::uint64_t, std::string>>>();
    save_outcome_ = outcome;
    saver_ = std::jthread([this, project = std::move(*copy), dir = dir_, post = std::move(post),
                           outcome]() mutable {
        auto r = ldoc::save_project(project, dir);
        *outcome = r ? std::expected<std::uint64_t, std::string>{project.generation}
                     : std::expected<std::uint64_t, std::string>{std::unexpected(r.error())};
        // Completion runs on the session's thread. If wait_for_save() already finished it, the
        // posted call finds nothing in flight and does nothing.
        post([this, outcome] {
            if (saving_ && save_outcome_ == outcome)
                complete_save();
        });
    });
}

void LocalSession::complete_save() {
    if (saver_.joinable())
        saver_.join();
    const auto result = save_outcome_ && *save_outcome_ ? **save_outcome_
        : std::expected<std::uint64_t, std::string>{std::unexpected("save did not finish")};
    save_outcome_.reset();
    finish_save(result, save_at_);
    saving_ = false;
    auto done = std::move(save_done_);
    save_done_ = {};
    if (done)
        done(result ? std::expected<void, std::string>{}
                    : std::expected<void, std::string>{std::unexpected(result.error())});
    if (save_again_) {
        save_again_ = false;
        save_async(std::move(again_post_), std::move(again_done_));
    }
}

void LocalSession::wait_for_save() {
    // Finish any save in flight now, on this thread, so the next save starts from settled state.
    // A queued follow-up save is started by complete_save(), so wait for that one as well.
    while (saving_)
        complete_save();
}

LocalSession::~LocalSession() {
    save_again_ = false;
    if (saving_) {
        if (saver_.joinable())
            saver_.join();
        saving_ = false;
    }
}

std::expected<void, std::string> LocalSession::save_as(const std::filesystem::path& dir) {
    if (!dir_.empty())
        return std::unexpected("this project already has a location");

    Project copy = history_.project();
    copy.generation = 0;
    if (const auto r = ldoc::create_project(copy, dir); !r)
        return std::unexpected(r.error());

    // open_project returns the lock; taking one separately first would deadlock against it, now
    // that the lock correctly refuses a second holder inside one process.
    auto opened = ldoc::open_project(dir);
    if (!opened)
        return std::unexpected(opened.error());

    lock_ = std::move(opened->lock);
    dir_ = dir;
    opened->problems.resize(opened->problems.size() - opened->validation_problem_count);
    for (Problem& p : opened->problems)
        open_problems_.push_back(std::move(p));

    auto journal = ldoc::Journal::open(ldoc::journal_path(dir));
    if (journal) {
        journal_ = std::move(*journal);
    } else {
        open_problems_.push_back(
            Problem{ldoc::Severity::Warning,
                    std::format("the recovery journal could not be opened ({}), so changes made "
                                "now will be lost if the application stops before you save",
                                journal.error()),
                    ldoc::Id::nil()});
    }

    // The document is unchanged -- it was just written out -- so take the new generation and
    // keep the undo stack, exactly as save() does.
    history_.note_saved(opened->project.generation);
    dirty_ = false;
    ++revision_;
    notify(Change::Kind::Saved, std::nullopt);
    return {};
}

int LocalSession::subscribe(ChangeListener listener) {
    const int token = next_token_++;
    listeners_.emplace(token, std::move(listener));
    return token;
}

void LocalSession::unsubscribe(int token) { listeners_.erase(token); }

void LocalSession::set_notice_handler(NoticeHandler handler) {
    notice_handler_ = std::move(handler);
}

} // namespace lsession
