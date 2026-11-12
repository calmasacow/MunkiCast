// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rowmodel.hpp"

#include <QString>
#include <QStringList>

#include <vector>

namespace munkicast::ui {

struct SceneRow {
    QString id;
    QString name;
    int nodeCount = 0;
    /// On air on at least one deck.
    bool isProgram = false;
    bool isPreview = false;
    /// The safety net for at least one canvas (ADR-0025). Marked in the list, because losing it
    /// unnoticed is that ADR's own failure arriving by another route.
    bool isEmergency = false;
    /// Marked by the operator; the favourites panel is this filtered.
    bool isFavourite = false;
    /// How many Main scenes bring this scene up (ADR-0027). Zero on every Main scene, because
    /// linking runs from Main and never to it -- which is also why Main's rows draw no chain.
    int linkCount = 0;
    /// This row's canvas is not the first one, so it can be linked. Carried on the row rather
    /// than read from the panel's canvas, so the favourites panel -- which mixes canvases --
    /// gets the right answer per row.
    bool canLink = false;

    friend bool operator==(const SceneRow&, const SceneRow&) = default;
};

class SceneModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        NodeCountRole,
        IsProgramRole,
        IsPreviewRole,
        IsEmergencyRole,
        IsFavouriteRole,
        LinkCountRole,
        CanLinkRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<SceneRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<SceneRow>& rows() const { return rows_; }

private:
    std::vector<SceneRow> rows_;
};

struct NodeRow {
    QString id;
    QString name;
    /// The screen's word for the kind, which for a source node is not yet the file's.
    QString kind;
    /// What the source node is bound to, for the second line of the row. Empty when nothing is.
    QString sourceName;
    QString sourceType;
    /// The id of that source, so a row can open its settings without a lookup by name --
    /// names are not unique and two cameras called "Cam" are a normal thing to have.
    QString sourceId;
    /// Depth in the scene tree; the Sources panel is a flattened tree with indentation.
    int depth = 0;
    bool visible = true;
    bool locked = false;
    bool isGroup = false;
    bool expanded = true;
    int effectCount = 0;
    /// This node's source is a type that can have prepared local copies, so the row draws a
    /// CACHE box. Types that are not file-backed have no box at all rather than a grey one.
    bool cacheable = false;
    /// Its "Use caching" setting. False means the box reads as off even when the cache is on.
    bool cacheUsed = false;
    /// "off", "ok", "stale" or "error" -- the colour of the box, and which sentences the state
    /// dialog shows. Computed from `AssetCache`, which has already stat-ed off-thread.
    QString cacheState;

    friend bool operator==(const NodeRow&, const NodeRow&) = default;
};

class NodeModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        SourceNameRole,
        SourceTypeRole,
        SourceIdRole,
        DepthRole,
        VisibleRole,
        LockedRole,
        IsGroupRole,
        ExpandedRole,
        EffectCountRole,
        CacheableRole,
        CacheUsedRole,
        CacheStateRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<NodeRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<NodeRow>& rows() const { return rows_; }

private:
    std::vector<NodeRow> rows_;
};

struct SourceRow {
    QString id;
    QString name;
    QString type;
    /// How many source nodes across the whole project bind to it. Sources are project-level, so
    /// this is the number that says whether removing one is safe.
    int useCount = 0;
    /// Safe to put in an emergency scene (ADR-0025).
    bool selfContained = false;

    friend bool operator==(const SourceRow&, const SourceRow&) = default;
};

class SourceModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        TypeRole,
        UseCountRole,
        SelfContainedRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<SourceRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<SourceRow>& rows() const { return rows_; }

private:
    std::vector<SourceRow> rows_;
};

struct BusRow {
    QString id;
    QString name;
    QString kind;
    double gainDb = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool monitor = false;
    bool hidden = false;
    bool levelLocked = false;
    int channels = 2;
    /// How many source nodes, across every scene, route here. The mixer shows it because a bus with
    /// nothing feeding it looks identical to a working one until the show starts.
    int feedCount = 0;
    int sendCount = 0;
    /// This bus is where the selected node's audio goes, so the strip can say so.
    bool carriesSelection = false;

    friend bool operator==(const BusRow&, const BusRow&) = default;
};

class BusModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        KindRole,
        GainDbRole,
        PanRole,
        MuteRole,
        MonitorRole,
        HiddenRole,
        LevelLockedRole,
        ChannelsRole,
        FeedCountRole,
        SendCountRole,
        CarriesSelectionRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<BusRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<BusRow>& rows() const { return rows_; }

private:
    std::vector<BusRow> rows_;
};

/// One source node in the scene on screen, with audio of its own.
///
/// Not a bus: this is the strip an operator from OBS is looking for, the one per microphone.
/// Keyed by node rather than by source, because the same Source placed twice in a scene is two
/// placements with two faders -- the routing lives on the node (ADR-0027).
struct MixerStripRow {
    QString id;
    QString nodeId;
    QString sceneId;
    QString name;
    /// Where it goes, and the empty string when it goes nowhere. Unrouted is not muted: one is
    /// silent by omission and the other on purpose, and a strip that showed them alike would
    /// hide the commoner mistake.
    QString busId;
    QString busName;
    double gainDb = 0.0;
    bool mute = false;
    /// Heard locally; never changes the program mix (ADR-0031).
    bool monitor = false;
    /// Hidden from this panel only -- still routed, still on air (ADR-0032).
    bool hidden = false;
    /// The fader will not move. Mute still works.
    bool levelLocked = false;
    bool selected = false;

    friend bool operator==(const MixerStripRow&, const MixerStripRow&) = default;
};

class MixerStripModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NodeIdRole,
        SceneIdRole,
        NameRole,
        BusIdRole,
        BusNameRole,
        GainDbRole,
        MuteRole,
        MonitorRole,
        HiddenRole,
        LevelLockedRole,
        SelectedRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<MixerStripRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<MixerStripRow>& rows() const { return rows_; }

private:
    std::vector<MixerStripRow> rows_;
};

struct DeckRow {
    QString id;
    QString name;
    QString canvasName;
    /// Names rather than ids, because what the controls panel shows is what is on air.
    QString programScene;
    QString previewScene;
    QString programSceneId;
    QString previewSceneId;
    bool studioMode = false;
    QString transition;
    int transitionMs = 300;
    /// The canvas has a safety net designated (ADR-0025). A deck whose canvas has none is a
    /// pre-flight question, and this is where an operator would see it.
    bool hasEmergencyScene = false;

    friend bool operator==(const DeckRow&, const DeckRow&) = default;
};

class DeckModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CanvasNameRole,
        ProgramSceneRole,
        PreviewSceneRole,
        ProgramSceneIdRole,
        PreviewSceneIdRole,
        StudioModeRole,
        TransitionRole,
        TransitionMsRole,
        HasEmergencySceneRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<DeckRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<DeckRow>& rows() const { return rows_; }

private:
    std::vector<DeckRow> rows_;
};

struct CanvasRow {
    QString id;
    QString name;
    /// Not "width"/"height": QQuickItem declares both FINAL, so a delegate that is an Item --
    /// which every delegate here is -- cannot take roles by those names at all. Renaming the
    /// role is the fix; renaming the delegate's base class is not.
    int pixelWidth = 0;
    int pixelHeight = 0;
    /// Already divided out, because 60000/1001 is not what an operator calls that rate.
    double fps = 0.0;
    QString colorSpace;
    /// The first canvas. Main cannot be removed, renamed, or displaced from index 0 -- the
    /// command layer refuses all three (ADR-0027), and the panel greys the buttons to match
    /// rather than letting the operator press something that will only be refused.
    bool isMain = false;
    /// How many scenes belong to this canvas. A canvas still holding scenes cannot be removed.
    int sceneCount = 0;
    /// This canvas has a safety net designated (ADR-0025).
    bool hasEmergencyScene = false;

    friend bool operator==(const CanvasRow&, const CanvasRow&) = default;
};

class CanvasModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PixelWidthRole,
        PixelHeightRole,
        FpsRole,
        ColorSpaceRole,
        IsMainRole,
        SceneCountRole,
        HasEmergencySceneRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<CanvasRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<CanvasRow>& rows() const { return rows_; }

private:
    std::vector<CanvasRow> rows_;
};

