# BackEjectLoF

ACUFixes PluginLoader plugin for Assassin's Creed Unity 1.5.1: while hanging above a haystack, the leap-of-faith input
makes Arno backflip off the wall (Origins' animation, baked offline) and land in the hay.

- Build: `ACU-BackEject-LoF\build.ps1` (SolutionDir = the ACUFixes 1.5.1 source tree, which provides CommonLibACU).
- Runtime data (baked Origins tracks + mapping.json) is not in this repo: it is derived from Ubisoft game files.
- Design notes and the full RE log: `ACU-BackEject-LoF\PLAN.md` and `LEARNINGS.md` (outside this repo).

Accepted gameplay build: v0.18 (+ "mid ground v4" mapping). See the `v0.18-equivalent` branch.
