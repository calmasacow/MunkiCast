// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "documentmodels.hpp"

namespace munkicast::ui {

int SceneModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SceneModel::data(const QModelIndex& index, int role) const {
    const SceneRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:          return r->id;
    case NameRole:        return r->name;
    case NodeCountRole:   return r->nodeCount;
    case IsProgramRole:   return r->isProgram;
    case IsPreviewRole:   return r->isPreview;
    case IsEmergencyRole: return r->isEmergency;
    case IsFavouriteRole: return r->isFavourite;
    case LinkCountRole:   return r->linkCount;
    case CanLinkRole:     return r->canLink;
    default:              return {};
    }
}

QHash<int, QByteArray> SceneModel::roleNames() const {
    return {
        {IdRole, "sceneId"},          {NameRole, "name"},
        {NodeCountRole, "nodeCount"}, {IsProgramRole, "isProgram"},
        {IsPreviewRole, "isPreview"}, {IsEmergencyRole, "isEmergency"},
        {IsFavouriteRole, "isFavourite"},
        {LinkCountRole, "linkCount"}, {CanLinkRole, "canLink"},
    };
}

int NodeModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant NodeModel::data(const QModelIndex& index, int role) const {
    const NodeRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:          return r->id;
    case NameRole:        return r->name;
    case KindRole:        return r->kind;
    case SourceNameRole:  return r->sourceName;
    case SourceTypeRole:  return r->sourceType;
    case SourceIdRole:    return r->sourceId;
    case DepthRole:       return r->depth;
    case VisibleRole:     return r->visible;
    case LockedRole:      return r->locked;
    case IsGroupRole:     return r->isGroup;
    case ExpandedRole:    return r->expanded;
    case EffectCountRole: return r->effectCount;
    case CacheableRole:   return r->cacheable;
    case CacheUsedRole:   return r->cacheUsed;
    case CacheStateRole:  return r->cacheState;
    default:              return {};
    }
}

QHash<int, QByteArray> NodeModel::roleNames() const {
    return {
        {IdRole, "nodeId"},      {NameRole, "name"},
        {KindRole, "kind"},      {SourceNameRole, "sourceName"},
        {SourceTypeRole, "sourceType"},
        {SourceIdRole, "sourceId"},
        {DepthRole, "depth"},    {VisibleRole, "nodeVisible"},
        {LockedRole, "locked"},  {IsGroupRole, "isGroup"}, {ExpandedRole, "expanded"},
        {EffectCountRole, "effectCount"},
        {CacheableRole, "cacheable"},
        {CacheUsedRole, "cacheUsed"},
        {CacheStateRole, "cacheState"},
    };
}

int SourceModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant SourceModel::data(const QModelIndex& index, int role) const {
    const SourceRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:            return r->id;
    case NameRole:          return r->name;
    case TypeRole:          return r->type;
    case UseCountRole:      return r->useCount;
    case SelfContainedRole: return r->selfContained;
    default:                return {};
    }
}

QHash<int, QByteArray> SourceModel::roleNames() const {
    return {
        {IdRole, "sourceId"},  {NameRole, "name"},
        {TypeRole, "type"},    {UseCountRole, "useCount"},
        {SelfContainedRole, "selfContained"},
    };
}

int BusModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant BusModel::data(const QModelIndex& index, int role) const {
    const BusRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:               return r->id;
    case NameRole:             return r->name;
    case KindRole:             return r->kind;
    case GainDbRole:           return r->gainDb;
    case PanRole:              return r->pan;
    case MuteRole:             return r->mute;
    case MonitorRole:          return r->monitor;
    case HiddenRole:           return r->hidden;
    case LevelLockedRole:      return r->levelLocked;
    case ChannelsRole:         return r->channels;
    case FeedCountRole:        return r->feedCount;
    case SendCountRole:        return r->sendCount;
    case CarriesSelectionRole: return r->carriesSelection;
    default:                   return {};
    }
}

QHash<int, QByteArray> BusModel::roleNames() const {
    return {
        {IdRole, "busId"},           {NameRole, "name"},
        {KindRole, "kind"},          {GainDbRole, "gainDb"},
        {PanRole, "pan"},            {MuteRole, "mute"},
        {MonitorRole, "monitor"},    {HiddenRole, "hidden"},
        {LevelLockedRole, "levelLocked"}, {ChannelsRole, "channels"},
        {FeedCountRole, "feedCount"}, {SendCountRole, "sendCount"},
        {CarriesSelectionRole, "carriesSelection"},
    };
}

int MixerStripModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant MixerStripModel::data(const QModelIndex& index, int role) const {
    const MixerStripRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:       return r->id;
    case NodeIdRole:   return r->nodeId;
    case SceneIdRole:  return r->sceneId;
    case NameRole:     return r->name;
    case BusIdRole:    return r->busId;
    case BusNameRole:  return r->busName;
    case GainDbRole:   return r->gainDb;
    case MuteRole:     return r->mute;
    case MonitorRole:  return r->monitor;
    case HiddenRole:   return r->hidden;
    case LevelLockedRole: return r->levelLocked;
    case SelectedRole: return r->selected;
    default:           return {};
    }
}

QHash<int, QByteArray> MixerStripModel::roleNames() const {
    return {
        {IdRole, "stripId"},      {NodeIdRole, "nodeId"},
        {SceneIdRole, "sceneId"}, {NameRole, "name"},
        {BusIdRole, "busId"},     {BusNameRole, "busName"},
        {GainDbRole, "gainDb"},   {MuteRole, "mute"},
        {MonitorRole, "monitor"}, {HiddenRole, "hidden"},
        {LevelLockedRole, "levelLocked"}, {SelectedRole, "selected"},
    };
}

int DeckModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DeckModel::data(const QModelIndex& index, int role) const {
    const DeckRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:                return r->id;
    case NameRole:              return r->name;
    case CanvasNameRole:        return r->canvasName;
    case ProgramSceneRole:      return r->programScene;
    case PreviewSceneRole:      return r->previewScene;
    case ProgramSceneIdRole:    return r->programSceneId;
    case PreviewSceneIdRole:    return r->previewSceneId;
    case StudioModeRole:        return r->studioMode;
    case TransitionRole:        return r->transition;
    case TransitionMsRole:      return r->transitionMs;
    case HasEmergencySceneRole: return r->hasEmergencyScene;
    default:                    return {};
    }
}

QHash<int, QByteArray> DeckModel::roleNames() const {
    return {
        {IdRole, "deckId"},
        {NameRole, "name"},
        {CanvasNameRole, "canvasName"},
        {ProgramSceneRole, "programScene"},
        {PreviewSceneRole, "previewScene"},
        {ProgramSceneIdRole, "programSceneId"},
        {PreviewSceneIdRole, "previewSceneId"},
        {StudioModeRole, "studioMode"},
        {TransitionRole, "transition"},
        {TransitionMsRole, "transitionMs"},
        {HasEmergencySceneRole, "hasEmergencyScene"},
    };
}

int OutputModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant OutputModel::data(const QModelIndex& index, int role) const {
    const OutputRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:              return r->id;
    case NameRole:            return r->name;
    case CanvasNameRole:      return r->canvasName;
    case EncodeNameRole:      return r->encodeName;
    case ResolutionRole:      return r->resolution;
    case SharedWithRole:      return r->sharedWith;
    case CodecRole:           return r->codec;
    case ImplementationRole:  return r->implementation;
    case BitrateRole:         return r->bitrateKbps;
    case DestinationKindRole: return r->destinationKind;
    case ServiceIconRole:     return r->serviceIcon;
    case UrlRole:             return r->url;
    case CredentialRefRole:   return r->credentialRef;
    case EnabledRole:         return r->enabled;
    case MisconfiguredRole:   return r->misconfigured;
    case EncodeIdRole:        return r->encodeId;
    case AudioEncodeIdRole:   return r->audioEncodeId;
    case AudioEncodeNameRole: return r->audioEncodeName;
    case PositionXRole:       return r->positionX;
    case PositionYRole:       return r->positionY;
    default:                  return {};
    }
}

