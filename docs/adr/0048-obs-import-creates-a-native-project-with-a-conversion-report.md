# ADR-0048: OBS import creates a native project with a conversion report

Status: Accepted

## Context

OBS collections describe sources and scenes; base resolution and frame rate normally live in
the separate profile. Source settings are sparse and plugin/version-specific. Native groups use
canvas-sized bounds, whereas OBS groups compute tight bounds. Pretending these formats are
interchangeable would silently change shows.

## Decision

Import is an explicit conversion into a new directory, never a change to the OBS collection or
an existing MunkiCast project. Require canvas width, height and rational frame rate. Resolve UUIDs
first, names only when unique. Preserve shared source identities, scene ordering, nested scenes,
visibility, locks, audio routes and supported placement. Reverse OBS's bottom-first item order.

Use native Linux backends, without libobs. OBS groups become nested scenes with their saved
tight dimensions; skip compatibility backup items to avoid duplicate pictures. Quarter turns,
flips, cropping and bounds are converted using shared compositor geometry. Arbitrary rotations,
filters and blend differences remain explicitly reported. Unknown source records retain their
original JSON in source settings and appear as unavailable, rather than disappearing.

Import performs no capture or network access. File paths resolve against the collection's
directory; absent files and foreign device identities require operator repair. A durable report
records approximations, unsupported options, transitions, hotkeys and output settings. Original
JSON is retained alongside the report for future conversion improvements. Parser input, counts,
nesting and dimensions are bounded; malformed references fail before writing.

## Consequences

The importer is a separate library used by the CLI and UI. It may use Qt image/font metadata;
the document library keeps no Qt dependency. Tests create and reopen actual projects, render
converted layers, exercise source sharing/order/crops, and reject malformed input and existing
destinations. Import success means an editable conversion with a report, not OBS pixel parity.