struct EncodeRow {
    QString id;
    QString name;
    /// Which canvas this encode reads. The id as well as the name, because the manager lets it
    /// be changed and a dropdown needs something to compare against (ADR-0035).
    QString canvasId;
    QString canvasName;
    /// What leaves this encoder: the rescale when it has one, the canvas size when it does not.
    QString resolution;
    QString codec;
    QString implementation;
    int bitrateKbps = 0;
    QString preset;
    /// The destinations fed by this one encode, by name. This is the number the inspector counts
    /// against the GPU made visible: one encode, several rows under it.
    QStringList destinations;
    /// Where this node sits on the Encode workspace's graph.
    double positionX = 0;
    double positionY = 0;

    friend bool operator==(const EncodeRow&, const EncodeRow&) = default;
};

/// One audio bitstream, with the bus it encodes and the destinations that receive it.
///
/// Separate from EncodeRow rather than a flag on it: the graph draws them as different node
/// kinds fed from different columns, and a single row type carrying "which half am I" would
/// have to be unpacked at every use.
struct AudioEncodeRow {
    QString id;
    QString name;
    QString busId;
    QString busName;
    QString codec;
    int bitrateKbps = 0;
    /// The destinations fed by this one audio encode, by name.
    QStringList destinations;
    double positionX = 0;
    double positionY = 0;

    friend bool operator==(const AudioEncodeRow&, const AudioEncodeRow&) = default;
};

class AudioEncodeModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        BusIdRole,
        BusNameRole,
        CodecRole,
        BitrateRole,
        DestinationsRole,
        PositionXRole,
        PositionYRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<AudioEncodeRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<AudioEncodeRow>& rows() const { return rows_; }

private:
    std::vector<AudioEncodeRow> rows_;
};

class EncodeModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CanvasIdRole,
        CanvasNameRole,
        ResolutionRole,
        CodecRole,
        ImplementationRole,
        BitrateRole,
        PresetRole,
        DestinationsRole,
        PositionXRole,
        PositionYRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<EncodeRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<EncodeRow>& rows() const { return rows_; }

private:
    std::vector<EncodeRow> rows_;
};

struct OutputRow {
    QString id;
    QString encodeId;
    QString name;
    QString canvasName;
    /// The encode's own name. Two destinations fed by one encode show the same name here,
    /// which is the only thing on the row that tells them apart from two encodes that merely
    /// happen to be configured alike (ADR-0029).
    QString encodeName;
    /// What actually leaves the encoder: the encode's rescale when it has one, the canvas
    /// size when it does not. The canvas name alone says the wrong resolution for a
    /// downscaled encode, which is the common case -- 1080p to Twitch off a 1440p canvas.
    QString resolution;
    /// The *other* outputs fed by this same encode, by name. Empty for an unshared encode.
    QStringList sharedWith;
    QString codec;
    QString implementation;
    int bitrateKbps = 0;
    QString destinationKind;
    /// `qrc:` icon for the destination service; empty means the text badge.
    QString serviceIcon;
    QString url;
    /// The *name* of a credential. There is no field anywhere in this program that holds the
    /// credential itself (ADR-0022), and this row is one of the places that would be tempting.
    QString credentialRef;
    bool enabled = false;
    /// Enabled and pointed at nothing, which validate() calls an error.
    bool misconfigured = false;
    /// Which audio bitstream this destination receives. Empty means none, which validate()
    /// warns about rather than refusing: a silent file capture is a thing someone can mean.
    QString audioEncodeId;
    QString audioEncodeName;
    double positionX = 0;
    double positionY = 0;

    friend bool operator==(const OutputRow&, const OutputRow&) = default;
};

class OutputModel : public RowModelBase {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        CanvasNameRole,
        EncodeNameRole,
        ResolutionRole,
        SharedWithRole,
        CodecRole,
        ImplementationRole,
        BitrateRole,
        DestinationKindRole,
        UrlRole,
        CredentialRefRole,
        EnabledRole,
        MisconfiguredRole,
        EncodeIdRole,
        AudioEncodeIdRole,
        AudioEncodeNameRole,
        PositionXRole,
        PositionYRole,
        ServiceIconRole,
    };

    using RowModelBase::RowModelBase;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void set_rows(std::vector<OutputRow> rows) { sync(rows_, std::move(rows)); }
    [[nodiscard]] const std::vector<OutputRow>& rows() const { return rows_; }

private:
    std::vector<OutputRow> rows_;
};

} // namespace munkicast::ui
