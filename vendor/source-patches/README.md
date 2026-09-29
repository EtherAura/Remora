# Remora source patches

The custom modifications that turn a stock LineageOS / AOSP tree into Remora's
hardware-accelerated image. A clean `repo init` + `repo sync` (see
../source-build/pinned-manifest-a<N>.xml for exact SHAs) yields upstream sources;
these patches layer Remora's work on top. The registry is `src/core/SourcePatches.cpp`;
in the GUI they appear on the Image page under *Build features & source patches*,
where enabling a build feature also ticks the patches it needs, and the *Source build*
card applies the selected set.

Each `<project-flat>/` holds patches for one repo project (path with `/`→`-`):
- `NNNN-*.patch` — git-am-able commits.
- `working.patch` — a `git apply`-able working-tree diff.

A `<project-flat>-a<N>/` directory, when it has content, replaces the shared one for
Android `<N>`. A `SKIP` file in it means that release needs nothing from the series
(its changes are already in the base), and the patch is not offered there.
