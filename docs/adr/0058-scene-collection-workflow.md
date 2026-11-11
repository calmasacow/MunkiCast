# ADR-0058: Scene collections in the desktop UI

Status: Accepted

## Decision

A scene collection is an existing native project directory, opened through LocalSession.
Document switches its session; the document schema and command protocol stay unchanged.
Rename uses SetProjectName. New collections use the studio canvas/encode/destination defaults.
A small atomic catalog under the application config directory records names, paths and the last
collection. Explicit --project takes precedence. Managed collections live under application data.

Before leaving a collection, save it successfully, including unsaved sessions into managed storage.
Open the target before releasing the current session. Failures leave current work available.
Duplicate/native import/export create fresh directories, regenerate project files and copy companion
files without copying locks, journals or recovery state. Existing destinations are refused.
External absolute asset references remain external; native copies retain relative companion files.
Remove requires a dialog naming the directory and moves it to desktop Trash. Failed trash leaves
the collection open. Open a remaining readable collection after removal, or a fresh default
collection if none remain. Reusing an existing collection avoids saving an Untitled replacement
when the user switches again.

Import and export use the desktop file-chooser portal. Native imports copy a chosen collection;
OBS imports ask for canvas dimensions and rational FPS because these belong to OBS profiles.
Both conversions show a durable report. OBS export writes standard scene collection JSON with
shared sources, scenes and groups, and reports approximations or omitted features. OBS profiles,
outputs, credentials, hotkeys and MunkiCast-specific routing are not scene collection JSON.
Native export retains the full model. Neither export accesses the external credential store.

## Validation

Drive actual dialog acceptance and menu actions in the headless shell check, isolated catalog and
collection storage, real project files, failures, switching and persistence. Exported OBS JSON is
parsed by the existing importer and rendered for supported transforms. Builds and full suite must
pass before committing.
