// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "theme.hpp"

#include "ltheme/omarchy.hpp"

#include <QColor>

#include <filesystem>

#include <array>
#include <utility>

namespace munkicast::ui {
namespace {

/// Role to QML key. camelCase because `theme.c.panelBackground` is a property access and
/// `theme.c["panel-background"]` is a lookup nobody enjoys reading.
constexpr std::array<std::pair<ltheme::Role, const char*>, ltheme::role_count()> kKeys = {{
    {ltheme::Role::Window, "window"},
    {ltheme::Role::PanelBackground, "panelBackground"},
    {ltheme::Role::PanelHeader, "panelHeader"},
    {ltheme::Role::PanelHeaderActive, "panelHeaderActive"},
    {ltheme::Role::Border, "border"},
    {ltheme::Role::Divider, "divider"},
    {ltheme::Role::DividerHover, "dividerHover"},
    {ltheme::Role::Text, "text"},
    {ltheme::Role::TextMuted, "textMuted"},
    {ltheme::Role::Accent, "accent"},
    {ltheme::Role::DropFill, "dropFill"},
    {ltheme::Role::DropBorder, "dropBorder"},
    {ltheme::Role::Good, "good"},
    {ltheme::Role::Warning, "warning"},
    {ltheme::Role::Error, "error"},
}};

QColor to_qcolor(ltheme::Color c) {
    return QColor{c.r, c.g, c.b, c.a};
}

} // namespace

Theme::Theme(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&debounce_, &QTimer::timeout, this, &Theme::reload);

    const auto queue = [this] { debounce_.start(); };
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, queue);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, queue);

    apply(palette_, name_);
    reload();
}

void Theme::rewatch() {
    // Watches are re-established from scratch every read. A theme switch replaces the files we
    // were watching, and an inotify watch follows the inode, not the path -- so a watch kept
    // across a switch is watching a file nobody will ever write again.
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());

    for (const auto& p : ltheme::watch_paths()) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec))
            watcher_.addPath(QString::fromStdString(p.string()));
    }
}

void Theme::apply(const ltheme::Palette& p, QString theme_name) {
    palette_ = p;
    name_ = std::move(theme_name);
    colors_.clear();
    for (const auto& [role, key] : kKeys)
        colors_[QString::fromLatin1(key)] = to_qcolor(palette_.get(role));
    emit changed();
}

void Theme::reload() {
    debounce_.stop();
    rewatch();

    if (!ltheme::omarchy_present()) {
        // Not an Omarchy machine, or Omarchy has not written a theme yet. The built-in palette is
        // the correct answer, not an error.
        if (palette_.source() != "built-in")
            apply(ltheme::Palette::fallback(), QStringLiteral("built-in"));
        return;
    }

    auto t = ltheme::read_omarchy();
    if (!t) {
        emit notice(QStringLiteral("Could not read the Omarchy theme: %1")
                        .arg(QString::fromStdString(t.error())));
        return;
    }
    if (t->palette == palette_)
        return; // the file was touched but nothing we draw with changed

    if (!t->missing_tokens.empty()) {
        emit notice(QStringLiteral("Theme '%1' is missing %2 token(s); using built-in values "
                                   "for those")
                        .arg(QString::fromStdString(t->name))
                        .arg(t->missing_tokens.size()));
    }
    apply(t->palette, QString::fromStdString(t->name));
}

} // namespace munkicast::ui