QHash<int, QByteArray> OutputModel::roleNames() const {
    return {
        {IdRole, "outputId"},
        {NameRole, "name"},
        {CanvasNameRole, "canvasName"},
        {EncodeNameRole, "encodeName"},
        {ResolutionRole, "resolution"},
        {SharedWithRole, "sharedWith"},
        {CodecRole, "codec"},
        {ImplementationRole, "implementation"},
        {BitrateRole, "bitrateKbps"},
        {DestinationKindRole, "destinationKind"},
        {ServiceIconRole, "serviceIcon"},
        {UrlRole, "url"},
        {CredentialRefRole, "credentialRef"},
        // Not "enabled": that is QQuickItem's own property, and a delegate exposing a role
        // by that name would disable itself whenever the output was disabled.
        {EnabledRole, "isEnabled"},
        {MisconfiguredRole, "misconfigured"},
        {EncodeIdRole, "encodeId"},
        {AudioEncodeIdRole, "audioEncodeId"},
        {AudioEncodeNameRole, "audioEncodeName"},
        {PositionXRole, "positionX"},
        {PositionYRole, "positionY"},
    };
}

int CanvasModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant CanvasModel::data(const QModelIndex& index, int role) const {
    const CanvasRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:                return r->id;
    case NameRole:              return r->name;
    case PixelWidthRole:        return r->pixelWidth;
    case PixelHeightRole:       return r->pixelHeight;
    case FpsRole:               return r->fps;
    case ColorSpaceRole:        return r->colorSpace;
    case IsMainRole:            return r->isMain;
    case SceneCountRole:        return r->sceneCount;
    case HasEmergencySceneRole: return r->hasEmergencyScene;
    default:                    return {};
    }
}

QHash<int, QByteArray> CanvasModel::roleNames() const {
    return {
        {IdRole, "canvasId"},
        {NameRole, "name"},
        {PixelWidthRole, "pixelWidth"},
        {PixelHeightRole, "pixelHeight"},
        {FpsRole, "fps"},
        {ColorSpaceRole, "colorSpace"},
        {IsMainRole, "isMain"},
        {SceneCountRole, "sceneCount"},
        {HasEmergencySceneRole, "hasEmergencyScene"},
    };
}

int EncodeModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant EncodeModel::data(const QModelIndex& index, int role) const {
    const EncodeRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:             return r->id;
    case NameRole:           return r->name;
    case CanvasIdRole:       return r->canvasId;
    case CanvasNameRole:     return r->canvasName;
    case ResolutionRole:     return r->resolution;
    case CodecRole:          return r->codec;
    case ImplementationRole: return r->implementation;
    case BitrateRole:        return r->bitrateKbps;
    case PresetRole:         return r->preset;
    case DestinationsRole:   return r->destinations;
    case PositionXRole:      return r->positionX;
    case PositionYRole:      return r->positionY;
    default:                 return {};
    }
}

QHash<int, QByteArray> EncodeModel::roleNames() const {
    return {
        {IdRole, "encodeId"},
        {NameRole, "name"},
        {CanvasIdRole, "canvasId"},
        {CanvasNameRole, "canvasName"},
        {ResolutionRole, "resolution"},
        {CodecRole, "codec"},
        {ImplementationRole, "implementation"},
        {BitrateRole, "bitrateKbps"},
        {PresetRole, "preset"},
        {DestinationsRole, "destinations"},
        {PositionXRole, "positionX"},
        {PositionYRole, "positionY"},
    };
}

int AudioEncodeModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant AudioEncodeModel::data(const QModelIndex& index, int role) const {
    const AudioEncodeRow* r = at(rows_, index);
    if (!r)
        return {};
    switch (role) {
    case IdRole:           return r->id;
    case NameRole:         return r->name;
    case BusIdRole:        return r->busId;
    case BusNameRole:      return r->busName;
    case CodecRole:        return r->codec;
    case BitrateRole:      return r->bitrateKbps;
    case DestinationsRole: return r->destinations;
    case PositionXRole:    return r->positionX;
    case PositionYRole:    return r->positionY;
    default:               return {};
    }
}

QHash<int, QByteArray> AudioEncodeModel::roleNames() const {
    return {
        {IdRole, "audioEncodeId"},
        {NameRole, "name"},
        {BusIdRole, "busId"},
        {BusNameRole, "busName"},
        {CodecRole, "codec"},
        {BitrateRole, "bitrateKbps"},
        {DestinationsRole, "destinations"},
        {PositionXRole, "positionX"},
        {PositionYRole, "positionY"},
    };
}

} // namespace munkicast::ui
