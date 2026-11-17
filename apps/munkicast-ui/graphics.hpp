// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <ldoc/workbench.hpp>

#include <filesystem>

namespace munkicast::ui {

/// The Graphics workspace: authoring the animated overlays a show plays back.
///
/// Its undo stack is its own, on the same terms as `Edit`. A keyframe moved here cannot be
/// undone from the Live tab, and a scene change on Live cannot be undone from here.
///
/// A graphic is authored as a `.manim` document and *played back* as HTML5: `exportHtml` writes
/// a self-contained page that a browser source plays, with the layers bound to fields the show
/// fills in at run time. Authoring in a document rather than in hand-written HTML is what makes
/// the timeline, the undo and the data bindings possible; shipping HTML is what makes the result
/// playable by anything, including playout systems that are not this one.
class Graphics : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantMap graphic READ graphic NOTIFY changed)
    Q_PROPERTY(bool hasGraphic READ hasGraphic NOTIFY changed)
    Q_PROPERTY(QVariantList graphics READ graphics NOTIFY listChanged)
    Q_PROPERTY(QString selectedLayer READ selectedLayer NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selectedLayerDetails READ selectedLayerDetails NOTIFY selectionChanged)
    /// Every layer as it stands at the playhead: the values the stage draws.
    Q_PROPERTY(QVariantList frame READ frame NOTIFY changed)
    Q_PROPERTY(QStringList dataFields READ dataFields NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)

public:
    static Graphics* create(QQmlEngine* engine, QJSEngine*);



    [[nodiscard]] QVariantMap graphic() const;
    [[nodiscard]] bool hasGraphic() const { return !open_.is_nil(); }
    [[nodiscard]] QVariantList graphics() const;
    [[nodiscard]] QString selectedLayer() const;
    [[nodiscard]] QVariantMap selectedLayerDetails() const;
    [[nodiscard]] QVariantList frame() const;
    [[nodiscard]] QStringList dataFields() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] bool dirty() const;

    Q_INVOKABLE QString createGraphic(const QString& name);
    Q_INVOKABLE void openGraphic(const QString& id);
    Q_INVOKABLE void save();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void endGesture();

    Q_INVOKABLE void setPlayhead(int frame);
    Q_INVOKABLE void addLayer(const QString& kind);
    Q_INVOKABLE void removeSelectedLayer();
    Q_INVOKABLE void selectLayer(const QString& id);
    /// Move the selected layer on the stage. One undo step per drag.
    Q_INVOKABLE void moveSelectedLayer(double x, double y);
    Q_INVOKABLE void resizeSelectedLayer(double width, double height);
    /// Set a property of the selected layer: "name", "text", "dataField", "color",
    /// "fontSize", "visible", "locked".
    Q_INVOKABLE void setLayerProperty(const QString& key, const QVariant& value);
    /// Put a keyframe on one of the selected layer's channels at the playhead, holding its
    /// current value. Removing one is the same call with `on` false.
    Q_INVOKABLE void setKeyframe(const QString& channel, bool on);
    /// Whether that channel has a keyframe exactly at the playhead, for the diamond's state.
    Q_INVOKABLE [[nodiscard]] bool hasKeyframe(const QString& channel) const;

    /// Write the graphic as a self-contained HTML page into the project's Graphics folder, and
    /// return its path. This is what a browser source plays.
    Q_INVOKABLE QString exportHtml();

    void setProject(const std::filesystem::path& dir);
    [[nodiscard]] ldoc::Workbench& workbench() { return bench_; }

signals:
    void changed();
    void listChanged();
    void selectionChanged();
    void notice(const QString& text);

private:
    /// Private so that `create` is the only way in.
    ///
    /// A QML singleton with a public default constructor gets default-constructed, and the
    /// `create` overload -- which is where it is wired to `Document` -- is never called. That
    /// fails quietly: the workspace comes up with no project and refuses every edit, saying
    /// only "open a project first" about one that is already open.
    explicit Graphics(QObject* parent = nullptr);

    [[nodiscard]] ldoc::GraphicHistory* history();
    [[nodiscard]] const ldoc::GraphicHistory* history() const;
    void run(const ldoc::GraphicCommand& command, const QString& gesture = {});
    /// The selected layer, or nothing. Every editing call starts here.
    [[nodiscard]] const ldoc::Layer* selected() const;

    class Document* document_ = nullptr;
    ldoc::Workbench bench_;
    ldoc::Id open_;
    ldoc::Id selected_layer_;
    std::filesystem::path dir_;
};

} // namespace munkicast::ui
