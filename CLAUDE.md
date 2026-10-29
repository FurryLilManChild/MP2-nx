# mp2-nx: notes for Claude

Native Nintendo Switch homebrew port of Metroid Prime 2: Echoes (GameCube, USA G2ME01). No recompilation: the game
code comes from Echoes Unbound (Odrannnn/MetroidPrime2Port), a native PC port built on the PrimeDecomp/echoes
decompilation, rendered through Aurora (SDL3, WebGPU via Dawn), with a MusyX audio port. This repo adds the Switch
layer: Aurora and Dawn on Mesa for Horizon's NVK Vulkan driver, SDL3 on libnx, and the Horizon front end.

Prior art for the Switch layer: Alexgg1014's Twilight Princess port (Dusklight on Switch, CC0, built on native
Windows with devkitPro), KawaiiBunga/Melee-NX (GPL-3.0, Docker build), HayatoG/aurora-switch, and encounter's Dawn
fork with Switch support. Check each license before using code from it, and never copy from a repo with no license.

## Who is working on this

The owner is very tech savvy but writes no code. Claude writes all code and scripts. The owner builds on his own
PC (Windows), tests on the PC and on his Switch, and sends logs.
Instructions for him: numbered, click by click, copy-paste PowerShell. Never use em dashes in anything. No decorative
emoji. Do not start cloud build or test runs without asking him first.

## Hard rules

- Never commit game data or anything derived from it: the disc image, extracted DOL/RELs, shader or pipeline caches,
  logs, SD staging folders. `.gitignore` covers these; check `git status` before every commit.
- Upstream code stays on pinned commits. Switch changes to upstream go in `switch/patches/*.patch`, applied
  idempotently by the build script.
- Log each session in `docs/progress-log.md` (newest first) with evidence: what broke, the cause, the fix.
- Commit as `FurryLilManChild <346934328+FurryLilManChild@users.noreply.github.com>`.

