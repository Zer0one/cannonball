# CannonBall Libretro Modern development history

This document summarizes the public development lineage of CannonBall
Libretro Modern. Git history, release tags and upstream repositories remain
the authoritative sources for individual changes.

## Repository lineage

CannonBall was created by Chris White as a modern reimplementation of the
original OutRun engine. The standalone project is maintained at
[djyt/cannonball](https://github.com/djyt/cannonball).

The official Libretro port is maintained at
[libretro/cannonball](https://github.com/libretro/cannonball). This repository
is a fork of that port and contains the `cannonball-modern` integration branch,
which combines current Libretro development with tested enhancements and
packaged releases.

The modernization work was also contributed to the official Libretro project
through [pull request 48](https://github.com/libretro/cannonball/pull/48).

## Release milestones

| Release | Main result |
|---|---|
| v0.1.0 | Integrated the modern standalone engine with the Libretro core |
| v0.2.0 | Added black and white Ferrari palettes |
| v0.3.0 | Added custom music and functional Libretro reset support |
| v0.3.1 | Removed compiler warning noise and corrected legacy expressions |
| v0.4.0 | Removed Boost, migrated XML handling and improved reset behavior |
| v0.5.0 | Added per-track WAV volume and optional in-game music selection |
| v0.5.1 | Migrated custom music configuration to CSV and restored cross-platform builds |

Release tags preserve the exact source state used for each published version.

## Maintenance workflow

The fork uses two long-lived branches:

- `master` mirrors `libretro/master` without project-specific changes;
- `cannonball-modern` contains integration work, customizations, release tags
  and release builds.

Updates from `libretro/master` are integrated into `cannonball-modern`, built
and tested locally, and pushed only after validation. Published regressions are
corrected with explicit revert or follow-up commits so that release history
remains traceable.

The standalone `djyt/cannonball` repository is retained as the original engine
reference. Its changes normally reach this fork through the official Libretro
port unless a direct integration is deliberately required.

## Related resources

Custom music examples and supporting files are maintained separately in
[Zer0one/cannonball-libretro-resources](https://github.com/Zer0one/cannonball-libretro-resources).
ROM images, extracted game assets and other copyrighted data are not included
in this source repository.
