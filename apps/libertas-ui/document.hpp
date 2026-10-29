// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "documentmodels.hpp"
#include "connectors.hpp"

#include "lsession/local_session.hpp"

#include <QAbstractItemModel>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

class QJSEngine;
class QQmlEngine;

namespace libertas::ui {

/// The QML-facing face of the document, and the only route the UI has to it.
///
/// Everything here turns a gesture into an `ldoc::Command` and hands it to the session. Nothing
/// in this class, and nothing in QML, holds a mutable `Project` -- ADR-0024's first rule, and the
/// reason the document can move to another process later without the UI noticing.
///
/// Models are rebuilt from the session's change notification rather than from the return value of
/// the call that caused it. That is ADR-0024's second rule, and it is what makes a remote session
/// a drop-in: the notification is the only thing that is guaranteed to arrive after the document
/// has actually changed.
class Document : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* scenes READ scenesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* nodes READ nodesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* sources READ sourcesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* buses READ busesModel CONSTANT)
    /// The scene list filtered to the starred ones. A second model rather than a filter proxy
    /// in QML: the favourites panel must not be able to change the scene list's own selection
    /// or ordering by being looked at.
    Q_PROPERTY(QAbstractItemModel* favouriteScenes READ favouriteScenesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* decks READ decksModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* outputs READ outputsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* canvases READ canvasesModel CONSTANT)
    /// The source strips: one per source node with audio in the scene on screen. Separate from
    /// `buses` because they are different things, not two views of one -- a strip is a place
    /// audio comes from and a bus is where it goes.
    Q_PROPERTY(QAbstractItemModel* mixerStrips READ mixerStripsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* encodes READ encodesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel* audioEncodes READ audioEncodesModel CONSTANT)

    Q_PROPERTY(QVariantList collections READ collections NOTIFY collectionsChanged)
    Q_PROPERTY(QString collectionReport READ collectionReport NOTIFY collectionsChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY changed)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)

    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(QString undoDescription READ undoDescription NOTIFY changed)
    Q_PROPERTY(QString redoDescription READ redoDescription NOTIFY changed)

    Q_PROPERTY(QString selectedScene READ selectedScene NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedNode READ selectedNode NOTIFY selectionChanged)
    /// Every selected node, in the order the scene holds them. `selectedNode` is the last one
    /// clicked and stays the one panels follow; this is what operations act on (ADR-0037).
    Q_PROPERTY(QStringList selectedNodes READ selectedNodes NOTIFY selectionChanged)
    /// Everything the properties panel shows about the selected node, or empty when nothing is
    /// selected.
    Q_PROPERTY(QVariantMap nodeProperties READ nodeProperties NOTIFY selectionChanged)

    /// The canvas every canvas-shaped panel in the shell is working on: which scenes the
    /// scenes panel lists, where a new scene lands, which canvas the emergency button
    /// designates on. Selection, not document state -- it is not saved with the project.
    Q_PROPERTY(QString currentCanvas READ currentCanvas NOTIFY canvasChanged)
    Q_PROPERTY(QString currentCanvasName READ currentCanvasName NOTIFY canvasChanged)
    /// The current canvas is the first one. Linking is a secondary canvas's function, so the
    /// scenes panel shows no link control at all while this is true (ADR-0027).
    Q_PROPERTY(bool currentCanvasIsMain READ currentCanvasIsMain NOTIFY canvasChanged)
    /// How many scenes belong to the current canvas. A canvas still holding scenes cannot be
    /// removed, so the panel greys its own button rather than offering a refused press.
    Q_PROPERTY(int currentCanvasSceneCount READ currentCanvasSceneCount NOTIFY changed)

    Q_PROPERTY(QStringList problems READ problems NOTIFY changed)
    Q_PROPERTY(QString monitoringOutput READ monitoringOutput NOTIFY changed)
    Q_PROPERTY(QStringList blendModes READ blendModes CONSTANT)
    Q_PROPERTY(QVariantList countdowns READ countdowns NOTIFY changed)

public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;
    [[nodiscard]] QVariantList countdowns() const;
    Q_INVOKABLE QVariantList countdownTextSources() const;
    Q_INVOKABLE QVariantList countdownScenes() const;
    Q_INVOKABLE QString addCountdown(const QString& sourceId);
    Q_INVOKABLE void configureCountdown(const QString& sourceId, const QVariantMap& settings);
    Q_INVOKABLE void controlCountdown(const QString& sourceId, const QString& action);
    /// Swap this timer with its neighbour in the dock's order. Not a reorder of the project's
    /// source list, which belongs to the sources panel.
    Q_INVOKABLE void moveCountdown(const QString& sourceId, bool up);
    /// The dock's play-all and stop-all, which only appear once there is more than one timer.
    Q_INVOKABLE void controlAllCountdowns(const QString& action);
    Q_INVOKABLE QVariantMap countdownStatus(const QString& sourceId) const;
    Q_INVOKABLE void removeCountdown(const QString& sourceId);
    Q_INVOKABLE bool importCountdowns(const QString& path);

    /// Where to open from. Empty means a new, unsaved document. Call before the QML engine
    /// instantiates the singleton.
    static void configure(std::filesystem::path project_dir);
    static Document* create(QQmlEngine*, QJSEngine*);

    /// The current canvas's pair. The `scenes` and `nodes` QML properties and the self-check
    /// go through these; a panel bound to one particular canvas uses scenesFor/nodesFor.
    [[nodiscard]] QAbstractItemModel* scenesModel() { return scenesFor(current_canvas_); }
    [[nodiscard]] QAbstractItemModel* nodesModel() { return nodesFor(current_canvas_); }
    [[nodiscard]] QAbstractItemModel* sourcesModel() { return &sources_; }
    [[nodiscard]] QAbstractItemModel* busesModel() { return &buses_; }
    [[nodiscard]] QAbstractItemModel* favouriteScenesModel() { return &favourites_; }
    [[nodiscard]] QAbstractItemModel* decksModel() { return &decks_; }
    [[nodiscard]] QAbstractItemModel* outputsModel() { return &outputs_; }
    [[nodiscard]] QAbstractItemModel* canvasesModel() { return &canvases_; }
    [[nodiscard]] QAbstractItemModel* mixerStripsModel() { return &mixer_strips_; }

    /// The scene list of one canvas, and the node tree of that canvas's selected scene.
    ///
    /// Each canvas gets its own pair of panels -- "Scenes: Main", "Sources: Vertical" -- so
    /// each needs its own model and its own selected scene. An empty `canvasId` means the
    /// first canvas, which is how the layout's plain "scenes" and "sources" panel ids keep
    /// working: Main is `canvases[0]` and the command layer will not let it be otherwise
    /// (ADR-0027), so it is the one canvas whose panel id can be a constant.
    Q_INVOKABLE [[nodiscard]] QAbstractItemModel* scenesFor(const QString& canvasId);
    Q_INVOKABLE [[nodiscard]] QAbstractItemModel* nodesFor(const QString& canvasId);
    /// Which scene that canvas's panels are showing.
    Q_INVOKABLE [[nodiscard]] QString selectedSceneOn(const QString& canvasId) const;
    /// Select a scene in a particular canvas's list. The canvas the shell is working on
    /// follows: clicking a row in "Scenes: Vertical" is how an operator says "Vertical now".
    Q_INVOKABLE void selectSceneOn(const QString& canvasId, const QString& sceneId);
    /// The name a panel title puts after "Scenes: ". Empty `canvasId` means the first canvas.
    Q_INVOKABLE [[nodiscard]] QString canvasNameOf(const QString& canvasId) const;
    /// That canvas's pixel size, so its Program panel can be drawn the shape it really is.
    /// Empty `canvasId` means the first canvas; an unknown one gives an invalid size.
    Q_INVOKABLE [[nodiscard]] QSize canvasSizeOf(const QString& canvasId) const;
    /// A canvas's frame rate as a number, for a panel that wants to say "60.0". Empty id means
    /// the first canvas; an unknown one gives zero. Exact arithmetic stays in the document --
    /// 59.94 is 60000/1001 and this is only for showing.
    Q_INVOKABLE [[nodiscard]] double canvasFpsOf(const QString& canvasId) const;
    /// The rectangle the program picture draws for this node, in canvas pixels, and the terms a
    /// resize needs to keep the opposite edge still. Empty when this canvas's program scene does
    /// not draw that node.
    Q_INVOKABLE [[nodiscard]] QVariantMap placedRect(const QString& canvasId,
                                                     const QString& nodeId) const;
    /// Every such rectangle in the program scene, topmost first, so a click can find a source.
    Q_INVOKABLE [[nodiscard]] QVariantList placedNodes(const QString& canvasId) const;
    /// Select that node, and the program scene the picture is of, so a drag edits the picture.
    Q_INVOKABLE void focusPlacedNode(const QString& canvasId, const QString& nodeId);
    /// Every canvas id, first one first. Drives the per-canvas panels the workspace keeps.
    Q_INVOKABLE [[nodiscard]] QStringList canvasIds() const;
    /// The same list, by name, in the same order.
    Q_INVOKABLE [[nodiscard]] QStringList canvasNames() const;
    [[nodiscard]] QAbstractItemModel* encodesModel() { return &encodes_; }
    [[nodiscard]] QAbstractItemModel* audioEncodesModel() { return &audio_encodes_; }

    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectPath() const;
    [[nodiscard]] bool dirty() const;
    [[nodiscard]] int revision() const;
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    [[nodiscard]] QString undoDescription() const;
    [[nodiscard]] QString redoDescription() const;
    /// The current canvas's selected scene. Properties, Controls and the emergency
    /// designation all mean this one; a panel bound to a particular canvas asks
    /// selectedSceneOn instead.
    [[nodiscard]] QString selectedScene() const { return selectedSceneOn(current_canvas_); }
    [[nodiscard]] QString selectedNode() const { return selected_node_; }
    [[nodiscard]] QStringList selectedNodes() const;

    /// Extend or narrow the selection, the way every list in every other application does.
    /// `ctrl` toggles one node, `shift` takes everything between the anchor and this one.
    /// Neither means a plain click, which selects just this one.
    Q_INVOKABLE void selectNodeExtended(const QString& canvasId, const QString& nodeId,
                                        bool ctrl, bool shift);
    /// Put the selected nodes in a new group, at the topmost one's place (Ctrl+G).
    Q_INVOKABLE void groupSelectedNodes();
    /// The group a node sits in, or "" when it sits at the top of the scene.
    Q_INVOKABLE [[nodiscard]] QString parentOfNode(const QString& nodeId) const;
    /// Take a group apart: its children move to where it was, in order, and the group goes.
    /// The children keep their own transforms, so what the group's transform was doing to
    /// them is lost -- see ADR-0037.
    Q_INVOKABLE void ungroupNode(const QString& nodeId);

    /// Remove every selected source, as one undoable step. A node inside another selected node
    /// is left to its parent rather than removed twice (ADR-0037).
    Q_INVOKABLE void removeSelectedNodes();
    /// Copy every selected source. Pasting puts them all back, in scene order.
    Q_INVOKABLE void copySelectedNodes();
    /// Select every source in the scene on screen (Ctrl+A). Top-level nodes only: taking the
    /// children as well would mean a group and its contents both selected, which every
    /// operation then has to undo again (ADR-0037).
    Q_INVOKABLE void selectAllNodes();

    /// Move every selected source into `groupId`, or out to the top of the scene when it is
    /// empty. The gesture the Sources panel has instead of dragging, which it cannot do yet.
    Q_INVOKABLE void moveSelectedIntoGroup(const QString& groupId);

    /// Drop the selected sources at a row, the way a layers panel does (ADR-0038).
    ///
    /// `zone` is "above", "below" or "into": above and below put them beside `targetId` among
    /// its own siblings, into puts them inside it when it is a group. Refused when the target
    /// is inside what is being dragged -- a node cannot be moved into itself.
    Q_INVOKABLE void dropSelectedAt(const QString& targetId, const QString& zone);
    /// Whether that drop would do anything, so the panel can draw the line only where one
    /// would land.
    Q_INVOKABLE [[nodiscard]] bool canDropAt(const QString& targetId, const QString& zone) const;
    /// The node at a row of the current canvas's flattened tree, and whether it is a group:
    /// {id, isGroup}. What the panel needs to turn a pointer position into a drop target
    /// without reaching into model roles by number.
    Q_INVOKABLE void toggleGroupExpanded(const QString& canvasId, const QString& nodeId);
    Q_INVOKABLE [[nodiscard]] QVariantMap nodeAtRow(int row) const;
    /// The row a node occupies in that flattened tree, or -1. The inverse of `nodeAtRow`,
    /// for drawing the drop line.
    Q_INVOKABLE [[nodiscard]] int rowOfNode(const QString& nodeId) const;

    [[nodiscard]] QVariantMap nodeProperties() const;
    [[nodiscard]] QStringList problems() const;
    [[nodiscard]] QString monitoringOutput() const;
    Q_INVOKABLE [[nodiscard]] QVariantMap monitoringOutputChoices() const;
    Q_INVOKABLE void setMonitoringOutput(const QString& output);
    [[nodiscard]] QStringList blendModes() const;
    [[nodiscard]] QString currentCanvas() const { return current_canvas_; }
    [[nodiscard]] QString currentCanvasName() const;
    [[nodiscard]] bool currentCanvasIsMain() const;
    [[nodiscard]] int currentCanvasSceneCount() const;

    // ---- selection (UI state, not document state) -------------------------------------------

    Q_INVOKABLE void selectScene(const QString& id);
    Q_INVOKABLE void activateScene(const QString& id);
    Q_INVOKABLE void selectNode(const QString& id);
    Q_INVOKABLE void selectCanvas(const QString& id);

    // ---- canvases ----------------------------------------------------------------------------

    Q_INVOKABLE void addCanvas(const QString& name, bool vertical = false);
    Q_INVOKABLE void removeCanvas(const QString& id);
    Q_INVOKABLE void renameCanvas(const QString& id, const QString& name);
    Q_INVOKABLE void setCanvasSize(const QString& id, int width, int height);
    /// The rate as a fraction, because 60000/1001 is a different clock from 60/1 and rounding
    /// it to 59.94 at the model boundary would lose which one the operator meant.
    Q_INVOKABLE void setCanvasFps(const QString& id, int num, int den);

    // ---- scene links -------------------------------------------------------------------------

    /// Secondary canvases offered by the scene linker, as {id, name}.
    Q_INVOKABLE [[nodiscard]] QVariantList sceneLinkCanvases() const;
    /// Every scene on Main, as {id, name, ticked, takenBy}, for the link popup of `sceneId`.
    /// `ticked` is true when that Main scene already brings this scene up; `takenBy` names the
    /// scene it currently brings up on this canvas instead, and is empty when it is free.
    Q_INVOKABLE [[nodiscard]] QVariantList mainScenesForLink(const QString& sceneId) const;
    /// Commit a whole tick-list as one undo step. Links not in the list are removed, links in
    /// it that point elsewhere on this canvas are moved, and the notice names what moved.
    Q_INVOKABLE void commitSceneLinks(const QString& sceneId, const QStringList& mainSceneIds);
    /// How many Main scenes bring `sceneId` up. Drives the chain icon's colour.
    Q_INVOKABLE [[nodiscard]] int sceneLinkCount(const QString& sceneId) const;

    // ---- scenes -----------------------------------------------------------------------------

    Q_INVOKABLE void addScene(const QString& name);
    /// Add a scene to a named canvas. Empty `canvasId` means the first one.
    Q_INVOKABLE void addSceneOn(const QString& canvasId, const QString& name);
    /// Select a node in a particular canvas's tree. The canvas the shell is working on
    /// follows, exactly as it does for a scene.
    Q_INVOKABLE void selectNodeOn(const QString& canvasId, const QString& nodeId);
    Q_INVOKABLE void removeScene(const QString& id);
    Q_INVOKABLE void renameScene(const QString& id, const QString& name);
    Q_INVOKABLE void duplicateScene(const QString& id);
    /// Negative moves it towards the top of the list.
    Q_INVOKABLE void moveScene(const QString& id, int delta);
    Q_INVOKABLE void reorderScene(const QString& id, const QString& target, bool after,
                                 bool favourites = false);
    Q_INVOKABLE void moveSceneInList(const QString& id, int delta, bool favourites = false);
    /// Mark a scene as one to reach for. Favourites are part of the document, not the layout:
    /// they travel with the project and are the same set whichever workspace is on screen.
    Q_INVOKABLE void setSceneFavourite(const QString& id, bool favourite);
    /// Make a scene this canvas's safety net (ADR-0025). Checks suitability first and reports
    /// what is wrong rather than silently accepting a net that will not hold.
    Q_INVOKABLE void setEmergencyScene(const QString& id);
    /// What is wrong with using this scene as a safety net, for the confirmation the designation
    /// flow shows. Empty when it is suitable.
    Q_INVOKABLE [[nodiscard]] QStringList emergencySceneProblems(const QString& id) const;

    // ---- sources (project level; a source node binds one) -------------------------------------

    /// The source types this build can hold in a document. A colour is drawn by the program
    /// preview; nothing else is. The list is here because a document that cannot name what it
    /// is waiting for is harder to build a pipeline against than one that can.
    /// What the add dialog lists down its left side: every source type, plus "Scene".
    ///
    /// Separate from `sourceTypes` deliberately. A scene is not a source -- it has no settings
    /// schema, cannot be created from the "+ Add new" row and is not stored in
    /// `Project::sources` -- so putting it in that list would make every consumer of it treat
    /// a precomp as a kind of source. The dialog is a dialog; this is the dialog's list.
    Q_PROPERTY(QVariantList addableKinds READ addableKinds CONSTANT)
    [[nodiscard]] QVariantList addableKinds() const;

    Q_PROPERTY(QVariantList sourceTypes READ sourceTypes CONSTANT)
    [[nodiscard]] QVariantList sourceTypes() const;

    /// Put a new source of this type into the current scene: make the source, make a source
    /// node, bind them. One `Batch`, so it is one undo step -- nobody pressing Ctrl+Z after
    /// adding a camera wants the node back without its source.
    Q_INVOKABLE void addNewSourceToScene(const QString& name, const QString& type,
                                         bool visible = true);
    /// Put a source the project already has into the current scene. Also one step, and
    /// deliberately does not create a second source: one camera, opened once, in six scenes.
    Q_INVOKABLE void addExistingSourceToScene(const QString& sourceId, bool visible = true);

    /// Put a whole scene into the current scene as one node -- a precomp (ADR-0034).
    ///
    /// A live reference, not a copy: editing the referenced scene changes every scene showing
    /// it. The node carries its own size, where (0,0) means "the canvas I am composited into",
    /// which is how a 2560x1440 overlay scene sits correctly inside a vertical one.
    Q_INVOKABLE void addSceneToScene(const QString& sceneId, bool visible = true);

    // ---- the clipboard ------------------------------------------------------------------
    //
    // Shell state, not document state, and not the system clipboard: what is held is a node or
    // a scene out of *this* project, and pasting it into another process would mean a
    // serialisation format nothing else can read. It is deliberately not in the layout either
    // -- a copied source is not worth restoring on the next launch (ADR-0036).

    /// Copy the selected source node. Empty when nothing is selected.
    Q_INVOKABLE void copySelectedNode();
    /// Copy a whole scene, to be pasted onto any canvas.
    Q_INVOKABLE void copyScene(const QString& sceneId);
    /// Paste what was copied. A node goes into the current scene, a scene onto the current
    /// canvas as a new scene.
    Q_INVOKABLE void paste();
    /// What `paste()` would do, for the menu item: "" when there is nothing to paste.
    Q_INVOKABLE [[nodiscard]] QString pasteDescription() const;
    /// How many sources would land outside the canvas being pasted onto. The paste asks before
    /// it goes ahead when this is not zero -- a landscape layout dropped onto a vertical canvas
    /// mostly misses.
    Q_INVOKABLE [[nodiscard]] int pasteWouldMissCanvas() const;
    /// The scenes that can be put into the current one: every scene except the current scene
    /// itself and any that would make a loop. Each entry is {id, name, canvasName}.
    Q_INVOKABLE [[nodiscard]] QVariantList scenesForPrecomp() const;

    /// Every source of one type, for the add dialog's right-hand grid. Each entry carries what
    /// the tile needs to draw itself: `id`, `name`, `typeTitle`, `useCount`, `selfContained`,
    /// and `swatch` -- a colour string when the source is one we can honestly preview without a
    /// renderer, empty otherwise, in which case the tile falls back to the type's glyph.
    Q_INVOKABLE [[nodiscard]] QVariantList sourcesOfType(const QString& type) const;
    /// The most recently created sources, newest first, whatever their type. Sources are
    /// appended to the project, so this is the tail reversed -- the real order, not a guess.
    Q_INVOKABLE [[nodiscard]] QVariantList recentSources(int limit) const;

    // ---- a source's own settings, behind the gear in the Sources panel ----------------------
    //
    // A source's settings are document state, not pipeline state: they are what the render
    // process will be told to open when it exists, and they serialise into the project today.
    // So editing them now is real work rather than a mock-up -- the same reasoning that applies
    // to the outputs panel, which configures encoders no encoder reads yet.

    /// What a source of this type is configured by, in the order the dialog should draw it.
    /// Each entry carries `key`, `label`, `kind` and `value` -- the type's default -- plus
    /// `choices` and `suffix` where the kind uses them.
    ///
    /// A `note` entry has no key and no value: it is a line of prose the dialog prints above
    /// the field it is about. They are in the schema rather than looked up separately so that
    /// a note sits where it belongs instead of all of them piling up at the top.
    Q_INVOKABLE [[nodiscard]] QVariantList sourceSettingsSchema(const QString& type) const;
    /// Every file this source reads, for asking `AssetCache` about its cache state. Empty for
    /// a source type that is not file-backed, and for one that has gone from the project.
    Q_INVOKABLE [[nodiscard]] QStringList sourceAssetPaths(const QString& id) const;
    Q_INVOKABLE void showFilters(const QString& target);
    Q_INVOKABLE QVariantMap filterDetails(const QString& target) const;
    Q_INVOKABLE QVariantList filterTypes(const QString& owner) const;
    Q_INVOKABLE void addFilter(const QString& owner, const QString& type);
    Q_INVOKABLE void editFilter(const QString& owner, const QString& effect, const QString& name,
                                bool enabled, const QVariantMap& values);
    Q_INVOKABLE void moveFilter(const QString& owner, const QString& effect, int offset);
    Q_INVOKABLE void removeFilter(const QString& owner, const QString& effect);
    Q_INVOKABLE [[nodiscard]] QVariantMap videoFormatChoices(const QString& device) const;

    /// What the compositor says about one monitor: name, description, size and refresh. Empty
    /// when that connector is not connected, which is what the settings dialog shows instead of
    /// a preview it cannot draw. An empty connector means the first screen (ADR-0041).
    Q_INVOKABLE [[nodiscard]] QVariantMap screenDetails(const QString& connector) const;

    /// Whether any canvas's program scene holds a source whose picture changes on its own -- a
    /// screen, a window, a camera, a browser, a playing file. The preview republishes on a timer
    /// only while this is true (ADR-0024 keeps the document the only other reason to republish).
    [[nodiscard]] bool hasLiveSource() const;
    /// Everything the settings dialog needs: the tile fields plus `settings`, which is the
    /// stored values with the type's defaults showing through wherever a key is absent.
    Q_INVOKABLE [[nodiscard]] QVariantMap sourceDetails(const QString& id) const;
    /// Commit a dialog's worth of edits as one `SetSource`, and so one undo step.
    ///
    /// Values are coerced to the kind the schema declares for each key, which is what stops a
    /// field from changing which alternative of `ParamValue` a setting is stored as merely by
    /// being typed into. Keys the schema does not name are left exactly as they were: a project
    /// written by a later build keeps settings this one cannot draw.
    Q_INVOKABLE void applySourceSettings(const QString& id, const QString& name,
                                         const QVariantMap& values);

    /// Add a source to the *project* only: nothing appears in any scene. Not `addSourceNode`,
    /// which is the other half -- a node in the current scene, bound to nothing -- and not
    /// `addNewSourceToScene`, which is both at once and is what the add dialog uses.
    Q_INVOKABLE void addSource(const QString& name, const QString& type);
    Q_INVOKABLE void removeSource(const QString& id);
    /// The scenes holding a node bound to this source, by name, each listed once however many
    /// times it appears there. What the delete confirmation shows before it is agreed to.
    Q_INVOKABLE [[nodiscard]] QStringList scenesUsing(const QString& sourceId) const;
    /// Delete the source *and* every node bound to it, as one undoable step (ADR-0033). The
    /// bare `removeSource` leaves those nodes bound to nothing on purpose; the manager does
    /// not, because "remove it from those scenes" is what the operator was shown and agreed to.
    Q_INVOKABLE void removeSourceAndUses(const QString& id);
    Q_INVOKABLE void renameSource(const QString& id, const QString& name);
    /// Bind the selected source node to this source, or to nothing when the id is empty.
    Q_INVOKABLE void bindSelectedNode(const QString& sourceId);

    // ---- buses (the audio graph) -------------------------------------------------------------
    //
    // Nothing here makes a sound: there is no audio host yet, so the mixer edits routing and
    // levels in the document and says plainly that it is not metering anything. The alternative
    // -- drawing moving meters off a timer -- would make the panel look finished and make it
    // impossible to tell later which half works.

    /// The bus kinds a document can hold, as {id, title, note} for the add menu.
    Q_PROPERTY(QVariantList busKinds READ busKinds CONSTANT)
    [[nodiscard]] QVariantList busKinds() const;

    Q_INVOKABLE void addBus(const QString& name, const QString& kind);
    Q_INVOKABLE void removeBus(const QString& id);
    Q_INVOKABLE void renameBus(const QString& id, const QString& name);
    /// `dragging` true while the fader is under the pointer: the whole drag becomes one undo
    /// step that lands back where it started. The caller must finish with `endGesture()`.
    Q_INVOKABLE void setBusGain(const QString& id, double gainDb, bool dragging = false);
    /// The operator let go. Harmless if no drag was in progress.
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void setBusPan(const QString& id, double pan);
    Q_INVOKABLE void setBusMute(const QString& id, bool mute);
    /// Heard locally. Never changes the program mix, and there is deliberately no solo
    /// (ADR-0031): solo here would be document state, so it would silence other buses on air.
    Q_INVOKABLE void setBusMonitor(const QString& id, bool monitor);
    /// Hides the strip and nothing else: still routed, still on air (ADR-0032).
    Q_INVOKABLE void setBusHidden(const QString& id, bool hidden);
    /// How many strips are hidden, both halves counted. Asked of the document rather than
    /// counted in QML, which would have to name model roles by numeric offset -- the exact
    /// thing that silently re-pointed four self-check assertions when a role was inserted.
    Q_INVOKABLE [[nodiscard]] int hiddenStripCount() const;
    /// Stops the fader and nothing else. Mute still works.
    Q_INVOKABLE void setBusLevelLocked(const QString& id, bool locked);

    /// Route this source in every scene, or disconnect it everywhere when the id is empty.
    /// Unrouted is not the same as muted, and the mixer says which it is.
    Q_INVOKABLE void routeSelectedNode(const QString& busId);
    Q_INVOKABLE void setSelectedNodeGain(double gainDb);
    Q_INVOKABLE void setSelectedNodeMute(bool mute);

    /// The same two edits, keyed by node, for the mixer: it draws many strips and none of them
    /// is the selection, so routing a strip must not require selecting it first.
    Q_INVOKABLE void setNodeGain(const QString& sceneId, const QString& nodeId, double gainDb,
                                 bool dragging = false);
    Q_INVOKABLE void setNodeMute(const QString& sceneId, const QString& nodeId, bool mute);
    Q_INVOKABLE void setNodeMonitor(const QString& sceneId, const QString& nodeId,
                                    bool monitor);
    Q_INVOKABLE void setNodeHidden(const QString& sceneId, const QString& nodeId, bool hidden);
    Q_INVOKABLE void setNodeLevelLocked(const QString& sceneId, const QString& nodeId,
                                        bool locked);
    Q_INVOKABLE void routeNode(const QString& sceneId, const QString& nodeId,
                               const QString& busId);

    // ---- decks and outputs (the controls panel) ---------------------------------------------
    //
    // Nothing here starts a broadcast. The render process draws the program preview and does
    // not encode or send, and this panel does not start it. What these edit is the document --
    // which scene is on program, which is staged on preview, what each output would encode and
    // where it would send it. That is the state a later encode will read.

    /// Transitions this build can name, as {id, title}. One of them is a cut, which is the only
    /// one that needs no renderer to be honest about.
    Q_PROPERTY(QVariantList transitions READ transitions NOTIFY changed)
    [[nodiscard]] QVariantList transitions() const;
    Q_INVOKABLE QVariantMap stingerSettings(const QString& deckId) const;
    Q_INVOKABLE void setStingerTiming(const QString& deckId, int cutMs, int durationMs);
    Q_INVOKABLE bool saveStinger(const QString& deckId, const QString& name, const QString& path,
                                 int cutMs, int durationMs, const QString& sourceId);
    Q_PROPERTY(Connectors* connectors READ connectors CONSTANT)
    Connectors* connectors() { return &connectors_; }
    Q_INVOKABLE void setOutputService(const QString& id, const QString& service);
    Q_INVOKABLE void connectOutputAccount(const QString& id);
    Q_PROPERTY(QVariantList destinationKinds READ destinationKinds CONSTANT)
    [[nodiscard]] QVariantList destinationKinds() const;
    Q_PROPERTY(QVariantList codecs READ codecs CONSTANT)
    [[nodiscard]] QVariantList codecs() const;

    Q_INVOKABLE void setDeckProgram(const QString& deckId, const QString& sceneId);
    Q_INVOKABLE void setDeckPreview(const QString& deckId, const QString& sceneId);
    /// Put what is on preview onto program. The transition itself is the renderer's job; what
    /// this does is move the pointer, which is the part that lives in the document.
    Q_PROPERTY(QVariantMap mainDeck READ mainDeck NOTIFY changed)
    [[nodiscard]] QVariantMap mainDeck() const;
    Q_PROPERTY(QVariantList quickTransitions READ quickTransitions NOTIFY changed)
    [[nodiscard]] QVariantList quickTransitions() const;
    Q_INVOKABLE void setQuickTransition(int index, const QString& transition, int durationMs);
    Q_INVOKABLE void removeQuickTransition(int index);
    Q_INVOKABLE void triggerQuickTransition(int index);
    Q_INVOKABLE void takeDeck(const QString& deckId);
    Q_INVOKABLE void setDeckStudioMode(const QString& deckId, bool studio);
    Q_INVOKABLE void setDeckTransition(const QString& deckId, const QString& transition,
                                       int durationMs);

    /// How many distinct encodes the project asks of the GPU. Not the number of outputs:
    /// destinations sharing one encode cost one (ADR-0029).
    Q_INVOKABLE [[nodiscard]] int encodeCount() const;
    Q_INVOKABLE void addOutput(const QString& name);
    Q_INVOKABLE void removeOutput(const QString& id);
    Q_INVOKABLE void renameOutput(const QString& id, const QString& name);
    Q_INVOKABLE void setOutputEnabled(const QString& id, bool enabled);
    Q_INVOKABLE void setReplaySettings(const QString& id, int seconds, int memoryMB);
    Q_INVOKABLE QString replayOutputId() const;
    Q_INVOKABLE QString replayPath() const;
    Q_INVOKABLE void setOutputBitrate(const QString& id, int kbps);
    Q_INVOKABLE void setOutputEncoder(const QString& id, const QString& codec,
                                      const QString& implementation);
    /// Destination kind and URL. Keys go to Secret Service, never the project (ADR-0049).
    Q_INVOKABLE void setOutputDestination(const QString& id, const QString& kind,
                                          const QString& url);
    Q_INVOKABLE QVariantList encodeChoices() const;
    Q_INVOKABLE QVariantMap outputEncoderSettings(const QString& id) const;
    Q_INVOKABLE void setOutputEncodingOptions(const QString& id, const QString& rateControl,
                                             const QString& preset, int keyframeMs);
    Q_INVOKABLE void makeOutputEncodeIndependent(const QString& id);
    Q_INVOKABLE void setOutputEncoderExtra(const QString& id, const QString& key,
                                           const QVariant& value);
    Q_INVOKABLE void saveOutputKey(const QString& id, const QString& key);
    Q_INVOKABLE void setOutputCredentialRef(const QString& id, const QString& ref);
    /// Point an output at an encode that already exists -- the Aitum case: encode 1080p once,
    /// send it to Twitch and Kick (ADR-0029). The outputs manager window is where this lives.
    Q_INVOKABLE void setOutputEncode(const QString& outputId, const QString& encodeId);
    /// A new destination hung straight off an existing encode, which is the same reuse said
    /// the other way round.
    Q_INVOKABLE void addOutputOnEncode(const QString& encodeId, const QString& name);
    /// A new encode of the current canvas, with one unconfigured destination.
    Q_INVOKABLE void addEncode();
    Q_INVOKABLE void initializeEncodeGraph();
    Q_INVOKABLE void disconnectGraphPort(const QString& kind, const QString& id,
                                         const QString& port);
    Q_INVOKABLE bool connectGraphPorts(const QString& sourceKind, const QString& sourceId,
                                       const QString& targetKind, const QString& targetId,
                                       const QString& targetPort);
    Q_INVOKABLE QString recordingEncodeId() const;
    Q_INVOKABLE QVariantMap streamingTarget() const;
    Q_INVOKABLE void removeEncode(const QString& id);
    Q_INVOKABLE QVariantMap encodeSettings(const QString& id) const;
    Q_INVOKABLE void setEncodeBitrate(const QString& id, int kbps);
    Q_INVOKABLE void setEncodeEncoder(const QString& id, const QString& codec, const QString& implementation);
    Q_INVOKABLE void setEncodeOptions(const QString& id, const QString& rateControl,
                                      const QString& preset, int keyframeMs);
    Q_INVOKABLE void setEncodeExtra(const QString& id, const QString& key, const QVariant& value);
    static void configurePresets(std::filesystem::path path);
    Q_PROPERTY(QStringList encoderPresetNames READ encoderPresetNames NOTIFY encoderPresetsChanged)
    QStringList encoderPresetNames() const;
    Q_INVOKABLE bool saveEncoderPreset(const QString& encodeId, const QString& name, bool overwrite = false);
    Q_INVOKABLE bool applyEncoderPreset(const QString& encodeId, const QString& name);
    Q_INVOKABLE bool removeEncoderPreset(const QString& name);
    static ldoc::Encode defaultEncode(const ldoc::Canvas& canvas);

    /// Point an encode at a different canvas. Without this a second canvas can never be
    /// streamed: `addEncode` uses whichever canvas is current and nothing could change it
    /// afterwards (ADR-0035).
    Q_INVOKABLE void setEncodeCanvas(const QString& id, const QString& canvasId);
    Q_INVOKABLE void renameEncode(const QString& id, const QString& name);
    Q_INVOKABLE void setEncodeRescale(const QString& id, int width, int height,
                                     const QString& filter = {});

    // ---- audio encodes ----------------------------------------------------------------------
    //
    // The audio half of ADR-0029, and the same shape: one bitstream, named, that any number of
    // destinations receive. These were four string keys inside EncoderConfig::extra until
    // schema 4, which could not say that several destinations share one AAC stream.

    /// A new audio encode of the program bus, named for what it is.
    Q_INVOKABLE void addAudioEncode();
    Q_INVOKABLE void removeAudioEncode(const QString& id);
    Q_INVOKABLE void renameAudioEncode(const QString& id, const QString& name);
    Q_INVOKABLE void setAudioEncodeBus(const QString& id, const QString& busId);
    Q_INVOKABLE void setAudioEncodeCodec(const QString& id, const QString& codec);
    Q_INVOKABLE void setAudioEncodeBitrate(const QString& id, int kbps);
    Q_INVOKABLE [[nodiscard]] QVariantMap audioEncodeSettings(const QString& id) const;
    /// {id, name} for every audio encode, for a dropdown to choose between them.
    Q_INVOKABLE [[nodiscard]] QVariantList audioEncodeChoices() const;
    /// Which bus each audio encode could read: the program and track buses, by {id, name}.
    Q_INVOKABLE [[nodiscard]] QVariantList audioBusChoices() const;
    /// Point a destination at an audio bitstream, or at none when the id is empty. None is a
    /// destination that goes out silent, which validate() warns about rather than refusing.
    Q_INVOKABLE void setOutputAudioEncode(const QString& outputId, const QString& audioEncodeId);
    /// Which audio encode a destination names, or empty for none. A call rather than a model
    /// role lookup, because QML reading a role by its integer offset breaks the moment a role
    /// is inserted above it.
    Q_INVOKABLE [[nodiscard]] QString outputAudioEncode(const QString& outputId) const;
    /// Where a recording of this encode would be written: beside the project when there is
    /// one, in the home directory when there is not, stamped so a second take does not
    /// overwrite the first. Empty when the encode is not in the project.
    Q_INVOKABLE void setRecordingDirectory(const QString& outputId, const QString& path);
    Q_INVOKABLE [[nodiscard]] QString recordingDirectory() const;
    Q_INVOKABLE [[nodiscard]] QString recordingPathFor(const QString& encodeId) const;
    /// Which canvas an encode reads. For the graph, which draws that as an edge.
    Q_INVOKABLE [[nodiscard]] QString encodeCanvasOf(const QString& encodeId) const;
    /// {id, audioEncode} for every destination that carries audio. The graph's other edge;
    /// destinations with no audio are simply absent rather than listed with an empty field.
    Q_INVOKABLE [[nodiscard]] QVariantList outputsWithAudio() const;

    /// Move a node on the Encode workspace's graph. `kind` is "encode", "audio-encode" or
    /// "output".
    ///
    /// `dragging` is true while the pointer is still down, so a whole drag becomes one undo
    /// step and lands back where it started -- the same gesture handling as setBusGain and
    /// setNodeFrame. The caller must finish with endGesture().
    Q_INVOKABLE void setGraphPosition(const QString& kind, const QString& id, double x, double y,
                                      bool dragging = false);

    // ---- the nodes of the current scene ------------------------------------------------------
    //
    // "Node" rather than "source" in these names because they act on whatever is selected,
    // groups included, and match the `SetNode*` commands underneath. Only `addSourceNode` is
    // about one kind: a source node bound to nothing yet, for `bindSelectedNode` to fill.

    Q_INVOKABLE void addSourceNode(const QString& name);
    Q_INVOKABLE void addGroup(const QString& name);
    Q_INVOKABLE void removeNode(const QString& id);
    Q_INVOKABLE void renameNode(const QString& id, const QString& name);
    Q_INVOKABLE void setNodeVisible(const QString& id, bool visible);
    Q_INVOKABLE void setNodeLocked(const QString& id, bool locked);
    /// Negative moves towards the front of the list, which is towards the top of the stack.
    Q_INVOKABLE void moveNode(const QString& id, int delta);

    // ---- the selected node's properties ------------------------------------------------------

    Q_INVOKABLE void setNodePosition(double x, double y);
    /// Position and scale together. `dragging` makes the whole gesture one undo step; the caller
    /// finishes with `endGesture()`. A canvas handle changes scale, not the layout box: with the
    /// default fit the box is not the rectangle on screen.
    Q_INVOKABLE void setNodeFrame(double x, double y, double scaleX, double scaleY,
                                  bool dragging = false);
    /// The drawn rectangle, in canvas pixels. Rotation stays, and so does the sign of the
    /// scale. Position and scale are solved so the picture lands on this rectangle. `dragging`
    /// is the same one-undo gesture as `setNodeFrame`.
    Q_INVOKABLE void setDrawnRect(double x, double y, double width, double height,
                                  bool dragging = false);
    Q_INVOKABLE void resetTransform();
    Q_INVOKABLE void rotateBy(double degrees);
    Q_INVOKABLE void flipHorizontal();
    Q_INVOKABLE void flipVertical();
    /// `stretch` fills the canvas on both axes. Otherwise the picture keeps its aspect and sits
    /// in the middle.
    Q_INVOKABLE void fitToCanvas(bool stretch);
    Q_INVOKABLE void centerOnCanvas(bool horizontal, bool vertical);
    Q_INVOKABLE void copyTransform();
    Q_INVOKABLE void pasteTransform();
    Q_INVOKABLE [[nodiscard]] bool hasCopiedTransform() const;
    /// `front` is the top of the stack, index 0. One undo either way.
    Q_INVOKABLE void moveSelectedToEnd(bool front);
    /// Ask the sources list to rename the selection. The canvas has no text field of its own.
    Q_INVOKABLE void requestRename();
    Q_INVOKABLE void setNodeScale(double x, double y);
    Q_INVOKABLE void setNodeRotation(double degrees);
    Q_INVOKABLE void setNodeOpacity(double opacity);
    Q_INVOKABLE void setNodeBlendMode(const QString& mode);
    /// How the content fills the node's box, which is a different thing from where the box sits.
    Q_INVOKABLE void setNodeFit(const QString& fit);
    Q_INVOKABLE void setNodeBoxSize(double w, double h);
    Q_INVOKABLE void setNodeCrop(double left, double top, double right, double bottom);
    Q_INVOKABLE void setNodeAlignment(double x, double y);

    Q_PROPERTY(QStringList fitModes READ fitModes CONSTANT)
    [[nodiscard]] QStringList fitModes() const;

    // ---- history and persistence -------------------------------------------------------------

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void save();
    static void configureCollections(std::filesystem::path root, std::filesystem::path catalog);
    static std::filesystem::path lastCollection();
    QVariantList collections() const { return collection_items_; }
    QString collectionReport() const { return collection_report_; }
    Q_INVOKABLE bool createCollection(const QString& name, bool duplicate = false);
    Q_INVOKABLE bool renameCollection(const QString& name);
    Q_INVOKABLE bool openCollection(const QString& path);
    Q_INVOKABLE bool removeCollection();
    Q_INVOKABLE bool importCollection(const QString& path, bool obs, const QString& name,
                                      int width, int height, int fpsNum, int fpsDen);
    Q_INVOKABLE bool exportCollection(const QString& path, bool obs);


    // ---- for other C++ bridges, deliberately not Q_INVOKABLE -------------------------------
    //
    // Reachable from C++ so that Shortcuts can resolve an action against the real project and
    // hand back the command it produced; not reachable from QML, which must keep going through
    // the typed invokables above. A const reference either way: nothing outside the session
    // holds a mutable Project.

    [[nodiscard]] const ldoc::Project& projectForBindings() const;
    void runCommand(const ldoc::Command& command);
    std::expected<std::string, std::string> remoteCommand(const ldoc::Command& command);
    /// Rebuilds the row models without the project having changed. For `AssetCache`: a prepared
    /// file appearing or going stale changes what a Sources row's indicator should say, and
    /// nothing in the document moved to make the usual rebuild happen.
    void refreshRows();

signals:
  void filtersRequested(const QString& owner);
  void collectionsChanged();
  void encoderPresetsChanged();
  void changed();
  void selectionChanged();
  /// The shell is now working on a different canvas.
  void canvasChanged();
  /// Something to say in the status bar: a refused command, a failed save, a recovered journal.
  void notice(const QString& text);
  void outputKeySaveFinished(const QString& outputId, bool saved);
  /// The sources list should start renaming this node.
  void renameRequested(const QString& nodeId);

private:
    void rebuild();
    static ldoc::Project studioProject(std::string name);
    void initCollections();
    bool rememberCollection();
    bool saveCurrentCollection();
    void installSession(std::unique_ptr<lsession::Session> session);
    QVariantList collection_items_;
    QString collection_report_;
    QString collection_catalog_error_;

    void run(const ldoc::Command& command);
    [[nodiscard]] const ldoc::Scene* current_scene() const;
    [[nodiscard]] const ldoc::Scene* scene_of_canvas(const QString& canvasId) const;
    /// `canvasId`, or the first canvas's id when it is empty.
    [[nodiscard]] QString canvas_key(const QString& canvasId) const;
    [[nodiscard]] const ldoc::Node* current_node() const;
    /// The selection, when a transform edit is allowed. A locked node is refused out loud.
    bool prepare_transform(const ldoc::Scene*& scene, const ldoc::Node*& node);
    QVariantMap placed_rect(const ldoc::Scene& scene, const ldoc::Canvas& canvas,
                            const ldoc::Node& node) const;
    /// The canvas the shell is currently working against. One canvas until Phase 2 gives the UI
    /// a way to choose; taking the first is honest rather than pretending otherwise.
    [[nodiscard]] const ldoc::Canvas* current_canvas() const;

    /// Read-modify-write helpers. SetBus and SetNodeAudio carry a whole value rather than a
    /// delta, so every edit has to start from the current one; doing it in one place is what
    /// stops a setter that forgot a field from quietly resetting it.
    void edit_bus(const QString& id, const std::function<void(ldoc::Bus&)>& change,
                  std::string_view gesture = {});
    void edit_selected_audio(const std::function<void(ldoc::AudioRouting&)>& change);
    /// The node-keyed form the selection one is written in terms of. `gesture` collapses a
    /// fader drag into one undo step, exactly as the bus faders do.
    void edit_node_audio(const QString& sceneId, const QString& nodeId,
                         const std::function<void(ldoc::AudioRouting&)>& change,
                         std::string_view gesture = {});
    /// One entry for the add dialog's grid, built the same way wherever it is listed from.
    [[nodiscard]] QVariantMap source_tile(const ldoc::Source& src) const;

    void edit_deck(const QString& id, const std::function<void(ldoc::Deck&)>& change);
    void edit_output(const QString& id, const std::function<void(ldoc::Output&)>& change);
    void edit_encode(const QString& id, const std::function<void(ldoc::Encode&)>& change);
    void edit_audio_encode(const QString& id,
                           const std::function<void(ldoc::AudioEncode&)>& change,
                           std::string_view gesture = {});
    static std::optional<ldoc::ParamValue> encoderExtraValue(
        const QString& key, const QVariant& value);
    void edit_canvas(const QString& id, const std::function<void(ldoc::Canvas&)>& change);

    Connectors connectors_{this};
    std::unique_ptr<lsession::Session> session_;
    /// One per canvas, keyed by id, created on demand and owned by this object. Kept after a
    /// canvas goes rather than deleted: a panel can outlive its canvas by a frame, and an
    /// empty model is a better thing for it to be bound to than a dangling pointer.
    std::map<QString, SceneModel*> scenes_by_canvas_;
    std::map<QString, NodeModel*> nodes_by_canvas_;
    SceneModel favourites_;
    SourceModel sources_;
    /// What was copied, if anything: exactly one of these holds a value.
    /// What was copied. A list because the selection is one: copying three and pasting gives
    /// three. One copied node is a list of one rather than a separate case.
    std::vector<ldoc::Node> copied_nodes_;
    std::optional<ldoc::Scene> copied_scene_;
    /// A transform copied on its own, separate from a copied node (the menu's Copy Transform).
    std::optional<ldoc::Transform> copied_transform_;

    BusModel buses_;
    MixerStripModel mixer_strips_;
    DeckModel decks_;
    OutputModel outputs_;
    CanvasModel canvases_;
    EncodeModel encodes_;
    AudioEncodeModel audio_encodes_;

    /// Per canvas: each canvas's panels keep their own place in their own scene list.
    std::map<QString, QString> selected_scene_;
    QString selected_node_;
    /// The rest of the selection, never including `selected_node_`. Kept apart so that every
    /// existing reader of `selectedNode` keeps working unchanged: one click is still one node
    /// (ADR-0037).
    QStringList also_selected_;
    /// Where a shift-click measures from: the last node picked without shift.
    QString selection_anchor_;
    // Session-only presentation state; source visibility and saved scene data stay intact.
    QStringList collapsed_groups_;
    /// Empty means "the first canvas". Resolved on every read rather than stored as an id, so
    /// that removing the current canvas cannot leave the shell pointing at nothing.
    QString current_canvas_;
    QTimer autosave_;
    QTimer countdown_actions_;
};

} // namespace libertas::ui
