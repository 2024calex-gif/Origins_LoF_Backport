/*
BackEjectLoF - an ACUFixes-loader plugin (v0.27.1, probe inspector plus experiments; H = Origins backflip; J = trajectory recorder; K = camera selector diagnostic; L = LoF camera during the eject; M = early hay camera; N = eject action sampler; O = graph variable recorder; P = player state log; Q = state constructor probe).

Purpose: record which parkour "probes" (level-placed parkour points) the game's parkour scanners test,
so we can learn whether haystack probes (type 36) are seen while the player hangs on a ledge.
Apart from experiment C (default OFF), nothing the game decides is changed: every hook returns the original's result.

Technique: "fancy vtable" slot swap.
  Parkour action classes and parkour TESTERS (scanners) both have fancy vtables: arrays of 0x20-byte entries
  {uint32 nameHash @0, uint32 staticIdx @4, uint32 nestedness @8, pad, uint64 fn @0x10, pad}.
  We overwrite only the 8-byte fn pointer of one entry in each shared table. Every hook has its own compile-time thunk,
  so the thunk knows its row exactly and calls that row's original function with all arguments unchanged.
  Why this way: swapping data pointers instead of patching code avoids conflicts with ACUFixes, which
  code-patches the parkour functions.

A) Action hook, slot 0x4B (nameHash 0x3DD63101), InitialTestIfActionFits (InitialTestSlots.inc).
  CreateIfFits calls it through the table for every candidate action, with the probe as argument 7.
  After the original returns, the action (arg 1) and the probe (arg 7, if non-null) go into a ring buffer.

A2) Tester hook, slot 0x21 (nameHash 0xFBF777BC), gather leap-of-faith/haystack probes (TesterGatherSlots.inc).
  The scan calls it as (tester, &tester+0x90, r8, float xmm3, float [rsp+20], player [rsp+28], SmallArray<Probe*>* out).
  The thunk records the tester's enum list (SmallArray<int32> at +0x140), the out-array size before and after the
  original, and up to 16 newly gathered probes. For the player only, it also keeps a "hay cache": the newest type-35
  probe at least 3 m below the player, with the smallest horizontal distance, copied as raw bytes (0x00..0x5F) plus the
  hay entity (from the probe's shared block at +0x60).

A3) Back-eject tester hook, table 0x1439F0F30, slot 0x25 (nameHash 0xC686322F), the tester that creates the back eject.
  Its original is called first with all nine arguments. Then, only when experiment C is ON, see below.

C) Experiment "LoF from hang" (default OFF; menu checkbox only since v0.8; every toggle is logged).
  Why: hang scanners never try enum 112 (leap of faith). Their enum lists are empty; their moves come from hard-coded
  per-move functions. So instead of appending to a list, we add a LoF action aimed at the hay's own type-35 probe into
  the back-eject scan's candidate list, the same way ACUFixes adds wallEjectToHang there, but through the tester's
  fancy-table slot instead of a code patch.
  Per back-eject call (player's tester only): require the hay cache to be at most 60 frames old, the hay to be at least
  3 m below and at most 25 m horizontally from the player. Then the hay probe bytes are copied into our own probe buffer
  g_hayProbe, its shared ref is bound to the hay entity (game function 0x140159940, only when the entity changes), and
  the game's CreateIfFits (0x1401D1260) is called with enum 112 and the hay probe. A created action is appended to the
  back-eject's action array with SmallArrayAppend (0x1426E4100).
  Counters: attempts, created, appended, rejected, skipped (no fresh hay or too far), faults. Any fault turns C off
  and is logged by the frame callback. The hay cache is written on the game thread and only read from it, so the menu
  may show a torn value for a moment; that is accepted for a debug display.

D) Final-filter hook (v0.6.0). v0.5 in game: the injected LoF passed the candidate test (fitness 94.4) but the game
  still chose the eject. The likely blocker is the LoF action's FinalFilter1 (slot 0x76 of its fancy table
  0x143A48030, fn 0x1401B2280), which traces the flight path for collisions; from a hang the path starts at the wall.
  (v0.7) Hay memory: haystacks are static, so the player's gather calls feed a memory of up to 32 type-35 probes
  (merged within 0.5 m, oldest evicted). The back-eject attempt chooses from that memory: at least 3 m below the
  player, at most 25 m horizontally, seen within 60 s (3600 frames), nearest horizontally. Skips are classified as
  noHay, notBelow, tooFar or stale. Attempts are logged at most once every 10 frames.
  While the injected action is current (appended within the last 10 frames), the slot 0x76 thunk calls the original
  first and counts it. If experiment C is ON and "Force final filter" is ON, it returns 1 instead, so the injected
  action passes. Slot 0x73 (fn 0x140196C90, mode argument) is only observed, never changed. Both are plain fn-pointer
  swaps in the LoF action's own table, like the others. Nothing else is forced.

E) Experiment "eject on hay" (v0.8; default ON since 0.8.1; F6 toggles it; logged). Why: the hang state can perform the back eject
  (enum 36) but not LoF (112): with C, the LoF was selected yet nothing executed. So E makes the back eject itself land
  on the haystack. After the original back-eject call (player only, same hay choice as C): a second probe buffer
  (same bytes as the hay, type set to 28, the type the vanilla eject candidates use) goes into CreateIfFits with enum 36.
  If it creates an action, vanilla candidates with enum 36 or 50 are removed from the actions array (their memory is
  not freed: a small leak, accepted for this experiment) and ours is appended. The eject's FinalFilter1 (slot 0x76 of
  table 0x1439F8A70) is observed for our action; "Force eject final filter" (default OFF) returns 1 for it.
  Vanilla then catches a fall onto the hay, which is what the user saw happen once with an eject clipping a cart edge.

F) Range widening for our eject (v0.9; menu checkbox, default ON). Why: in game, experiment E landed Arno in the hay
  from 5-7 m drops, but from 10.5 m and 15 m the eject's InitialTest rejected our action, so the vanilla eject
  overshot. The range setter (fancy slot 0x64 of the eject table, fn 0x14013F720, SetExpectedDistanceRangesAccordingToParkourMode)
  runs inside our CreateIfFits call through InitialTest. A game-thread flag is set only around that call. The hook
  calls the original first, then (inside that window only) records the 7 range fields and, if the option is on,
  writes wider values: heightDifferenceMin -150, horizontalDifferenceMax 30, distanceMax 160, durationExtension 10,
  curveMax 100, curveMin -100, expectedCurveMin -100. The 0x1D4 field is left alone on purpose. Assumption: the
  widened fields are on the action passed to the setter (arg 1).

G) On-demand gatherer query (v0.10; menu checkbox, default ON; rate-limited to once per 10 frames). Why: the first eject
  of a session was skipped with noHay because the plugin had not seen that haystack yet (the gatherer only runs
  during the game's own scans). Before choosing a hay, the back-eject thunk calls the game's haystack gatherer
  (0x14012F150, the function the tester slot 0x21 wraps) directly with the tester's position, a vector, 150 m, and the
  player, into a plugin-owned SmallArray. Every probe it returns goes through the hay memory. The hay memory freshness
  is now 36000 frames (10 minutes), since haystacks are static. Leak: the gatherer's probes for our array are not
  freed (see the comment at g_qArr). Assumption: the gatherer accepts a zero-initialised SmallArray.

H) Origins backflip animation (v0.11; v0.12 retime; v0.14 flight variants; menu collapsible header, checkbox default ON). Why: swap the clip the game plays
  for the back eject onto the Origins backflip motion, through the game's own track data, with no code patch and no hook
  (OriginsAnim.inl). Data: <plugin dir>\BackEjectLoF\origins\mapping.json and the source files it names: v0.14 binary
  *.bake.bin (magic BEJ1, little-endian, see ParseBinary; 21 MB for all 132 files) or the older JSON *.baked.json (the WD2Vaults
  prebaked format). Keys are already in the game clip's 60 Hz frames. Loaded once at init; a missing folder or file only
  makes the feature inactive. About once a second, after the player has existed for 5 s, each clip is looked up by its
  handle (ACU::StrongRef, kept alive while built), its matching slots (TrackID and information type) get new track data
  built with the game codecs (1 for rotations, 28 for translations since v0.15, 27 only if 28 is unusable; sources may hold 27 or 28), and the first rotation and translation tracks are
  decoded back and compared; a mismatch means the clip is not used. Built data is never freed and never written into the
  clip yet. Arming runs on the GAME THREAD, synchronously, right after experiment E appends our eject (ArmFromGameThread,
  before the eject clip can start): it walks the current ArmPlan (immutable list of {slot address, original, new} built
  from the built clips) and writes the new pointer into each slot that still holds its original. It touches only atomics:
  no logging, no containers. The window (slider 1 to 10 s, default 5 s) restarts on every such arm. The frame callback
  logs the eject and the arm, and disarms when the window ends, the checkbox goes off, or "Restore originals now" is
  pressed: it walks the current plan and writes the original only where the new pointer is still in place. The frame
  callback republishes the plan after each build and before a built clip's StrongRef is released. Replaced plans are
  leaked on purpose (small, and a game thread may still be reading one), never deleted. Each second the clip's handle is
  looked up again; if it is another Animation object, or gone, that clip is dropped from the plan without writing to it.
  Any fault (game thread or frame side) turns the feature off for the session, logged, with no further writes.
  v0.12 retime: a mapping entry with new_land / new_end / new_length (plus takeoff, land, end, orig_length) is a retimed
  clip. Its baked tracks are clamped to the new end; every other track slot of the clip is re-encoded on the new timeline
  (warp: old takeoff to land stretched to the new land, after the land shifted by the new end minus the old end), with the
  u16-time codec of the same value format, and each key is self-checked. Any slot that cannot be retimed exactly, or an
  orig_length that differs from the game clip by more than 0.01 s, means the clip is not used. The clip's Length becomes
  new_length while armed, through the plan (written only where it still holds the original, restored the same way).
  Default arm window 5 s. Entries without the new_* fields behave as in v0.11.
  v0.14 flight variants: mapping.json has one entry per (handle, variant_flight), flight in frames (72, 110, 172, 220,
  260, 304). Entries of the same handle and different variants target the same Animation object and the same slots;
  only one variant is armed at a time. An entry without variant_flight is variant 72. Load groups the entries by flight
  (sorted, at most 8 flights) and logs the count per variant; each variant has its own immutable ArmPlan (g_plans[index],
  index = position in the sorted flights). Building: up to 12 entries per poll; nothing is built, looked up or dropped
  while a variant is armed (the originals would read as our pointers), so a clip is built only from original tracks.
  Selection on the game thread (ArmFromGameThread, given the drop = player z minus hay z of the eject just appended):
  the game's own leap-of-faith timing gives the fall seconds for that drop (piecewise linear: 0 m 0.45 s, 3 m 0.47,
  15 m 1.13, 30 m 1.83, 45 m 2.87, 70 m 4.07, 100 m 5.07, then +0.033 s per metre); times 60 is the target frames.
  The variant is the smallest flight >= target, or the largest. If its plan is empty, the non-empty variant nearest the
  target is used. A different armed variant is restored first (only where our pointer is still in place). A drop of
  -1 (not available) uses variant index 0. Window: the slider or the chosen flight plus 3 s, whichever is longer. The
  disarm walks the armed variant's plan; the checkbox and "Restore originals now" walk every variant's plan.
  v0.17 root scaling: the game rescales the eject clip's root (TrackID 0, position) so its end point lands on the real
  target, per axis. A mapping entry with root_scale (variants 110 to 304) has a normalized root template (10 m across,
  50 m down, the vanilla leap-of-faith shape). Its root track keeps a copy of the template values and a 16-byte aligned
  scratch buffer, both leaked like the trackdata. The game-thread arm rescales the template of the chosen variant to the
  attempt's drop and horizontal distance (drop 1..200 m, horizontal 0..60 m; otherwise the template is kept), re-encodes
  the root track and decodes its last key back. A failed check keeps the template. The rescale runs before the variant's
  plan is written. Frame log: "root scaled for N clip(s) to drop D m, horiz H m (failed M)".

I) Hay registry query (v0.13; menu checkbox, default ON, rate-limited like G). Why: experiment G asks the game's haystack
  gatherer (0x14012F150) for probes, but the gatherer passes every candidate through a filter (0x1401C7B70) that accepts
  only objects IN FRONT of the player (dot of the player's forward with the direction to the object). A hanging player
  faces the wall and the haystack is behind it, so G never found anything while hanging. At high places (Notre Dame) the
  first back eject therefore got noHay unless the player had looked at the hay before.
  Fix: read the game's list of leap-of-faith landing targets directly, which is what the gatherer iterates, with no facing
  filter. Steps (all absolute addresses, ACU 1.5.1): key = *(uint64*)(player + 0x60); owner = registry lookup 0x140D61A50(key)
  (a plain search of a global table, no side effects); owner + 0x60 is a pointer to an array of element pointers and
  owner + 0x6A is a uint16 count (loop capped at 512). For each element: entity = *(elem + 0x10), skipped if 0; the landing
  point p = 0x140CF5A50(elem, out16, tester + 0x90), skipped if null, read as x, y, z, w. The probe is built the way the
  game builds it: +0x00 p, +0x10 dir (0, 0, 1, 0), +0x20..0x2F zero, +0x30 uint64 0, +0x38 uint32 type 35, +0x40 p, +0x50 p;
  the shared ref at +0x60 is not copied here (the hay code binds it to the entity when the hay is used). The result is the
  same HayObs that CollectHayGuarded makes, and goes into the hay memory through RememberHay.
  Selection: the first 64 landing targets that are within 60 m horizontally of the player (not the nearest 64).
  Counters: calls, list size (last), remembered (total), faults. A fault turns this query off and is logged by the frame callback.

J) Trajectory recorder (v0.16; menu checkbox "Record eject trajectories (v0.16)", default ON; read-only). Why: the long
  flight variant undershot the haystack in game, and the real flight path is needed next to the clip's root motion.
  Each new game-thread arm (the Origins arm serial) starts a recording. Per frame callback it samples QPC seconds since
  the arm, the player position (GetPlayerPosGuarded, the read GameFrame already makes; no matrix reader exists) and whether
  the Origins clips are armed. It stops at the arm window + 1.0 s, 10 s after the arm, or 2400 samples, and writes
  <plugin dir>\BackEjectLoF\traj\traj_<session stamp>_<serial>.csv (start values as "# key=value" lines, then t,x,y,z,armed).
  A new arm while recording ends the current one first. The serial restarts at 1 each session, so a file from an earlier
  session with the same serial is overwritten. A fault in the player read turns the recorder off for the session.
  No writes to game memory, no hooks.

Recording: scans only run while the player gives input, so a single press with a 60-frame window misses them.
F7 (or the Start/Stop button) starts a recording session; the second press stops it and writes a session report to the log.
While recording, action records are folded into a session store keyed by (enum, type, probe pos, dest) at 1 cm, and
gather records into a per-tester-table store. The player position is sampled every 15 frames.
Players only unless "Include NPCs" is checked when the session starts.

K) Camera selector diagnostic (v0.19; menu checkbox "Log camera selector changes (v0.19)", default ON; read-only, polling
  only, frame side only, no hooks). Why: find which camera selector node the camera uses, and which node it blends toward,
  when the back eject and the hang change the camera. Each frame it reads the first camera component of the CameraManager
  singleton: +0x8F8 (CameraData), +0x9B8 (SharedBlock of the current selector blender node) and +0x9C0 -> +0x18 (a second
  node). When any of these changes it logs one line with the node type names. The first time a node is seen it logs a dump:
  the type chain, the first 0x60 bytes, the rule condition (type members, a few levels deep), the camera data type, the
  transitions and the children. Children are dumped gradually (8 per frame, at most 300 nodes per session). Type names come
  from the game's own GetTI (vtable slot 5), called only when the vtable lies in ACU.exe .rdata and slots 0..5 all point into .text.
  Every read is SEH-guarded; a fault is counted, and more than 1000 turn the feature off for the session. Log: the normal
  BackEjectLoF log. Code: CameraDiag.inl.
  v0.22: once per session (300 frames after start, or the menu button "Dump camera tree") the whole selector tree from CameraManager+0x10 (ACU_Camera_Root) and +0x18 is logged as 'tree ...' lines, one per node, with the rule types.
  v0.24: default OFF (the camera log is no longer needed; the menu checkbox can still turn it on).

L) LoF camera during the eject (v0.21; menu checkboxes "LoF camera during the eject (v0.21)" and "Bridge to the hay
  camera at landing (v0.21)", both default ON). Why: the vanilla leap of faith hands the camera over to the hay camera in
  the air; ours fell back to the default camera for about 0.3 s. No ACU.exe writes: the v0.20 vtable slot patch failed in
  game. Instead the BooleanRuleCondition vtable (0x14306F850) is copied into our DLL, its slot +0x58 (GetResult, original
  0x1420B5210, checked first) is replaced by our hook, and only the rule objects we find get our copy as their vtable
  pointer. Two rules, found by a bounded breadth-first search (depth 3, 200 nodes, 64 children per node) from the current
  camera node every 30 frames while unknown, and re-checked every 60 frames: the LoF node 0x1316A17B3E (SharedBlock handle,
  rule at node+0x18) has its rule forced true from our arm until the end; its parent group 0x102BC104D7 (child of the root
  0x863EA9EBA) has its rule forced true only in the landing phase, from player z <= hay z + 1.5 m until the hay camera
  0x139346D58A appears, 0.6 s after landing, disarm or 6 s, whichever comes first. Hook rules: no allocation, no logging,
  no locks, no SEH, atomics only. Code: LofCamera.inl. Log: "lofcam: ..." lines in the normal BackEjectLoF log.
  v0.22: disabled (Install is not called; the user rejected it in game).

M) Early hay camera (v0.23; menu checkbox "Early hay camera at landing (v0.23)", default ON). Why: the vanilla leap of
  faith switches the camera to the hay (hide spot) camera about 1 m above the hay; ours switched to the default camera
  at landing and only reached the hay camera 0.3 s later. Two rules are forced: the group node 0x103CA083FC (handle,
  depth 3 below the root, BooleanRuleCondition, its list comes before the default/airborne branch) and its hay camera
  node 0x139346D58A (BooleanRuleCondition with a HideSpotTypeCondition child). Same technique as L: per-object vtable
  swap of the two rule objects to a copy of the BooleanRuleCondition vtable 0x14306F850 whose slot +0x58 (original
  0x1420B5210, checked first) is HayRuleHook. No ACU.exe writes. The rules are found by a bounded breadth-first search
  from the root 0x863EA9EBA (depth 6, 800 nodes, 64 children per list, both lists) every 30 frames while unknown, and
  re-checked every 60 frames. When forced: from the first frame after 0.3 s of the arm where the player is within
  max(1.5 m, 0.2 s of fall speed) above the hay top, until the game's own results of both rules are true, 0.8 s after
  the force starts, disarm, or 6 s after the arm, whichever comes first. The hook always calls the original first, and
  only changes the result of these two rules while forcing. Hook rules as in L. Code: HayCamera.inl. Log: "haycam: ..."
  lines in the normal BackEjectLoF log.
  v0.24: disabled (Install is no longer called; the user rejected it, "camera is still broken"). No rule is swapped.

N) Eject action sampler (v0.24; no menu checkbox, runs with the trajectory recorder, read-only, frame side only, no hooks).
  Why: in game every eject lands about 1.9 to 2.0 s after the arm whatever the height (13 to 62 m), although our clip is
  2.4 to 3.7 s long, so the game pulls the player to the target on its own fixed timing. This section looks for the
  duration and progress fields in the eject action object (0x290 bytes, allocated by the action factory for enum 36).
  The action is g_injectedEject, the one experiment E appended. At each recording start the pointer is taken; each
  trajectory sample (same frame, same time) also copies the 0x290 action bytes (SEH-guarded; a failed read only marks the
  sample not ok). At most kTrajMaxSamples samples. When the recording is written, the file
  <plugin dir>\BackEjectLoF\traj\action_<session stamp>_<serial>.csv is written too: "# " header lines, the initial values
  of the first readable sample for every 4-byte offset (float and int view), then one column per offset whose 32-bit
  value changed during the recording ("o%03X"), with t and z per row. Values with a zero exponent or an all-ones
  exponent (and not zero) are printed as hex ("i%08X"). Log: "action sampler: ..." lines.

O) Graph variable recorder (v0.25; menu checkbox "Record animation graph variables (v0.25)", default ON; read-only, frame
  side only, no hooks). Why: every eject lands about 1.9 s after the arm at every height, because the animation graph warps
  the root motion through its graph variables (TargetHeight, TargetDistance, SyncA..E_*, LandingHeight, FreeFallState,
  HayStack, WallEject, TimeRemaining, ...). This section records all of them over time, to compare a vanilla leap of faith
  with our eject. Each frame, right after haycam::Frame, the player's AtomAnimComponent (pD0 = GraphEvaluation) is read:
  rtcp hashes and offsets, and the values in stru400.graphVarsBuffer (SEH-guarded, up to 1024 variables), into a ring of
  300 samples allocated once. A dump, <plugin dir>\BackEjectLoF\traj\graph_<session stamp>_<k>.csv, is written 2.0 s after
  a rising edge of the HayStack variable, or 6.0 s after a new eject arm, at most 12 per session. The file lists every
  variable as a "# var" line (hash, value in the oldest sample), then one column per variable that changed inside the ring.
  Code: GraphVars.inl. Log: "graphvars: ..." lines.

P) Player state log (v0.26; menu checkbox "Log player states (v0.26)", default ON; read-only apart from one hook). Why:
  to compare the player's human states during a vanilla leap of faith with our eject. Every state Enter() is called from
  one place in ACU.exe: at 0x1427554E3 (mov rax, [rbx+0xE0], 7 bytes, RBX = the state node about to be entered). The hook
  is there, with the same 7 bytes stolen, so it never overlaps ACUFixes' logger at 0x1427554F2. Install verifies the bytes
  first and skips the hook on a mismatch. The callback records only player states (same test as ACUFixes: topmost parent
  of the node is HumanStatesHolder::GetForPlayer(), or the node is the holder itself) into a lock-free 4096-entry ring
  (QPC, enter function, node, depth). The frame side copies the ring every frame into a 1500-entry history with the
  player z and the Origins armed flag at drain time. A dump, <plugin dir>\BackEjectLoF\traj\states_<graph stamp>_<k>.csv,
  is written next to each graph dump (same stamp, k, trigger and reason), with the entries from 12 s before to 8 s after
  the trigger. Code: StateLog.inl. Log: "statelog: ..." lines.

Q) State constructor probe (v0.27.1; menu checkbox "Log state constructors (v0.27)", default ON; read-only apart from two
  function-start hooks). Why: to see what the game passes into the constructors of InSafeJumpOrLoweringToHang (0x141ABA990,
  kind SJ) and DetachingFromWall (0x141A274D0, kind DW). Each hook steals the same 9 bytes (push rbx; sub rsp,20h; mov rbx,rcx),
  verified first; a mismatch skips only that hook. The callback copies the registers, xmm0..7 (low 4 floats), 48 qwords from rsp
  and, for every candidate value, the state enter function at +0xE0 when it lies in ACU.exe code, plus the player test. It
  writes a 256-entry lock-free ring. The frame side keeps a 256-entry history and counts entries per (kind, ret). The first 2
  entries of each new key are logged in full. When a player enter of InSafeJump (0x141ADC860) or DetachingFromWall (0x141A4B010)
  is seen (StateLog OnEnterState counters), a "ctorprobe trigger SJenter|DWenter" line is written and the matching constructor
  entries from 0.5 s before to 0.05 s after the enter are logged in full, newest first, at most 6 per trigger, for 3 frames.
  Then a "ctorprobe counts:" line lists the top 16 keys. Full logs are capped at 200 per session.
  Code: CtorProbe.inl (and the enter counters in StateLog.inl). Log: "ctorprobe: ..." lines.

Hook rules: never throw, never allocate, never log to file, never block (TryAcquireSRWLockExclusive only).
SEH helpers are plain functions with no C++ objects, so __try is legal in them.

Data: <plugin dir>\BackEjectLoF\BackEjectLoF-log.log (truncated on each start).
*/
#include "pch.h"

#include <cstdint>
#include "ACU/World.h"
#include "ACU/Entity.h"
#include "ACU/ACUGetSingletons.h"
#include "ACU/AtomAnimComponent.h"   // v0.25 (O): GraphEvaluation, AtomGraph_RTCP
#include "ACU/AtomGraph.h"
#include "ACU/HumanStatesHolder.h"            // v0.26 (P): FunctorBase, HumanStatesHolder::GetForPlayer
#include "AutoAssemblerKinda/AutoAssemblerKinda.h"   // v0.26 (P): mid-function hook
#include "ACU/CameraManager.h"
#include "Common_Plugins/Common_PluginSide.h"
#include "ACU/Animation.h"
#include "ACU/ManagedPtrs/ManagedPtrs.h"
#include "ACU/Memory/ACUAllocs.h"
#include "SimpleJSON/json.hpp"
#include "Serialization/ToFromFile/ToFromFile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// Action slot constants (ACU 1.5.1). Entry = vtable + index * kSlotStride; fn pointer at entry + kSlotFnOffset.
constexpr uint32 kSlotNameHash = 0x3DD63101u;      // InitialTestIfActionFits
constexpr uint32 kSlotIndex = 0x4Bu;
// Tester slot constants.
constexpr uint32 kTesterNameHash = 0xFBF777BCu;    // gather leap-of-faith/haystack probes
constexpr uint32 kTesterIndex = 0x21u;
// Back-eject tester slot constants.
constexpr uint64 kBackEjectTable = 0x1439F0F30u;
constexpr uint32 kBackEjectNameHash = 0xC686322Fu;
constexpr uint32 kBackEjectIndex = 0x25u;
constexpr uint64 kBackEjectExpectedFn = 0x140138840u;
constexpr uint64 kSlotStride = 0x20u;
constexpr uint64 kSlotFnOffset = 0x10u;

// Game functions used by experiment C.
constexpr uint64 kCreateIfFitsAddr = 0x1401D1260u;
constexpr uint64 kSmallArrayAppendAddr = 0x1426E4100u;
constexpr uint64 kInitPlayerRefAddr = 0x140159940u;
constexpr uint64 kEmptySharedBlock = 0x14525DA98u;

// LoF action fancy table (experiment D).
constexpr uint64 kLofTable = 0x143A48030u;
constexpr uint32 kLofFinalFilterHash = 0xF0455AE0u;
constexpr uint32 kLofFinalFilterIndex = 0x76u;
constexpr uint64 kLofFinalFilterExpected = 0x1401B2280u;
constexpr uint32 kLofModeHash = 0x081284CAu;
constexpr uint32 kLofModeIndex = 0x73u;
constexpr uint64 kLofModeExpected = 0x140196C90u;
constexpr uint64 kInjectedCurrentFrames = 10;

// Experiment E: back eject (enum 36) aimed at the hay.
constexpr uint64 kEjectTable = 0x1439F8A70u;          // eject action fancy table
constexpr uint32 kEjectFinalIdx = 0x76u;              // FinalFilter1 slot of the eject table (hash checked, fn logged)
constexpr uint32 kEnumFancySlotIdx = 0x45u;           // fancy slot that returns an action's enum
constexpr uint64 kSmallArrayRemoveAddr = 0x142726000u;
constexpr int kEnumEject = 36;
constexpr int kEnumVanillaAlt = 50;                   // the other vanilla candidate removed alongside 36
constexpr uint32 kEjectProbeType = 28;                // type the vanilla eject candidates use

// Eject range setter (experiment F): fancy slot 0x64 (SetExpectedDistanceRangesAccordingToParkourMode).
constexpr uint32 kEjectRangeHash = 0xF914A46Fu;
constexpr uint32 kEjectRangeIdx = 0x64u;
constexpr uint64 kEjectRangeExpected = 0x14013F720u;
constexpr uint64 kRangeHeightMinOff = 0x1D0u;        // heightDifferenceMin
constexpr uint64 kRangeHorizMaxOff = 0x1DCu;         // horizontalDifferenceMax
constexpr uint64 kRangeDistMaxOff = 0x1E4u;          // distanceMax
constexpr uint64 kRangeDurExtOff = 0x224u;           // expectedDurationMaxExtension
constexpr uint64 kRangeCurveMaxOff = 0x230u;         // curveAllowedRangeMax
constexpr uint64 kRangeCurveMinOff = 0x234u;         // curveAllowedRangeMin
constexpr uint64 kRangeExpCurveMinOff = 0x22Cu;      // expectedCurveRangeMin

// Game haystack gatherer (experiment G): the same function the tester slot 0x21 wraps. Called directly, not via a table.
constexpr uint64 kGathererAddr = 0x14012F150u;
constexpr uint64 kGathererPosOffset = 0x90u;         // tester+0x90 -> rdx
constexpr uint64 kGathererVecOffset = 0x50u;         // tester+0x50 -> r8
constexpr float kGathererRadiusArg = 150.0f;         // [rsp+20]
constexpr uint64 kQueryInterval = 10;                // frames between on-demand queries

// Experiment I (v0.13): leap-of-faith landing-target registry, read directly (no facing filter).
constexpr uint64 kRegistryLookupAddr = 0x140D61A50u; // registry lookup: owner = fn(key), key = *(player + 0x60)
constexpr uint64 kRegistryLandAddr = 0x140CF5A50u;   // landing point: p = fn(elem, out16, tester + 0x90)
constexpr uint64 kRegistryKeyOffset = 0x60u;         // player + 0x60
constexpr uint64 kRegistryListOffset = 0x60u;        // owner + 0x60: uint64 pointer to element pointers
constexpr uint64 kRegistryCountOffset = 0x6Au;       // owner + 0x6A: uint16 count
constexpr uint64 kRegistryEntityOffset = 0x10u;      // elem + 0x10: entity
constexpr uint32 kRegistryMaxElems = 512;            // loop cap
constexpr size_t kRegistryCap = 64;                  // HayObs written per query (first 64 within the radius)
constexpr float kRegistryRadius = 60.0f;             // horizontal distance from the player, metres

// Probe layout (offsets into the probe struct). Position is a Vector3 at 0x00 (x, y, z).
constexpr uint64 kProbeTypeOffset = 0x38u;
constexpr uint64 kProbeDirOffset = 0x10u;
constexpr uint64 kProbePos40Offset = 0x40u;
constexpr uint64 kProbePos50Offset = 0x50u;
constexpr uint64 kProbeBlockOffset = 0x60u;        // SharedBlock* of the probe's entity reference
constexpr size_t kProbeCacheBytes = 0x60u;         // bytes 0x00..0x5F are copied into the hay probe
constexpr size_t kHayProbeBufBytes = 0x80u;
constexpr uint32 kProbeTypeHay = 35;               // haystack type in the gather list
constexpr uint32 kEnumLeapOfFaith = 112;

// SharedBlock layout: {uint64 manObj @0; int32 weak @8; int32 strongAndFlags @0xC}.
constexpr uint64 kBlockObjOffset = 0x0u;
constexpr uint64 kBlockFlagsOffset = 0xCu;
constexpr uint32 kBlockStrongFlag = 0x80000000u;

// Action layout (offsets into the action object, arg 1).
constexpr uint64 kActionDestOffset = 0x30u;
constexpr uint64 kActionDestFacingOffset = 0x40u;
constexpr uint64 kActionHeightDiffOffset = 0xC0u;
constexpr uint64 kActionHorizDiffOffset = 0xC4u;
constexpr uint64 kActionDistanceOffset = 0xC8u;
constexpr uint64 kActionLayerOffset = 0xE0u;
constexpr uint64 kActionFitnessOffset = 0x204u;

// SmallArray layout (#pragma pack 1): arr @0, capacity|flags uint16 @8, size uint16 @0xA.
constexpr uint64 kSmallArraySizeOffset = 0x0Au;
// Tester layout: enum list SmallArray<int32> at +0x140 (used only to report whether 112 is listed).
constexpr uint64 kTesterEnumArrOffset = 0x140u;
constexpr uint64 kTesterEnumCapOffset = 0x148u;
constexpr uint64 kTesterEnumSizeOffset = 0x14Au;
// Back-eject tester fields used by experiment C.
constexpr uint64 kTesterStateFloat1Offset = 0x104u;
constexpr uint64 kTesterStateFloat2Offset = 0x108u;
constexpr uint64 kTesterMoveVecOffset = 0x20u;     // movement vector; passed as CreateIfFits a4 (as ACUFixes does)
constexpr uint64 kTesterPlayerOffset = 0x118u;
constexpr uint64 kTesterPrevActionOffset = 0x120u;

constexpr uint32 kUnknownCount = 0xFFFFFFFFu;
constexpr uint32 kNoProbeType = 0xFFFFFFFFu;        // type recorded when the call had no probe
constexpr uint32 kMaxFaults = 3;
constexpr size_t kRingSize = 8192;
constexpr size_t kGatherRingSize = 2048;
constexpr size_t kGatherEnumMax = 48;
constexpr size_t kGatherNewMax = 16;
constexpr size_t kGatherScanMax = 64;               // new probes examined for the hay cache
constexpr size_t kTesterProbesCap = 500;
constexpr uint64 kRecentFrames = 60;
constexpr size_t kMaxTableRows = 200;
constexpr float kDefaultMaxDistance = 40.0f;
constexpr size_t kMaxSessionKeys = 20000;
constexpr uint64 kSampleEveryFrames = 15;
constexpr size_t kMaxSamples = 2000;
constexpr uint64 kHayMaxAgeFrames = 36000;         // 0.10.0: 10 minutes at 60 fps. Haystacks are static; alive-check guards unloads
constexpr size_t kHayMemCap = 32;
constexpr float kHayMergeDist = 0.5f;              // a sighting within this distance refreshes the same hay
constexpr size_t kHayObsMax = 16;                  // new type-35 probes copied per gather call
constexpr uint64 kAttemptLogInterval = 10;         // frames between attempt log lines
constexpr float kHayMinBelow = 3.0f;
constexpr float kHayMaxHoriz = 25.0f;
constexpr uint64 kCounterLogInterval = 30;

// InitialTestIfActionFits, nine integer-sized arguments forwarded bit-exactly.
using InitialTestFn = bool(__fastcall*)(uint64 action, uint64 a2, uint64 a3, uint64 a4, uint64 a5,
                                        uint64 a6, uint64 probe, uint64 player, uint64 prevAction);
// Tester gather: the 4th parameter is a float (xmm3). Return forwarded as uint64.
using GatherFn = uint64(__fastcall*)(uint64 tester, uint64 posPtr, uint64 r8, float f4, uint64 s5,
                                     uint64 playerEntity, uint64 outProbes);
// Back-eject tester: nine integer-class arguments, forwarded as uint64; return forwarded unchanged.
using BackEjectFn = uint64(__fastcall*)(uint64 tester, uint64 origin, uint64 r8, uint64 r9, uint64 a5,
                                        uint64 a6, uint64 a7, uint64 probes, uint64 actions);
// CreateParkourActionForParkourPointIfFits (CreateIfFits).
using CreateIfFitsFn = char(__fastcall*)(int enumAction, uint64 a2, uint64 a3, uint64 a4, float a5, int a6, char a7,
                                         uint64 probe, uint64 player, uint64 prevAction, uint64* outAction,
                                         float a12, float a13);
using SmallArrayAppendFn = void(__fastcall*)(uint64 array, uint64 item, uint64 zero);
using InitPlayerRefFn = void(__fastcall*)(uint64 refAt0x60, uint64 entity);
using FinalFilterFn = uint64(__fastcall*)(uint64 action, uint64 a2, uint64 a3, uint64 a4);
using LofModeFn = uint64(__fastcall*)(uint64 action, uint64 mode);
using EnumFn = int(__fastcall*)(uint64 action);
using RangeFn = uint64(__fastcall*)(uint64 action, uint64 a2, uint64 mode);
using SmallArrayRemoveFn = void(__fastcall*)(uint64 array, int index, uint32 elemSize);
using RegistryLookupFn = uint64(__fastcall*)(uint64 key);
using RegistryLandFn = float*(__fastcall*)(uint64 elem, float* out, const float* ref);

// ---------------------------------------------------------------------------------------------------------
// Slot tables: one row per (action enum, fancy vtable) and one row per tester table.
// ---------------------------------------------------------------------------------------------------------
struct SlotRow
{
    int enumAction;
    uint64 vtable;
    uint64 expectedFn;
    uint64 origFn;
    bool installed;
};

SlotRow g_rows[] = {
#define X(enumAction, vtable, expectedFn) { static_cast<int>(enumAction), static_cast<uint64>(vtable), static_cast<uint64>(expectedFn), 0, false },
#include "InitialTestSlots.inc"
#undef X
};
constexpr size_t kRowCount = sizeof(g_rows) / sizeof(g_rows[0]);

struct TesterRow
{
    uint64 table;
    uint64 expectedFn;
    uint64 origFn;
    bool installed;
};

TesterRow g_tRows[] = {
#define X(testerTable, expectedFn) { static_cast<uint64>(testerTable), static_cast<uint64>(expectedFn), 0, false },
#include "TesterGatherSlots.inc"
#undef X
};
constexpr size_t kTesterRowCount = sizeof(g_tRows) / sizeof(g_tRows[0]);

size_t g_installedCount = 0;
size_t g_skippedCount = 0;
size_t g_tInstalledCount = 0;
size_t g_tSkippedCount = 0;
uint64 g_backOrig = 0;               // original of the back-eject tester slot
bool g_backInstalled = false;

// ---------------------------------------------------------------------------------------------------------
// Captured records. POD only: they are written from inside the hooks.
// ---------------------------------------------------------------------------------------------------------
struct Record
{
    uint64 frame;
    int enumAction;
    uint32 type;                 // kNoProbeType when the call had no probe
    float pos[3];
    float dir[3];
    float p40[3];
    float p50[3];
    bool result;
    // Action fields (arg 1)
    float dest[3];
    float destFacing[3];
    float heightDiff;
    float horizDiff;
    float actDistance;
    uint32 actionLayer;
    float fitness;
    bool isPlayer;               // arg 8 == the cached player entity
};

struct GatherProbe
{
    uint32 type;
    float pos[3];
};

struct GatherRecord
{
    uint64 frame;
    uint64 testerTable;
    uint64 tester;
    uint32 enumCount;            // size of the tester's enum list
    uint32 storedEnums;          // how many of enums[] are filled (<= kGatherEnumMax)
    int32 enums[kGatherEnumMax];
    uint32 probesBefore;         // out-array size before the original (kUnknownCount on fault)
    uint32 probesAfter;          // out-array size after the original
    uint32 newCount;             // how many of newProbes[] are filled
    GatherProbe newProbes[kGatherNewMax];
    bool isPlayer;               // a6 == the cached player entity
};

// Ring buffer shared between the hooks (writer, TryAcquire only) and the frame callback (reader).
Record g_ring[kRingSize];
uint64 g_ringWrite = 0;               // total records written, guarded by g_ringLock
uint64 g_ringRead = 0;                // total records consumed, guarded by g_ringLock
SRWLOCK g_ringLock = SRWLOCK_INIT;

GatherRecord g_gRing[kGatherRingSize];
uint64 g_gWrite = 0;                  // guarded by g_gLock
uint64 g_gRead = 0;
SRWLOCK g_gLock = SRWLOCK_INIT;

std::atomic<bool> g_capture{ true };
std::atomic<uint64> g_frame{ 0 };
std::atomic<uint64> g_dropped{ 0 };   // lock misses plus ring overwrites
std::atomic<uint32> g_faults{ 0 };
// Player entity cached by the frame callback (0 when there is no player). Hooks compare against it.
std::atomic<uint64> g_playerEntity{ 0 };
// Menu checkbox: include NPC records in the live tables and in the next session report.
std::atomic<bool> g_includeNpcs{ false };

// ---------------------------------------------------------------------------------------------------------
// Experiment C state
// ---------------------------------------------------------------------------------------------------------
std::atomic<bool> g_expC{ false };    // set by F6 / the menu; cleared on a fault
std::atomic<bool> g_expCFault{ false };
std::atomic<uint64> g_cAttempts{ 0 };
std::atomic<uint64> g_cCreated{ 0 };
std::atomic<uint64> g_cAppended{ 0 };
std::atomic<uint64> g_cRejected{ 0 };
std::atomic<uint64> g_cSkipped{ 0 };       // total skips (sum of the four below)
std::atomic<uint64> g_cSkipNoHay{ 0 };
std::atomic<uint64> g_cSkipNotBelow{ 0 };
std::atomic<uint64> g_cSkipTooFar{ 0 };
std::atomic<uint64> g_cSkipStale{ 0 };
std::atomic<uint64> g_cFaults{ 0 };

// Hay memory: haystacks are static, so every type-35 probe the player's gather calls return is remembered.
// Written on the game thread (gather calls), read by the back-eject hook (same thread) and the menu.
struct HayEntry
{
    bool valid;
    uint64 lastSeen;             // frame of the last sighting
    uint64 entity;               // hay entity (0 if the probe had no live entity)
    uint8_t probeBytes[kProbeCacheBytes];
    float pos[3];
};
HayEntry g_hayMem[kHayMemCap]{};

// One probe observed by a player gather call, copied out by the guarded collector.
struct HayObs
{
    uint8_t probeBytes[kProbeCacheBytes];
    uint64 entity;
    float pos[3];
};

struct LastAttempt
{
    uint64 frame;
    float hayPos[3];
    float playerPos[3];
    float horiz;
    float dz;
    int result;                  // 0 none, 1 rejected, 2 created without action, 3 appended, -1 fault
};
LastAttempt g_lastAttempt{};
std::atomic<uint64> g_attemptSeq{ 0 };       // bumped per attempt; the frame callback logs the newest one
uint64 g_attemptLoggedSeq = 0;
uint64 g_lastAttemptLogFrame = 0;

// The hay probe we hand to CreateIfFits. Initialised once, never freed or reset.
alignas(16) uint8_t g_hayProbe[kHayProbeBufBytes];
bool g_hayProbeReady = false;
uint64 g_hayBoundEntity = 0;         // entity the probe's ref was last bound to

// Experiment D state. g_injected is the LoF action appended by experiment C, current for 10 frames.
std::atomic<uint64> g_injected{ 0 };
std::atomic<uint64> g_injectedFrame{ 0 };
std::atomic<bool> g_forceFinal{ true };   // menu checkbox: force the final filter for the injected action
std::atomic<uint64> g_ff1Calls{ 0 };
std::atomic<uint64> g_ff1OrigTrue{ 0 };
std::atomic<uint64> g_ff1OrigFalse{ 0 };
std::atomic<uint64> g_ff1Forced{ 0 };
std::atomic<uint64> g_modeCalls{ 0 };
std::atomic<uint64> g_lastMode{ 0 };
std::atomic<uint64> g_lastModeResult{ 0 };
uint64 g_ff1Orig = 0;                // originals of the LoF slots, stored before patching
uint64 g_modeOrig = 0;
bool g_lofInstalled = false;

// Experiment E state. g_injectedEject is the action E appended, current for kInjectedCurrentFrames.
// g_ejectProbe is a second probe buffer (same bytes as the hay, type set to kEjectProbeType).
std::atomic<bool> g_expE{ true };             // default ON since 0.8.1 (F6 toggles off); cleared on a fault
std::atomic<bool> g_expEFault{ false };
std::atomic<bool> g_forceEjectFinal{ false }; // menu checkbox: force the eject final filter for our action
// Probe type given to our eject. 0.8.1 used 28 (vanilla eject probes): Arno landed ON the hay with a ground landing.
// 0.8.2 defaults to 35 (the haystack/LoF type), which the game special-cases (e.g. 0x140135A4A), to get the hay entry.
std::atomic<uint32> g_ejectProbeType{ 28 };   // 0.8.2's 35 was rejected by the eject's InitialTest (eRejected) -> back to 28
// 0.8.3: after our type-28 eject is accepted, relabel it as a haystack target (action+0xE0 and our probe's type = 35)
// so the landing code can treat it as hay. Menu checkbox, default ON.
std::atomic<bool> g_relabelHay{ true };
std::atomic<uint32> g_relabelCount{ 0 };
// 0.9.0: range widening for our eject. g_inOurEjectCreate is game-thread only: true only around our CreateIfFits(36)
// call. The range setter (fancy slot 0x64 of the eject table) runs inside that call through InitialTest.
bool g_inOurEjectCreate = false;
std::atomic<bool> g_widenEjectRanges{ true };   // menu checkbox, default ON
std::atomic<uint64> g_widenCalls{ 0 };
std::atomic<uint64> g_widenWritten{ 0 };
uint64 g_rangeOrig = 0;                          // original of the range setter (slot 0x64)
bool g_rangeInstalled = false;

// 0.10.0 experiment G: on-demand query of the game's haystack gatherer while the player is hanging.
// g_qArr is a plugin-owned SmallArray<Probe*> (layout as in the game, #pragma pack 1). It is zero-initialised once,
// its size is reset before each query, and its buffer is reused. The gatherer allocates probes for it that are NOT
// freed: a leak of about 0x80 bytes per probe, only per back-eject attempt near hay. Accepted for now.
#pragma pack(push, 1)
struct QueryArray
{
    uint64 arr;
    uint16 capFlags;
    uint16 size;
};
#pragma pack(pop)
QueryArray g_qArr{};
std::atomic<bool> g_queryOn{ true };          // menu checkbox, default ON
std::atomic<uint64> g_qCalls{ 0 };
std::atomic<uint64> g_qProbesFound{ 0 };
std::atomic<uint32> g_qLastFound{ 0 };
std::atomic<uint64> g_qFaults{ 0 };
uint64 g_qLastFrame = 0;                      // frame of the last query (rate limit)

// Experiment I (v0.13): hay registry query. Game thread only, except the atomics the menu and frame callback read.
std::atomic<bool> g_registryOn{ true };       // menu checkbox, default ON; cleared on a fault
std::atomic<bool> g_registryFault{ false };   // set on a fault, consumed by the frame callback (log)
std::atomic<uint64> g_rCalls{ 0 };
std::atomic<uint32> g_rListSize{ 0 };         // owner + 0x6A of the last query
std::atomic<uint64> g_rRemembered{ 0 };       // total HayObs sent to RememberHay
std::atomic<uint64> g_rFaults{ 0 };
uint64 g_rLastFrame = 0;                      // frame of the last query (rate limit)
std::atomic<uint64> g_eAttempts{ 0 };
std::atomic<uint64> g_eCreated{ 0 };
std::atomic<uint64> g_eRejected{ 0 };
std::atomic<uint64> g_eRemovedVanilla{ 0 };
std::atomic<uint64> g_eAppended{ 0 };
std::atomic<uint64> g_eSkipped{ 0 };
std::atomic<uint64> g_eSkipNoHay{ 0 };
std::atomic<uint64> g_eSkipNotBelow{ 0 };
std::atomic<uint64> g_eSkipTooFar{ 0 };
std::atomic<uint64> g_eSkipStale{ 0 };
std::atomic<uint64> g_eFaults{ 0 };
std::atomic<uint64> g_eAttemptSeq{ 0 };
std::atomic<uint64> g_injectedEject{ 0 };
std::atomic<uint64> g_injectedEjectFrame{ 0 };
std::atomic<uint64> g_ef1Calls{ 0 };
std::atomic<uint64> g_ef1Zero{ 0 };
std::atomic<uint64> g_ef1NonZero{ 0 };
std::atomic<uint64> g_ef1Forced{ 0 };
std::atomic<uint64> g_ef1LastAL{ 0 };
alignas(16) uint8_t g_ejectProbe[kHayProbeBufBytes];
bool g_ejectProbeReady = false;
uint64 g_ejectBoundEntity = 0;                // entity the eject probe's ref was last bound to
uint64 g_ejectFinalOrig = 0;
bool g_ejectFinalInstalled = false;
// The 7 range fields of the action that the range setter writes, as they were before (orig) and after (now) the
// widening. Order: heightMin +0x1D0, horizMax +0x1DC, distMax +0x1E4, durExt +0x224, curveMax +0x230,
// curveMin +0x234, expCurveMin +0x22C.
struct RangeSnap
{
    uint32 calls;                // range setter calls during this eject creation
    uint32 widened;              // calls where the widening was written
    bool valid;                  // the setter ran at least once
    float orig[7];
    float now[7];
};
RangeSnap g_rangeSnap{};         // written by the range hook during our create (game thread)

struct LastEject
{
    uint64 frame;
    float hayPos[3];
    float playerPos[3];
    float horiz;
    float dz;
    float dest[3];
    float heightDiff;
    float horizDiff;
    RangeSnap ranges;            // widening data captured during this attempt's CreateIfFits
    int result;                  // 0 none, 1 rejected, 2 created without action, 3 appended, -1 fault
};
LastEject g_lastEject{};
// Frame-side bookkeeping for experiment E logs.
bool g_expELastLogged = false;
uint64 g_eLoggedSeq = 0;
uint64 g_eLastLogFrame = 0;
uint64 g_lastCounterSumE = 0;
uint64 g_lastCounterLogFrameE = 0;

// ---------------------------------------------------------------------------------------------------------
// Recording session state (frame callback and menu only)
// ---------------------------------------------------------------------------------------------------------
using SessionKey = std::tuple<int, uint32, long long, long long, long long, long long, long long, long long, bool>;

struct SessionEntry
{
    Record first;
    uint64 count;
    uint64 firstFrame;
    uint64 lastFrame;
    bool anyTrue;
};

struct Sample
{
    uint64 frame;
    float pos[3];
};

using TesterProbeKey = std::tuple<uint32, long long, long long, long long, bool>;
struct TesterProbeEntry
{
    uint32 type;
    float pos[3];
    uint64 count;
    bool isPlayer;
};

struct TesterSess
{
    uint64 calls;
    uint32 enumCount;
    uint32 storedEnums;
    bool lastIsPlayer;           // isPlayer of the last call stored
    int32 enums[kGatherEnumMax];
    std::map<TesterProbeKey, TesterProbeEntry> probes;
    uint64 probesDropped;
};

bool g_recording = false;
bool g_sessionIncludeNpcs = false;    // copied from g_includeNpcs at session start
uint64 g_sessionStartFrame = 0;
uint64 g_sessionEndFrame = 0;
uint64 g_sessionRecords = 0;
uint64 g_sessionOverflow = 0;         // new keys refused because the store is full
std::map<SessionKey, SessionEntry> g_session;
std::map<std::pair<int, uint32>, uint64> g_sessionCounts;
std::map<uint64, TesterSess> g_tSession;
std::vector<Sample> g_samples;
bool g_lastSessionValid = false;      // a report was written and the store holds its data

// ---------------------------------------------------------------------------------------------------------
// Frame-side state (only touched from the frame callback and the menu)
// ---------------------------------------------------------------------------------------------------------
std::ofstream g_log;
std::vector<Record> g_drain;          // scratch for the ring copy, reserved once
std::vector<Record> g_recent;         // records from the last kRecentFrames frames
std::vector<GatherRecord> g_gDrain;   // scratch for the gather ring copy
std::vector<GatherRecord> g_gatherRecent;
bool g_hasPlayer = false;
float g_playerPos[3] = { 0.0f, 0.0f, 0.0f };
bool g_f7Down = false;
bool g_f6Down = false;
bool g_faultLogged = false;
bool g_expCLastLogged = false;
uint64 g_lastCounterLogFrame = 0;
uint64 g_lastCounterSum = 0;
ULONGLONG g_msgUntilMs = 0;           // status message deadline (GetTickCount64)
const char* g_msgText = "";
char g_typeText[128] = "35,36,39";
float g_maxDistance = kDefaultMaxDistance;

// ---------------------------------------------------------------------------------------------------------
// Logging (file only; flushed every line)
// ---------------------------------------------------------------------------------------------------------
void LogLine(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log.is_open()) { g_log << buf << "\n"; g_log.flush(); }
}

const char* ActionName(int e)
{
    switch (e) {
    case 36:  return "eject";
    case 40:  return "dive";
    case 42:  return "lofWater";
    case 65:  return "wallClimb";
    case 69:  return "dismount";
    case 70:  return "wallToHangDown";
    case 79:  return "backEject";
    case 112: return "leapOfFaith";
    case 114: return "hayExit";
    default:  return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------
// SEH helpers. Plain functions with no C++ objects, so __try is allowed.
// ---------------------------------------------------------------------------------------------------------
bool ReadSlotGuarded(uint64 entry, uint32& nameHash, uint32& idx, uint64& fn)
{
    __try {
        nameHash = *reinterpret_cast<const uint32*>(entry);
        idx = *reinterpret_cast<const uint32*>(entry + 4);
        fn = *reinterpret_cast<const uint64*>(entry + kSlotFnOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool PatchSlotGuarded(uint64 addr, uint64 value)
{
    __try {
        DWORD oldProt = 0;
        if (!VirtualProtect(reinterpret_cast<LPVOID>(addr), 8, PAGE_READWRITE, &oldProt)) return false;
        *reinterpret_cast<volatile uint64*>(addr) = value;
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(addr), 8, oldProt, &tmp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Copies the action fields (arg 1) and, if the probe pointer is non-null, the probe fields (arg 7) into a record.
bool CaptureGuarded(uint64 action, uint64 probe, Record& r)
{
    __try {
        const uint8_t* a = reinterpret_cast<const uint8_t*>(action);
        memcpy(r.dest, a + kActionDestOffset, 12);
        memcpy(r.destFacing, a + kActionDestFacingOffset, 12);
        r.heightDiff = *reinterpret_cast<const float*>(a + kActionHeightDiffOffset);
        r.horizDiff = *reinterpret_cast<const float*>(a + kActionHorizDiffOffset);
        r.actDistance = *reinterpret_cast<const float*>(a + kActionDistanceOffset);
        r.actionLayer = *reinterpret_cast<const uint32*>(a + kActionLayerOffset);
        r.fitness = *reinterpret_cast<const float*>(a + kActionFitnessOffset);
        if (probe) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(probe);
            r.type = *reinterpret_cast<const uint32*>(p + kProbeTypeOffset);
            memcpy(r.pos, p + 0x00, 12);
            memcpy(r.dir, p + kProbeDirOffset, 12);
            memcpy(r.p40, p + kProbePos40Offset, 12);
            memcpy(r.p50, p + kProbePos50Offset, 12);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Out-array size (SmallArray<Probe*>) or kUnknownCount on fault.
uint32 ReadProbeCountGuarded(uint64 outProbes)
{
    __try {
        if (!outProbes) return kUnknownCount;
        return *reinterpret_cast<const uint16*>(outProbes + kSmallArraySizeOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return kUnknownCount; }
}

// Fills g with the tester's enum list and the probes gathered by this call (indices [before, after) of outProbes).
bool CaptureGatherGuarded(uint64 tester, uint64 outProbes, uint32 before, uint32 after, GatherRecord& g)
{
    __try {
        const uint64 enumArr = *reinterpret_cast<const uint64*>(tester + kTesterEnumArrOffset);
        const uint32 enumSize = *reinterpret_cast<const uint16*>(tester + kTesterEnumSizeOffset);
        g.enumCount = enumSize;
        const uint32 n = enumSize < kGatherEnumMax ? enumSize : static_cast<uint32>(kGatherEnumMax);
        if (enumArr) {
            const int32* list = reinterpret_cast<const int32*>(enumArr);
            for (uint32 i = 0; i < n; ++i) g.enums[i] = list[i];
        }
        g.storedEnums = n;

        if (before != kUnknownCount && after != kUnknownCount && after > before && outProbes) {
            const uint64 probeArr = *reinterpret_cast<const uint64*>(outProbes);
            const uint32 k = (after - before) < kGatherNewMax ? (after - before) : static_cast<uint32>(kGatherNewMax);
            const uint64* probes = reinterpret_cast<const uint64*>(probeArr);
            for (uint32 i = 0; i < k; ++i) {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(probes[before + i]);
                if (!p) continue;
                GatherProbe& np = g.newProbes[g.newCount++];
                np.type = *reinterpret_cast<const uint32*>(p + kProbeTypeOffset);
                memcpy(np.pos, p, 12);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Reads the player position and entity. Only called when World exists. entityOut is 0 when there is no player.
// v0.16: faulted is true only when the read itself faulted (no player is not a fault).
bool GetPlayerPosGuarded(float out[3], uint64& entityOut, bool& faulted)
{
    entityOut = 0;
    faulted = false;
    __try {
        World* world = World::GetSingleton();
        Entity* player = world ? ACU::GetPlayer() : nullptr;
        if (!player) return false;
        const auto& p = player->GetPosition();
        out[0] = p.x;
        out[1] = p.y;
        out[2] = p.z;
        entityOut = reinterpret_cast<uint64>(player);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { entityOut = 0; faulted = true; return false; }
}

// Position of an entity given by address (the player's entity, passed to the hooks). False on fault.
bool ReadEntityPosGuarded(uint64 entity, float out[3])
{
    __try {
        const auto& p = reinterpret_cast<Entity*>(entity)->GetPosition();
        out[0] = p.x;
        out[1] = p.y;
        out[2] = p.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadU64Guarded(uint64 addr, uint64& out)
{
    __try {
        out = *reinterpret_cast<const uint64*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Copies the type-35 probes this gather call added (up to kHayObsMax) into obs. Pure reads, guarded.
// count receives how many were copied; returns false on fault (nothing is copied then).
bool CollectHayGuarded(uint64 outProbes, uint32 before, uint32 after, HayObs* obs, size_t& count)
{
    count = 0;
    if (!outProbes || before == kUnknownCount || after == kUnknownCount || after <= before) return true;
    __try {
        const uint64 probeArr = *reinterpret_cast<const uint64*>(outProbes);
        const uint64* probes = reinterpret_cast<const uint64*>(probeArr);
        const uint32 end = after - before < kGatherScanMax ? after : static_cast<uint32>(before + kGatherScanMax);
        for (uint32 i = before; i < end && count < kHayObsMax; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(probes[i]);
            if (!p) continue;
            if (*reinterpret_cast<const uint32*>(p + kProbeTypeOffset) != kProbeTypeHay) continue;
            HayObs& o = obs[count];
            memcpy(o.probeBytes, p, kProbeCacheBytes);
            memcpy(o.pos, p, 12);
            const uint64 block = *reinterpret_cast<const uint64*>(p + kProbeBlockOffset);
            o.entity = 0;
            if (block) {
                const uint64 manObj = *reinterpret_cast<const uint64*>(block + kBlockObjOffset);
                const uint32 flags = *reinterpret_cast<const uint32*>(block + kBlockFlagsOffset);
                o.entity = (flags & kBlockStrongFlag) ? manObj : 0;
            }
            ++count;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; return false; }
}


// Makes g_hayProbe carry the cached hay bytes and binds its shared ref to the hay entity when that changed.
bool PrepareHayProbeGuarded(const HayEntry& h)
{
    __try {
        memcpy(g_hayProbe, h.probeBytes, kProbeCacheBytes);
        // A remembered entity may have been unloaded since it was seen. Bind only if it is still alive: its own
        // SharedBlock (entity+0x130, as read by 0x140159940) must point back at it with the strong/alive flag set.
        bool alive = false;
        if (h.entity) {
            const uint64 block = *reinterpret_cast<const uint64*>(h.entity + 0x130);
            alive = block
                && *reinterpret_cast<const uint64*>(block + kBlockObjOffset) == h.entity
                && (*reinterpret_cast<const uint32*>(block + kBlockFlagsOffset) & kBlockStrongFlag) != 0;
        }
        if (alive && h.entity != g_hayBoundEntity) {
            reinterpret_cast<InitPlayerRefFn>(kInitPlayerRefAddr)(reinterpret_cast<uint64>(g_hayProbe + kProbeBlockOffset), h.entity);
            g_hayBoundEntity = h.entity;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Calls CreateIfFits(enumAction, ...) with the given probe (experiment C: the hay probe, enum 112;
// experiment E: the eject probe, enum 36). Returns 0 on success, -1 on fault.
int CreateHayActionGuarded(uint64 tester, uint64 a2, uint64 a3, uint64 a5bits, uint64 a6, uint64 a7,
                           uint64 probeAddr, int enumAction, uint64& outAction, bool& created)
{
    __try {
        float f5 = 0.0f;
        memcpy(&f5, &a5bits, sizeof(f5));
        const uint64 player = *reinterpret_cast<const uint64*>(tester + kTesterPlayerOffset);
        const uint64 prev = *reinterpret_cast<const uint64*>(tester + kTesterPrevActionOffset);
        const float f12 = *reinterpret_cast<const float*>(tester + kTesterStateFloat1Offset);
        const float f13 = *reinterpret_cast<const float*>(tester + kTesterStateFloat2Offset);
        outAction = 0;
        const char ret = reinterpret_cast<CreateIfFitsFn>(kCreateIfFitsAddr)(
            enumAction, a2, a3, tester + kTesterMoveVecOffset, f5,
            static_cast<int>(a6), static_cast<char>(a7), probeAddr,
            player, prev, &outAction, f12, f13);
        created = ret != 0;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Appends the created action to the back-eject's action array. False on fault.
bool AppendActionGuarded(uint64 array, uint64 item)
{
    __try {
        reinterpret_cast<SmallArrayAppendFn>(kSmallArrayAppendAddr)(array, item, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------------------------------------
// Hooks (run on the game's thread)
// ---------------------------------------------------------------------------------------------------------
// TryAcquire only: a missed lock drops the record instead of blocking the game.
bool TryPush(const Record& r)
{
    if (!TryAcquireSRWLockExclusive(&g_ringLock)) return false;
    g_ring[g_ringWrite % kRingSize] = r;
    ++g_ringWrite;
    ReleaseSRWLockExclusive(&g_ringLock);
    return true;
}

bool TryPushGather(const GatherRecord& g)
{
    if (!TryAcquireSRWLockExclusive(&g_gLock)) return false;
    g_gRing[g_gWrite % kGatherRingSize] = g;
    ++g_gWrite;
    ReleaseSRWLockExclusive(&g_gLock);
    return true;
}

// Shared body for every action row. `row` is fixed per thunk, so it always names the right original function.
bool InitialTestImpl(size_t row, uint64 a1, uint64 a2, uint64 a3, uint64 a4, uint64 a5,
                     uint64 a6, uint64 a7, uint64 a8, uint64 a9)
{
    // Always call the original first, with all arguments unchanged, and return its result unchanged.
    const bool result = reinterpret_cast<InitialTestFn>(g_rows[row].origFn)(a1, a2, a3, a4, a5, a6, a7, a8, a9);

    if (g_capture.load(std::memory_order_relaxed)) {
        Record r{};
        r.frame = g_frame.load(std::memory_order_relaxed);
        r.enumAction = g_rows[row].enumAction;
        r.result = result;
        r.type = kNoProbeType;
        r.isPlayer = a8 != 0 && a8 == g_playerEntity.load(std::memory_order_relaxed);
        if (!CaptureGuarded(a1, a7, r)) {
            // The frame callback logs once when capture is turned off.
            if (++g_faults >= kMaxFaults) g_capture.store(false);
        } else if (!TryPush(r)) {
            ++g_dropped;
        }
    }
    return result;
}

// Remembers one sighting: refreshes the entry within kHayMergeDist, else takes a free slot, else evicts the oldest.
void RememberHay(const HayObs& o, uint64 frame)
{
    int target = -1;
    for (size_t i = 0; i < kHayMemCap && target < 0; ++i) {
        const HayEntry& e = g_hayMem[i];
        if (!e.valid) continue;
        const float dx = e.pos[0] - o.pos[0];
        const float dy = e.pos[1] - o.pos[1];
        const float dz = e.pos[2] - o.pos[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) <= kHayMergeDist) target = static_cast<int>(i);
    }
    if (target < 0) {
        for (size_t i = 0; i < kHayMemCap && target < 0; ++i)
            if (!g_hayMem[i].valid) target = static_cast<int>(i);
    }
    if (target < 0) {
        size_t oldest = 0;
        for (size_t i = 1; i < kHayMemCap; ++i)
            if (g_hayMem[i].lastSeen < g_hayMem[oldest].lastSeen) oldest = i;
        target = static_cast<int>(oldest);
    }
    HayEntry& e = g_hayMem[target];
    e.valid = true;
    e.lastSeen = frame;
    e.entity = o.entity;
    memcpy(e.probeBytes, o.probeBytes, kProbeCacheBytes);
    memcpy(e.pos, o.pos, 12);
}

// Hay memory update for the player's gather calls. Game thread only.
void UpdateHayCache(uint64 outProbes, uint32 before, uint32 after)
{
    HayObs obs[kHayObsMax];
    size_t n = 0;
    if (!CollectHayGuarded(outProbes, before, after, obs, n)) { ++g_faults; return; }
    const uint64 frame = g_frame.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) RememberHay(obs[i], frame);
}

// Shared body for every tester row.
uint64 GatherImpl(size_t row, uint64 tester, uint64 posPtr, uint64 r8, float f4, uint64 s5,
                  uint64 playerEntity, uint64 outProbes)
{
    const bool isPlayer = playerEntity != 0 && playerEntity == g_playerEntity.load(std::memory_order_relaxed);
    const uint32 before = ReadProbeCountGuarded(outProbes);

    const uint64 result = reinterpret_cast<GatherFn>(g_tRows[row].origFn)(tester, posPtr, r8, f4, s5, playerEntity, outProbes);

    const uint32 after = ReadProbeCountGuarded(outProbes);
    if (isPlayer) UpdateHayCache(outProbes, before, after);

    if (g_capture.load(std::memory_order_relaxed)) {
        GatherRecord g{};
        g.frame = g_frame.load(std::memory_order_relaxed);
        g.testerTable = g_tRows[row].table;
        g.tester = tester;
        g.probesBefore = before;
        g.probesAfter = after;
        g.isPlayer = isPlayer;
        if (!CaptureGatherGuarded(tester, outProbes, before, after, g)) {
            if (++g_faults >= kMaxFaults) g_capture.store(false);
        } else if (!TryPushGather(g)) {
            ++g_dropped;
        }
    }
    return result;
}

// ---- Experiment E: the back eject (enum 36) itself lands on the hay ----------------------------------------------

// ---- Experiment G: on-demand query of the game's haystack gatherer -----------------------------------------------

// Calls the gatherer exactly as the scan does: (tester, tester+0x90, tester+0x50, float, 150.0f, player, out array).
// Guarded. The float argument is the low 32 bits of the back-eject thunk's arg5, as in the game's own call.
bool QueryGathererGuarded(uint64 tester, uint64 a5bits)
{
    __try {
        float f4 = 0.0f;
        memcpy(&f4, &a5bits, sizeof(f4));
        float r = kGathererRadiusArg;
        uint32 radiusBits = 0;
        memcpy(&radiusBits, &r, sizeof(radiusBits));
        const uint64 player = *reinterpret_cast<const uint64*>(tester + kTesterPlayerOffset);
        reinterpret_cast<GatherFn>(kGathererAddr)(tester, tester + kGathererPosOffset, tester + kGathererVecOffset, f4,
                                                  static_cast<uint64>(radiusBits), player,
                                                  reinterpret_cast<uint64>(&g_qArr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// One query, at most once per kQueryInterval frames. Every probe the gatherer returns goes through the hay memory.
void QueryHaystackOnDemand(uint64 tester, uint64 a5bits, uint64 frame)
{
    if (!g_queryOn.load(std::memory_order_relaxed)) return;
    if (g_qLastFrame != 0 && frame - g_qLastFrame < kQueryInterval) return;
    g_qLastFrame = frame;

    g_qArr.size = 0;                  // keep arr and capacity: the buffer is reused
    ++g_qCalls;
    if (!QueryGathererGuarded(tester, a5bits)) {
        ++g_qFaults;
        g_queryOn.store(false);
        return;
    }
    const uint32 n = g_qArr.size;
    g_qLastFound.store(n);
    g_qProbesFound += n;

    for (uint32 start = 0; start < n; start += static_cast<uint32>(kHayObsMax)) {
        const uint32 end = (n - start) < kHayObsMax ? n : start + static_cast<uint32>(kHayObsMax);
        HayObs obs[kHayObsMax];
        size_t cnt = 0;
        if (!CollectHayGuarded(reinterpret_cast<uint64>(&g_qArr), start, end, obs, cnt)) {
            ++g_qFaults;
            g_queryOn.store(false);
            return;
        }
        for (size_t i = 0; i < cnt; ++i) RememberHay(obs[i], frame);
    }
}

// ---- Experiment I (v0.13): hay registry query (no facing filter) -------------------------------------------------

// Reads the game's leap-of-faith landing targets for the player: registry lookup owner = 0x140D61A50(*(player + 0x60)),
// then for each element (up to kRegistryMaxElems) its entity (elem + 0x10) and its landing point
// p = 0x140CF5A50(elem, out16, tester + 0x90). No facing filter, unlike the gatherer of experiment G.
// Writes at most cap HayObs, built with the same layout as a gathered type-35 probe (bytes 0x00..0x5F plus the entity).
// Selection: the first cap targets (in list order) that are within kRegistryRadius metres horizontally of the player.
// listSize receives owner + 0x6A. On a fault count is 0 and the function returns false. POD only, guarded.
bool QueryHayRegistryGuarded(uint64 tester, HayObs* out, size_t cap, size_t& count, uint32& listSize)
{
    count = 0;
    listSize = 0;
    __try {
        const uint64 player = *reinterpret_cast<const uint64*>(tester + kTesterPlayerOffset);
        if (!player) return true;
        float pp[3] = { 0.0f, 0.0f, 0.0f };
        if (!ReadEntityPosGuarded(player, pp)) return false;
        const uint64 key = *reinterpret_cast<const uint64*>(player + kRegistryKeyOffset);
        if (!key) return true;
        const uint64 owner = reinterpret_cast<RegistryLookupFn>(kRegistryLookupAddr)(key);
        if (!owner) return true;
        const uint64 list = *reinterpret_cast<const uint64*>(owner + kRegistryListOffset);
        const uint16 n = *reinterpret_cast<const uint16*>(owner + kRegistryCountOffset);
        listSize = n;
        if (!list) return true;
        const uint64* elems = reinterpret_cast<const uint64*>(list);
        const uint32 lim = n < kRegistryMaxElems ? n : kRegistryMaxElems;
        const float* ref = reinterpret_cast<const float*>(tester + kGathererPosOffset);
        for (uint32 i = 0; i < lim && count < cap; ++i) {
            const uint64 elem = elems[i];
            if (!elem) continue;
            const uint64 entity = *reinterpret_cast<const uint64*>(elem + kRegistryEntityOffset);
            if (!entity) continue;
            alignas(16) float outp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            const float* p = reinterpret_cast<RegistryLandFn>(kRegistryLandAddr)(elem, outp, ref);
            if (!p) continue;
            float v[4];
            memcpy(v, p, 16);
            const float dx = v[0] - pp[0];
            const float dy = v[1] - pp[1];
            if (std::sqrt(dx * dx + dy * dy) > kRegistryRadius) continue;

            // The probe layout the game uses for a gathered type-35 probe (see CollectHayGuarded and the hay cache).
            HayObs& o = out[count];
            uint8_t* b = o.probeBytes;
            memset(b, 0, kProbeCacheBytes);
            memcpy(b + 0x00, v, 16);                    // pos (x, y, z, w as returned)
            const float dir[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
            memcpy(b + kProbeDirOffset, dir, 16);       // dir (0, 0, 1, 0)
            const uint32 type = kProbeTypeHay;
            memcpy(b + kProbeTypeOffset, &type, 4);     // type 35 (+0x30..0x37 stay zero)
            memcpy(b + kProbePos40Offset, v, 16);       // pos copy at +0x40
            memcpy(b + kProbePos50Offset, v, 16);       // pos copy at +0x50
            memcpy(o.pos, v, 12);
            o.entity = entity;
            ++count;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { count = 0; return false; }
}

// One registry query, at most once per kQueryInterval frames (own rate limit). Every target it returns goes through
// the hay memory. A fault turns the query off and sets g_registryFault for the frame callback to log.
void QueryHayRegistry(uint64 tester, uint64 frame)
{
    if (!g_registryOn.load(std::memory_order_relaxed)) return;
    if (g_rLastFrame != 0 && frame - g_rLastFrame < kQueryInterval) return;
    g_rLastFrame = frame;

    HayObs obs[kRegistryCap];
    size_t n = 0;
    uint32 listSize = 0;
    ++g_rCalls;
    if (!QueryHayRegistryGuarded(tester, obs, kRegistryCap, n, listSize)) {
        ++g_rFaults;
        g_registryOn.store(false);
        g_registryFault.store(true);
        return;
    }
    g_rListSize.store(listSize);
    for (size_t i = 0; i < n; ++i) RememberHay(obs[i], frame);
    g_rRemembered += static_cast<uint64>(n);
}

// Picks the hay for an attempt from the memory (rules as in experiment C). Returns 0 with out/horiz set, or a skip
// reason: 1 noHay, 2 notBelow, 3 tooFar, 4 stale.
int ChooseHay(const float pp[3], uint64 frame, HayEntry& out, float& horiz)
{
    int chosen = -1;
    float chosenH = 1e30f;
    int nearest = -1;
    float nearestH = 1e30f;
    bool anyValid = false;
    for (size_t i = 0; i < kHayMemCap; ++i) {
        const HayEntry& e = g_hayMem[i];
        if (!e.valid) continue;
        anyValid = true;
        const float dx = e.pos[0] - pp[0];
        const float dy = e.pos[1] - pp[1];
        const float h = std::sqrt(dx * dx + dy * dy);
        if (h < nearestH) { nearestH = h; nearest = static_cast<int>(i); }
        const bool below = pp[2] - e.pos[2] >= kHayMinBelow;
        const bool inReach = h <= kHayMaxHoriz;
        const bool fresh = frame - e.lastSeen <= kHayMaxAgeFrames;
        if (below && inReach && fresh && h < chosenH) { chosen = static_cast<int>(i); chosenH = h; }
    }
    if (chosen >= 0) {
        out = g_hayMem[chosen];
        horiz = chosenH;
        return 0;
    }
    if (!anyValid) return 1;
    const HayEntry& e = g_hayMem[nearest];
    if (pp[2] - e.pos[2] < kHayMinBelow) return 2;
    if (nearestH > kHayMaxHoriz) return 3;
    return 4;
}

// Copies the hay bytes into g_ejectProbe, sets its type to kEjectProbeType, and binds its ref to the hay entity when
// that changed (tracked separately from the hay probe's binding).
bool PrepareEjectProbeGuarded(const HayEntry& h)
{
    __try {
        memcpy(g_ejectProbe, h.probeBytes, kProbeCacheBytes);
        *reinterpret_cast<uint32*>(g_ejectProbe + kProbeTypeOffset) = g_ejectProbeType.load(std::memory_order_relaxed);
        // Same alive check as PrepareHayProbeGuarded: bind only if the entity's SharedBlock points back at it, alive.
        bool alive = false;
        if (h.entity) {
            const uint64 block = *reinterpret_cast<const uint64*>(h.entity + 0x130);
            alive = block
                && *reinterpret_cast<const uint64*>(block + kBlockObjOffset) == h.entity
                && (*reinterpret_cast<const uint32*>(block + kBlockFlagsOffset) & kBlockStrongFlag) != 0;
        }
        if (alive && h.entity != g_ejectBoundEntity) {
            reinterpret_cast<InitPlayerRefFn>(kInitPlayerRefAddr)(reinterpret_cast<uint64>(g_ejectProbe + kProbeBlockOffset), h.entity);
            g_ejectBoundEntity = h.entity;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Marks our accepted eject (and the probe it was built from) as a haystack target: layer/type 35.
bool RelabelHayGuarded(uint64 action)
{
    __try {
        *reinterpret_cast<uint32*>(action + kActionLayerOffset) = kProbeTypeHay;
        *reinterpret_cast<uint32*>(g_ejectProbe + kProbeTypeOffset) = kProbeTypeHay;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadActionFieldsGuarded(uint64 action, float dest[3], float& heightDiff, float& horizDiff)
{
    __try {
        const uint8_t* a = reinterpret_cast<const uint8_t*>(action);
        memcpy(dest, a + kActionDestOffset, 12);
        heightDiff = *reinterpret_cast<const float*>(a + kActionHeightDiffOffset);
        horizDiff = *reinterpret_cast<const float*>(a + kActionHorizDiffOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Removes the vanilla candidates whose enum is 36 or 50 from the actions array (last to first). The enum comes from
// the action's fancy slot 0x45. Removed actions are NOT freed: a small leak, accepted for this experiment.
// Returns -1 on fault.
int RemoveVanillaGuarded(uint64 array, uint32& removed)
{
    __try {
        const uint32 size = *reinterpret_cast<const uint16*>(array + kSmallArraySizeOffset);
        for (int i = static_cast<int>(size) - 1; i >= 0; --i) {
            const uint64 arr = *reinterpret_cast<const uint64*>(array);
            const uint64 action = *reinterpret_cast<const uint64*>(arr + 8 * static_cast<uint64>(i));
            if (!action) continue;
            const uint64 fancy = *reinterpret_cast<const uint64*>(action + 8);
            const uint64 fn = *reinterpret_cast<const uint64*>(fancy + kEnumFancySlotIdx * kSlotStride + kSlotFnOffset);
            const int e = reinterpret_cast<EnumFn>(fn)(action);
            if (e == kEnumEject || e == kEnumVanillaAlt) {
                reinterpret_cast<SmallArrayRemoveFn>(kSmallArrayRemoveAddr)(array, i, 8);
                ++removed;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

void EjectFault()
{
    g_inOurEjectCreate = false;      // the fault path must never leave the range flag set
    g_expE.store(false);
    g_expEFault.store(true);
    ++g_eFaults;
}

void FinishEject(const LastEject& le)
{
    g_lastEject = le;
    g_eAttemptSeq.fetch_add(1, std::memory_order_relaxed);
}

#include "OriginsAnim.inl"   // H) Origins backflip (v0.11): origins::Load / ArmFromGameThread / Frame / Menu

// Experiment E, run after the original back-eject call. Player's tester only.
void BackEjectEject(uint64 tester, uint64 a2, uint64 a3, uint64 a5, uint64 a6, uint64 a7, uint64 a9)
{
    uint64 testerPlayer = 0;
    if (!ReadU64Guarded(tester + kTesterPlayerOffset, testerPlayer)) { EjectFault(); return; }
    const uint64 cachedPlayer = g_playerEntity.load(std::memory_order_relaxed);
    if (cachedPlayer == 0 || testerPlayer != cachedPlayer) return;

    float pp[3];
    if (!ReadEntityPosGuarded(cachedPlayer, pp)) { EjectFault(); return; }
    const uint64 frame = g_frame.load(std::memory_order_relaxed);

    // 0.10.0: ask the game's gatherer first, so a haystack we have not seen yet is in memory before we choose.
    QueryHaystackOnDemand(tester, a5, frame);
    QueryHayRegistry(tester, frame);      // v0.13 (experiment I): landing targets without the facing filter

    HayEntry hay{};
    float horiz = 0.0f;
    const int reason = ChooseHay(pp, frame, hay, horiz);
    if (reason != 0) {
        ++g_eSkipped;
        if (reason == 1) ++g_eSkipNoHay;
        else if (reason == 2) ++g_eSkipNotBelow;
        else if (reason == 3) ++g_eSkipTooFar;
        else ++g_eSkipStale;
        return;
    }

    ++g_eAttempts;
    LastEject le{};
    le.frame = frame;
    memcpy(le.hayPos, hay.pos, sizeof(le.hayPos));
    memcpy(le.playerPos, pp, sizeof(le.playerPos));
    le.horiz = horiz;
    le.dz = pp[2] - hay.pos[2];

    if (!g_ejectProbeReady) {
        memset(g_ejectProbe, 0, sizeof(g_ejectProbe));
        *reinterpret_cast<uint64*>(g_ejectProbe + kProbeBlockOffset) = kEmptySharedBlock;
        g_ejectProbeReady = true;
    }
    if (!PrepareEjectProbeGuarded(hay)) { le.result = -1; FinishEject(le); EjectFault(); return; }

    uint64 out = 0;
    bool created = false;
    // Experiment F: the range setter writes only inside this call.
    g_rangeSnap = RangeSnap{};
    g_inOurEjectCreate = true;
    const int createRet = CreateHayActionGuarded(tester, a2, a3, a5, a6, a7, reinterpret_cast<uint64>(g_ejectProbe),
                                                 kEnumEject, out, created);
    g_inOurEjectCreate = false;
    le.ranges = g_rangeSnap;
    if (createRet != 0) {
        le.result = -1; FinishEject(le); EjectFault(); return;
    }
    if (!created) {
        // CreateIfFits said no: leave the vanilla candidates alone.
        ++g_eRejected;
        le.result = 1;
        FinishEject(le);
        return;
    }
    ++g_eCreated;
    if (!out) { le.result = 2; FinishEject(le); return; }

    ReadActionFieldsGuarded(out, le.dest, le.heightDiff, le.horizDiff);   // informational; failure is ignored
    if (g_relabelHay.load(std::memory_order_relaxed)) {
        if (RelabelHayGuarded(out)) ++g_relabelCount; else { le.result = -1; FinishEject(le); EjectFault(); return; }
    }

    uint32 removed = 0;
    if (RemoveVanillaGuarded(a9, removed) != 0) { le.result = -1; FinishEject(le); EjectFault(); return; }
    g_eRemovedVanilla += removed;
    if (!AppendActionGuarded(a9, out)) { le.result = -1; FinishEject(le); EjectFault(); return; }
    ++g_eAppended;
    origins::ArmFromGameThread(le.dz, le.horiz);   // H) arms the Origins variant for this drop now, before the eject clip can start
    g_injectedEject.store(out);
    g_injectedEjectFrame.store(frame);
    le.result = 3;
    FinishEject(le);
}

bool IsEjectCurrent(uint64 action)
{
    const uint64 inj = g_injectedEject.load(std::memory_order_relaxed);
    if (!inj || action != inj) return false;
    return g_frame.load(std::memory_order_relaxed) - g_injectedEjectFrame.load(std::memory_order_relaxed) <= kInjectedCurrentFrames;
}

// Eject FinalFilter1 (slot 0x76 of the eject table). Observation only, unless the force option is on.
// Only our action is affected. The AL byte of the original result is counted.
uint64 __fastcall EjectFinalThunk(uint64 a1, uint64 a2, uint64 a3, uint64 a4)
{
    const uint64 orig = reinterpret_cast<FinalFilterFn>(g_ejectFinalOrig)(a1, a2, a3, a4);
    if (!IsEjectCurrent(a1)) return orig;
    ++g_ef1Calls;
    const uint64 al = orig & 0xFFu;
    g_ef1LastAL.store(al);
    if (al) ++g_ef1NonZero; else ++g_ef1Zero;
    if (g_forceEjectFinal.load(std::memory_order_relaxed)) {
        ++g_ef1Forced;
        return 1;
    }
    return orig;
}

// ---- Experiment F: widen the eject's range fields while our eject is created --------------------------------------

// Reads the 7 range fields, then (if widen) writes the widened values. Plain memory access, guarded.
bool WidenRangesGuarded(uint64 action, bool widen, RangeSnap& s)
{
    constexpr uint64 offs[7] = { kRangeHeightMinOff, kRangeHorizMaxOff, kRangeDistMaxOff, kRangeDurExtOff,
                                 kRangeCurveMaxOff, kRangeCurveMinOff, kRangeExpCurveMinOff };
    constexpr float vals[7] = { -150.0f, 30.0f, 160.0f, 10.0f, 100.0f, -100.0f, -100.0f };
    __try {
        uint8_t* a = reinterpret_cast<uint8_t*>(action);
        for (int i = 0; i < 7; ++i) s.orig[i] = *reinterpret_cast<float*>(a + offs[i]);
        for (int i = 0; i < 7; ++i) {
            if (widen) *reinterpret_cast<float*>(a + offs[i]) = vals[i];
            s.now[i] = widen ? vals[i] : s.orig[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Range setter (slot 0x64). Original first; widening only while our CreateIfFits(36) is running. The 0x1D4 field
// is left untouched on purpose. The widened fields are written to the action passed in (arg 1).
uint64 __fastcall EjectRangeThunk(uint64 action, uint64 a2, uint64 mode)
{
    const uint64 orig = reinterpret_cast<RangeFn>(g_rangeOrig)(action, a2, mode);
    if (!g_inOurEjectCreate) return orig;
    ++g_widenCalls;
    RangeSnap& s = g_rangeSnap;
    ++s.calls;
    const bool widen = g_widenEjectRanges.load(std::memory_order_relaxed);
    if (!WidenRangesGuarded(action, widen, s)) {
        // Fault: stop widening (the snapshot shows what was seen). The frame callback logs it.
        g_widenEjectRanges.store(false);
        ++g_eFaults;
        return orig;
    }
    s.valid = true;
    if (widen) { ++s.widened; ++g_widenWritten; }
    return orig;
}

// Experiment C, run after the original back-eject call. Player's tester only. See the top comment.
void FaultC()
{
    g_expC.store(false);
    g_expCFault.store(true);
    ++g_cFaults;
}

void FinishAttempt(const LastAttempt& la)
{
    g_lastAttempt = la;
    g_attemptSeq.fetch_add(1, std::memory_order_relaxed);
}

void BackEjectInject(uint64 tester, uint64 a2, uint64 a3, uint64 a5, uint64 a6, uint64 a7, uint64 a9)
{
    uint64 testerPlayer = 0;
    if (!ReadU64Guarded(tester + kTesterPlayerOffset, testerPlayer)) { FaultC(); return; }
    const uint64 cachedPlayer = g_playerEntity.load(std::memory_order_relaxed);
    if (cachedPlayer == 0 || testerPlayer != cachedPlayer) return;     // not the player's tester

    float pp[3];
    if (!ReadEntityPosGuarded(cachedPlayer, pp)) { FaultC(); return; }
    const uint64 frame = g_frame.load(std::memory_order_relaxed);

    // Choose from the hay memory: at least kHayMinBelow below the player, at most kHayMaxHoriz away horizontally,
    // seen within kHayMaxAgeFrames; the smallest horizontal distance wins.
    int chosen = -1;
    float chosenH = 1e30f;
    int nearest = -1;
    float nearestH = 1e30f;
    bool anyValid = false;
    for (size_t i = 0; i < kHayMemCap; ++i) {
        const HayEntry& e = g_hayMem[i];
        if (!e.valid) continue;
        anyValid = true;
        const float dx = e.pos[0] - pp[0];
        const float dy = e.pos[1] - pp[1];
        const float h = std::sqrt(dx * dx + dy * dy);
        if (h < nearestH) { nearestH = h; nearest = static_cast<int>(i); }
        const bool below = pp[2] - e.pos[2] >= kHayMinBelow;
        const bool inReach = h <= kHayMaxHoriz;
        const bool fresh = frame - e.lastSeen <= kHayMaxAgeFrames;
        if (below && inReach && fresh && h < chosenH) { chosen = static_cast<int>(i); chosenH = h; }
    }
    if (chosen < 0) {
        // Skip reason: from the entry nearest horizontally (none at all -> noHay).
        ++g_cSkipped;
        if (!anyValid) {
            ++g_cSkipNoHay;
        } else {
            const HayEntry& e = g_hayMem[nearest];
            if (pp[2] - e.pos[2] < kHayMinBelow) ++g_cSkipNotBelow;
            else if (nearestH > kHayMaxHoriz) ++g_cSkipTooFar;
            else ++g_cSkipStale;
        }
        return;
    }

    const HayEntry hay = g_hayMem[chosen];
    ++g_cAttempts;
    LastAttempt la{};
    la.frame = frame;
    memcpy(la.hayPos, hay.pos, sizeof(la.hayPos));
    memcpy(la.playerPos, pp, sizeof(la.playerPos));
    la.horiz = chosenH;
    la.dz = pp[2] - hay.pos[2];

    if (!g_hayProbeReady) {
        memset(g_hayProbe, 0, sizeof(g_hayProbe));
        *reinterpret_cast<uint64*>(g_hayProbe + kProbeBlockOffset) = kEmptySharedBlock;
        g_hayProbeReady = true;
    }
    if (!PrepareHayProbeGuarded(hay)) { la.result = -1; FinishAttempt(la); FaultC(); return; }

    uint64 outAction = 0;
    bool created = false;
    if (CreateHayActionGuarded(tester, a2, a3, a5, a6, a7, reinterpret_cast<uint64>(g_hayProbe),
                               static_cast<int>(kEnumLeapOfFaith), outAction, created) != 0) {
        la.result = -1; FinishAttempt(la); FaultC(); return;
    }
    if (created) {
        ++g_cCreated;
        if (outAction) {
            if (!AppendActionGuarded(a9, outAction)) { la.result = -1; FinishAttempt(la); FaultC(); return; }
            ++g_cAppended;
            g_injected.store(outAction);
            g_injectedFrame.store(frame);
            la.result = 3;
        } else {
            la.result = 2;
        }
    } else {
        ++g_cRejected;
        la.result = 1;
    }
    FinishAttempt(la);
}

// The back-eject tester's slot 0x25: the original runs first with all nine arguments, then experiment C.
uint64 __fastcall BackEjectThunk(uint64 a1, uint64 a2, uint64 a3, uint64 a4, uint64 a5,
                                 uint64 a6, uint64 a7, uint64 a8, uint64 a9)
{
    const uint64 result = reinterpret_cast<BackEjectFn>(g_backOrig)(a1, a2, a3, a4, a5, a6, a7, a8, a9);
    if (g_expC.load(std::memory_order_relaxed)) BackEjectInject(a1, a2, a3, a5, a6, a7, a9);
    if (g_expE.load(std::memory_order_relaxed)) BackEjectEject(a1, a2, a3, a5, a6, a7, a9);
    return result;
}

// Experiment D: true while `action` is the LoF action experiment C appended within the last kInjectedCurrentFrames.
bool IsInjectedCurrent(uint64 action)
{
    const uint64 inj = g_injected.load(std::memory_order_relaxed);
    if (!inj || action != inj) return false;
    return g_frame.load(std::memory_order_relaxed) - g_injectedFrame.load(std::memory_order_relaxed) <= kInjectedCurrentFrames;
}

// LoF FinalFilter1 (slot 0x76): original first; only the injected action can be forced to pass.
uint64 __fastcall FinalFilterThunk(uint64 action, uint64 a2, uint64 a3, uint64 a4)
{
    const uint64 orig = reinterpret_cast<FinalFilterFn>(g_ff1Orig)(action, a2, a3, a4);
    if (!IsInjectedCurrent(action)) return orig;
    ++g_ff1Calls;
    // bool results live in AL only; the upper bits of RAX are garbage.
    if (orig & 0xFF) ++g_ff1OrigTrue; else ++g_ff1OrigFalse;
    if (g_expC.load(std::memory_order_relaxed) && g_forceFinal.load(std::memory_order_relaxed)) {
        ++g_ff1Forced;
        return 1;
    }
    return orig;
}

// LoF mode function (slot 0x73): observed only.
uint64 __fastcall LofModeThunk(uint64 action, uint64 mode)
{
    const uint64 orig = reinterpret_cast<LofModeFn>(g_modeOrig)(action, mode);
    if (IsInjectedCurrent(action)) {
        ++g_modeCalls;
        g_lastMode.store(mode);
        g_lastModeResult.store(orig & 0xFF);   // bool in AL only
    }
    return orig;
}

// One thunk per action row: the row index is a compile-time constant.
template <size_t I>
bool __fastcall InitialTestThunk(uint64 a1, uint64 a2, uint64 a3, uint64 a4, uint64 a5,
                                 uint64 a6, uint64 a7, uint64 a8, uint64 a9)
{
    return InitialTestImpl(I, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

// One thunk per tester row. The 4th parameter is a float (xmm3).
template <size_t I>
uint64 __fastcall GatherThunk(uint64 a1, uint64 a2, uint64 a3, float a4, uint64 a5, uint64 a6, uint64 a7)
{
    return GatherImpl(I, a1, a2, a3, a4, a5, a6, a7);
}

template <size_t... I>
constexpr std::array<InitialTestFn, sizeof...(I)> MakeInitialThunks(std::index_sequence<I...>)
{
    return { &InitialTestThunk<I>... };
}

template <size_t... I>
constexpr std::array<GatherFn, sizeof...(I)> MakeGatherThunks(std::index_sequence<I...>)
{
    return { &GatherThunk<I>... };
}

constexpr auto kThunks = MakeInitialThunks(std::make_index_sequence<kRowCount>{});
static_assert(kThunks.size() == kRowCount, "one thunk per slot row");
constexpr auto kGatherThunks = MakeGatherThunks(std::make_index_sequence<kTesterRowCount>{});
static_assert(kGatherThunks.size() == kTesterRowCount, "one gather thunk per tester row");

// ---------------------------------------------------------------------------------------------------------
// Install: validate every row first, then patch. Validation first means every stored origFn is set
// before any slot points at a thunk.
// ---------------------------------------------------------------------------------------------------------
// Checks name hash, static index and expected fn of one fancy-table entry. Logs the reason when it fails.
bool ValidateEntry(uint64 entry, uint32 wantHash, uint32 wantIdx, uint64 expectedFn, const char* tag, uint64 id,
                   uint64& fnOut)
{
    uint32 nameHash = 0, idx = 0;
    uint64 fn = 0;
    if (!ReadSlotGuarded(entry, nameHash, idx, fn)) {
        LogLine("skip %s 0x%llX: entry unreadable (found 0x0)", tag, static_cast<unsigned long long>(id));
        return false;
    }
    if (nameHash != wantHash) {
        LogLine("skip %s 0x%llX: name hash mismatch (found 0x%08X)", tag, static_cast<unsigned long long>(id), nameHash);
        return false;
    }
    if (idx != wantIdx) {
        LogLine("skip %s 0x%llX: static index mismatch (found 0x%X)", tag, static_cast<unsigned long long>(id), idx);
        return false;
    }
    if (fn != expectedFn) {
        // Also the case when a shared table was already patched by another row (found = our thunk).
        LogLine("skip %s 0x%llX: fn mismatch (found 0x%llX)", tag, static_cast<unsigned long long>(id),
                static_cast<unsigned long long>(fn));
        return false;
    }
    fnOut = fn;
    return true;
}

void InstallProbeHooks()
{
    // Phase 1: validate.
    bool valid[kRowCount] = {};
    for (size_t i = 0; i < kRowCount; ++i) {
        SlotRow& row = g_rows[i];
        const uint64 entry = row.vtable + kSlotIndex * kSlotStride;
        uint64 fn = 0;
        if (!ValidateEntry(entry, kSlotNameHash, kSlotIndex, row.expectedFn, "enum", static_cast<uint64>(row.enumAction), fn))
            continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            if (valid[j] && g_rows[j].vtable == row.vtable) {
                LogLine("skip enum %d: vtable 0x%llX already used by enum %d", row.enumAction,
                        static_cast<unsigned long long>(row.vtable), g_rows[j].enumAction);
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        row.origFn = fn;          // stored before any patch, so a thunk can never see origFn == 0
        valid[i] = true;
    }

    // Phase 2: patch each valid row's slot with that row's thunk.
    for (size_t i = 0; i < kRowCount; ++i) {
        SlotRow& row = g_rows[i];
        if (!valid[i]) { ++g_skippedCount; continue; }
        const uint64 entry = row.vtable + kSlotIndex * kSlotStride;
        if (!PatchSlotGuarded(entry + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(kThunks[i])))) {
            LogLine("skip enum %d: patch failed (found 0x%llX)", row.enumAction, static_cast<unsigned long long>(row.origFn));
            ++g_skippedCount;
            continue;
        }
        row.installed = true;
        ++g_installedCount;
    }
    LogLine("hook install: installed %zu / %zu, skipped %zu", g_installedCount, kRowCount, g_skippedCount);
}

void InstallTesterHooks()
{
    bool valid[kTesterRowCount] = {};
    for (size_t i = 0; i < kTesterRowCount; ++i) {
        TesterRow& row = g_tRows[i];
        const uint64 entry = row.table + kTesterIndex * kSlotStride;
        uint64 fn = 0;
        if (!ValidateEntry(entry, kTesterNameHash, kTesterIndex, row.expectedFn, "tester table", row.table, fn))
            continue;
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            if (valid[j] && g_tRows[j].table == row.table) { duplicate = true; break; }
        }
        if (duplicate) { LogLine("skip tester table 0x%llX: duplicate", static_cast<unsigned long long>(row.table)); continue; }
        row.origFn = fn;
        valid[i] = true;
    }
    for (size_t i = 0; i < kTesterRowCount; ++i) {
        TesterRow& row = g_tRows[i];
        if (!valid[i]) { ++g_tSkippedCount; continue; }
        const uint64 entry = row.table + kTesterIndex * kSlotStride;
        if (!PatchSlotGuarded(entry + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(kGatherThunks[i])))) {
            LogLine("skip tester table 0x%llX: patch failed", static_cast<unsigned long long>(row.table));
            ++g_tSkippedCount;
            continue;
        }
        row.installed = true;
        ++g_tInstalledCount;
    }
    LogLine("tester hook install: installed %zu / %zu, skipped %zu", g_tInstalledCount, kTesterRowCount, g_tSkippedCount);
}

// Experiment D: validate both LoF slots first, then patch them. One thunk each.
void InstallLofHooks()
{
    const uint64 entry76 = kLofTable + kLofFinalFilterIndex * kSlotStride;
    const uint64 entry73 = kLofTable + kLofModeIndex * kSlotStride;
    uint64 fn76 = 0, fn73 = 0;
    if (!ValidateEntry(entry76, kLofFinalFilterHash, kLofFinalFilterIndex, kLofFinalFilterExpected, "LoF final filter", kLofTable, fn76)) {
        LogLine("experiment D unavailable: LoF final filter hook not installed");
        return;
    }
    if (!ValidateEntry(entry73, kLofModeHash, kLofModeIndex, kLofModeExpected, "LoF mode", kLofTable, fn73)) {
        LogLine("experiment D: LoF mode hook not installed (final filter skipped too)");
        return;
    }
    g_ff1Orig = fn76;
    g_modeOrig = fn73;     // both stored before any patch
    const bool ok76 = PatchSlotGuarded(entry76 + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(&FinalFilterThunk)));
    const bool ok73 = PatchSlotGuarded(entry73 + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(&LofModeThunk)));
    g_lofInstalled = ok76 && ok73;
    LogLine("experiment D hooks: final filter %s, mode %s", ok76 ? "installed" : "FAILED", ok73 ? "installed" : "FAILED");
}

// Experiment E: observation hook on the eject table's FinalFilter1. Validates the hash and index only; the original
// function is read here and logged (no expected value is hardcoded).
void InstallEjectFinalHook()
{
    const uint64 entry = kEjectTable + kEjectFinalIdx * kSlotStride;
    uint32 nameHash = 0, idx = 0;
    uint64 fn = 0;
    if (!ReadSlotGuarded(entry, nameHash, idx, fn)) {
        LogLine("experiment E unavailable: eject final filter entry unreadable");
        return;
    }
    if (nameHash != kLofFinalFilterHash || idx != kEjectFinalIdx) {
        LogLine("experiment E unavailable: eject final filter hash/index mismatch (found 0x%08X idx 0x%X)", nameHash, idx);
        return;
    }
    g_ejectFinalOrig = fn;   // stored before the patch
    if (!PatchSlotGuarded(entry + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(&EjectFinalThunk)))) {
        LogLine("experiment E: eject final filter patch failed");
        return;
    }
    g_ejectFinalInstalled = true;
    LogLine("eject final filter hooked: table 0x%llX slot 0x%X original fn 0x%llX",
            static_cast<unsigned long long>(kEjectTable), kEjectFinalIdx, static_cast<unsigned long long>(fn));
}

// Experiment F: validate hash, index and the expected fn of the eject table's range setter, then patch.
void InstallEjectRangeHook()
{
    const uint64 entry = kEjectTable + kEjectRangeIdx * kSlotStride;
    uint64 fn = 0;
    if (!ValidateEntry(entry, kEjectRangeHash, kEjectRangeIdx, kEjectRangeExpected, "eject range setter", kEjectTable, fn)) {
        LogLine("experiment F unavailable: range setter not hooked");
        return;
    }
    g_rangeOrig = fn;      // stored before the patch
    if (!PatchSlotGuarded(entry + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(&EjectRangeThunk)))) {
        LogLine("experiment F: range setter patch failed");
        return;
    }
    g_rangeInstalled = true;
    LogLine("range setter hooked: table 0x%llX slot 0x%X original fn 0x%llX",
            static_cast<unsigned long long>(kEjectTable), kEjectRangeIdx, static_cast<unsigned long long>(fn));
}

void InstallBackEjectHook()
{
    const uint64 entry = kBackEjectTable + kBackEjectIndex * kSlotStride;
    uint64 fn = 0;
    if (!ValidateEntry(entry, kBackEjectNameHash, kBackEjectIndex, kBackEjectExpectedFn, "back-eject tester",
                       kBackEjectTable, fn)) {
        LogLine("experiment C unavailable: back-eject hook not installed");
        return;
    }
    g_backOrig = fn;     // stored before the patch, so the thunk can never see origFn == 0
    if (!PatchSlotGuarded(entry + kSlotFnOffset, static_cast<uint64>(reinterpret_cast<uintptr_t>(&BackEjectThunk)))) {
        LogLine("back-eject hook: patch failed");
        return;
    }
    g_backInstalled = true;
    LogLine("back-eject hook installed (table 0x%llX slot 0x%X)", static_cast<unsigned long long>(kBackEjectTable), kBackEjectIndex);
}

// ---------------------------------------------------------------------------------------------------------
// Geometry and keys
// ---------------------------------------------------------------------------------------------------------
float DistanceTo(const float pos[3], const float p[3])
{
    const float dx = pos[0] - p[0];
    const float dy = pos[1] - p[1];
    const float dz = pos[2] - p[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float DistanceToPlayer(const Record& r)
{
    return DistanceTo(r.pos, g_playerPos);
}

bool PassesDistance(const Record& r)
{
    if (!g_hasPlayer) return true;
    return DistanceToPlayer(r) <= g_maxDistance;
}

SessionKey KeyOf(const Record& r)
{
    return std::make_tuple(r.enumAction, r.type,
                           std::llround(r.pos[0] * 100.0), std::llround(r.pos[1] * 100.0), std::llround(r.pos[2] * 100.0),
                           std::llround(r.dest[0] * 100.0), std::llround(r.dest[1] * 100.0), std::llround(r.dest[2] * 100.0),
                           r.isPlayer);
}

// Nearest player sample in frame, or nullptr when none was taken.
const Sample* NearestSample(uint64 frame)
{
    if (g_samples.empty()) return nullptr;
    auto it = std::lower_bound(g_samples.begin(), g_samples.end(), frame,
                               [](const Sample& s, uint64 f) { return s.frame < f; });
    if (it == g_samples.end()) return &g_samples.back();
    if (it == g_samples.begin()) return &*it;
    const auto prev = it - 1;
    return (frame - prev->frame <= it->frame - frame) ? &*prev : &*it;
}

// ---------------------------------------------------------------------------------------------------------
// Session store
// ---------------------------------------------------------------------------------------------------------
void AddToSession(const Record& r)
{
    if (!g_sessionIncludeNpcs && !r.isPlayer) return;
    ++g_sessionRecords;
    ++g_sessionCounts[{ r.enumAction, r.type }];

    const SessionKey k = KeyOf(r);
    auto it = g_session.find(k);
    if (it == g_session.end()) {
        if (g_session.size() >= kMaxSessionKeys) { ++g_sessionOverflow; return; }
        g_session.emplace(k, SessionEntry{ r, 1, r.frame, r.frame, r.result });
        return;
    }
    SessionEntry& e = it->second;
    ++e.count;
    e.lastFrame = r.frame;
    e.anyTrue = e.anyTrue || r.result;
}

void AddGatherToSession(const GatherRecord& g)
{
    if (!g_sessionIncludeNpcs && !g.isPlayer) return;
    TesterSess& t = g_tSession[g.testerTable];
    ++t.calls;
    t.enumCount = g.enumCount;
    t.storedEnums = g.storedEnums;
    t.lastIsPlayer = g.isPlayer;
    for (uint32 i = 0; i < g.storedEnums; ++i) t.enums[i] = g.enums[i];
    for (uint32 i = 0; i < g.newCount; ++i) {
        const GatherProbe& p = g.newProbes[i];
        const TesterProbeKey k = std::make_tuple(p.type, std::llround(p.pos[0] * 100.0),
                                                 std::llround(p.pos[1] * 100.0), std::llround(p.pos[2] * 100.0), g.isPlayer);
        auto it = t.probes.find(k);
        if (it != t.probes.end()) { ++it->second.count; continue; }
        if (t.probes.size() >= kTesterProbesCap) { ++t.probesDropped; continue; }
        TesterProbeEntry e{};
        e.type = p.type;
        memcpy(e.pos, p.pos, sizeof(e.pos));
        e.count = 1;
        e.isPlayer = g.isPlayer;
        t.probes.emplace(k, e);
    }
}

// True when 112 is in the table's last seen enum list. `known` is false if that list was truncated and 112 was not in it.
bool TesterHas112(const TesterSess& t, bool& known)
{
    known = true;
    for (uint32 i = 0; i < t.storedEnums; ++i)
        if (t.enums[i] == static_cast<int32>(kEnumLeapOfFaith)) return true;
    known = t.enumCount <= t.storedEnums;
    return false;
}

void SampleIfDue(uint64 frame)
{
    if (!g_hasPlayer || g_samples.size() >= kMaxSamples) return;
    if ((frame - g_sessionStartFrame) % kSampleEveryFrames != 0) return;
    Sample s{};
    s.frame = frame;
    memcpy(s.pos, g_playerPos, sizeof(s.pos));
    g_samples.push_back(s);
}

// ---------------------------------------------------------------------------------------------------------
// Frame-side helpers
// ---------------------------------------------------------------------------------------------------------
void DrainRing()
{
    g_drain.clear();
    AcquireSRWLockExclusive(&g_ringLock);
    if (g_ringWrite - g_ringRead > kRingSize) {
        g_dropped += (g_ringWrite - g_ringRead) - kRingSize;
        g_ringRead = g_ringWrite - kRingSize;
    }
    for (uint64 i = g_ringRead; i < g_ringWrite; ++i) g_drain.push_back(g_ring[i % kRingSize]);
    g_ringRead = g_ringWrite;
    ReleaseSRWLockExclusive(&g_ringLock);
    g_recent.insert(g_recent.end(), g_drain.begin(), g_drain.end());
    if (g_recording) {
        for (const Record& r : g_drain) AddToSession(r);
    }
}

void DrainGather()
{
    g_gDrain.clear();
    AcquireSRWLockExclusive(&g_gLock);
    if (g_gWrite - g_gRead > kGatherRingSize) {
        g_dropped += (g_gWrite - g_gRead) - kGatherRingSize;
        g_gRead = g_gWrite - kGatherRingSize;
    }
    for (uint64 i = g_gRead; i < g_gWrite; ++i) g_gDrain.push_back(g_gRing[i % kGatherRingSize]);
    g_gRead = g_gWrite;
    ReleaseSRWLockExclusive(&g_gLock);
    for (const GatherRecord& g : g_gDrain) {
        g_gatherRecent.push_back(g);
        if (g_recording) AddGatherToSession(g);
    }
}

void PruneRecent(uint64 frame)
{
    g_recent.erase(std::remove_if(g_recent.begin(), g_recent.end(),
                                  [frame](const Record& r) { return r.frame + kRecentFrames <= frame; }),
                   g_recent.end());
    g_gatherRecent.erase(std::remove_if(g_gatherRecent.begin(), g_gatherRecent.end(),
                                        [frame](const GatherRecord& g) { return g.frame + kRecentFrames <= frame; }),
                         g_gatherRecent.end());
}

std::set<uint32> ParseTypes(const char* s)
{
    std::set<uint32> out;
    const char* p = s;
    while (*p) {
        if (std::isdigit(static_cast<unsigned char>(*p))) {
            char* end = nullptr;
            out.insert(static_cast<uint32>(std::strtoul(p, &end, 10)));
            p = end;
        } else {
            ++p;
        }
    }
    return out;
}

bool PassesType(const std::set<uint32>& types, uint32 t)
{
    return types.empty() || types.count(t) != 0;
}

// Live-view filter: player records only unless the NPC checkbox is on.
bool ViewOk(bool isPlayer)
{
    return isPlayer || g_includeNpcs.load();
}

// Distinct probes by (enum, type, position rounded to 1 cm, isPlayer). Newest first; keeps the latest record of each key.
// Probe-less records have position zero, so they collapse to one row per enum.
std::vector<Record> Distinct(const std::vector<Record>& src)
{
    std::vector<Record> out;
    std::set<std::tuple<int, uint32, long long, long long, long long, bool>> seen;
    for (auto it = src.rbegin(); it != src.rend(); ++it) {
        const auto key = std::make_tuple(it->enumAction, it->type,
                                         std::llround(it->pos[0] * 100.0), std::llround(it->pos[1] * 100.0),
                                         std::llround(it->pos[2] * 100.0), it->isPlayer);
        if (seen.insert(key).second) out.push_back(*it);
    }
    return out;
}

std::map<std::pair<int, uint32>, size_t> CountPairs(const std::vector<Record>& src)
{
    std::map<std::pair<int, uint32>, size_t> counts;
    for (const Record& r : src) ++counts[{ r.enumAction, r.type }];
    return counts;
}

// One record as a log line body. dist and dz are relative to `ref` (-1 when there is no reference).
void FormatRecord(const Record& r, const float* ref, char* out, size_t n)
{
    const double dist = ref ? static_cast<double>(DistanceTo(r.pos, ref)) : -1.0;
    const double dz = ref ? static_cast<double>(r.pos[2] - ref[2]) : -1.0;
    snprintf(out, n,
             "frame=%llu enum=%d type=%u result=%d dist=%.2f dz=%.2f pos=(%.3f, %.3f, %.3f) dir=(%.3f, %.3f, %.3f) "
             "pos40=(%.3f, %.3f, %.3f) pos50=(%.3f, %.3f, %.3f) dest=(%.3f, %.3f, %.3f) destFacing=(%.3f, %.3f, %.3f) "
             "heightDiff=%.3f horizDiff=%.3f actDistance=%.3f actionLayer=%u fitness=%.4f isPlayer=%d",
             static_cast<unsigned long long>(r.frame), r.enumAction, r.type, r.result ? 1 : 0, dist, dz,
             r.pos[0], r.pos[1], r.pos[2], r.dir[0], r.dir[1], r.dir[2],
             r.p40[0], r.p40[1], r.p40[2], r.p50[0], r.p50[1], r.p50[2],
             r.dest[0], r.dest[1], r.dest[2], r.destFacing[0], r.destFacing[1], r.destFacing[2],
             static_cast<double>(r.heightDiff), static_cast<double>(r.horizDiff), static_cast<double>(r.actDistance),
             r.actionLayer, static_cast<double>(r.fitness), r.isPlayer ? 1 : 0);
}

// Comma-separated enum list (up to kGatherEnumMax entries).
void FormatEnums(const int32* enums, uint32 count, char* out, size_t n)
{
    size_t used = 0;
    out[0] = '\0';
    for (uint32 i = 0; i < count && used + 12 < n; ++i)
        used += static_cast<size_t>(snprintf(out + used, n - used, "%s%d", i ? "," : "", enums[i]));
}

// Experiment C counters and the last attempt, as one log body.
void FormatExpC(char* out, size_t n)
{
    snprintf(out, n, "attempts=%llu created=%llu appended=%llu rejected=%llu skipped=%llu (noHay=%llu notBelow=%llu tooFar=%llu stale=%llu) faults=%llu enabled=%d "
             "ff1Calls=%llu ff1OrigTrue=%llu ff1OrigFalse=%llu ff1Forced=%llu modeCalls=%llu lastMode=%llu lastModeResult=%llu",
             static_cast<unsigned long long>(g_cAttempts.load()), static_cast<unsigned long long>(g_cCreated.load()),
             static_cast<unsigned long long>(g_cAppended.load()), static_cast<unsigned long long>(g_cRejected.load()),
             static_cast<unsigned long long>(g_cSkipped.load()),
             static_cast<unsigned long long>(g_cSkipNoHay.load()), static_cast<unsigned long long>(g_cSkipNotBelow.load()),
             static_cast<unsigned long long>(g_cSkipTooFar.load()), static_cast<unsigned long long>(g_cSkipStale.load()),
             static_cast<unsigned long long>(g_cFaults.load()),
             g_expC.load() ? 1 : 0,
             static_cast<unsigned long long>(g_ff1Calls.load()), static_cast<unsigned long long>(g_ff1OrigTrue.load()),
             static_cast<unsigned long long>(g_ff1OrigFalse.load()), static_cast<unsigned long long>(g_ff1Forced.load()),
             static_cast<unsigned long long>(g_modeCalls.load()), static_cast<unsigned long long>(g_lastMode.load()),
             static_cast<unsigned long long>(g_lastModeResult.load()));
}

// Experiment E counters as one log body.
void FormatExpE(char* out, size_t n)
{
    snprintf(out, n,
             "eAttempts=%llu eCreated=%llu eRejected=%llu eRemovedVanilla=%llu eAppended=%llu eSkipped=%llu "
             "(noHay=%llu notBelow=%llu tooFar=%llu stale=%llu) eFaults=%llu enabled=%d "
             "ef1Calls=%llu ef1Zero=%llu ef1NonZero=%llu ef1Forced=%llu lastAL=%llu",
             static_cast<unsigned long long>(g_eAttempts.load()), static_cast<unsigned long long>(g_eCreated.load()),
             static_cast<unsigned long long>(g_eRejected.load()), static_cast<unsigned long long>(g_eRemovedVanilla.load()),
             static_cast<unsigned long long>(g_eAppended.load()), static_cast<unsigned long long>(g_eSkipped.load()),
             static_cast<unsigned long long>(g_eSkipNoHay.load()), static_cast<unsigned long long>(g_eSkipNotBelow.load()),
             static_cast<unsigned long long>(g_eSkipTooFar.load()), static_cast<unsigned long long>(g_eSkipStale.load()),
             static_cast<unsigned long long>(g_eFaults.load()), g_expE.load() ? 1 : 0,
             static_cast<unsigned long long>(g_ef1Calls.load()), static_cast<unsigned long long>(g_ef1Zero.load()),
             static_cast<unsigned long long>(g_ef1NonZero.load()), static_cast<unsigned long long>(g_ef1Forced.load()),
             static_cast<unsigned long long>(g_ef1LastAL.load()));
}

void WriteSnapshot(const char* why)
{
    std::vector<Record> view;
    for (const Record& r : g_recent) if (ViewOk(r.isPlayer)) view.push_back(r);
    const std::vector<Record> distinct = Distinct(view);
    size_t written = 0;
    LogLine("---- BackEjectLoF snapshot (%s) frame=%llu hook installed=%zu/%zu dropped=%llu faults=%u ----",
            why, static_cast<unsigned long long>(g_frame.load()), g_installedCount, kRowCount,
            static_cast<unsigned long long>(g_dropped.load()), static_cast<unsigned>(g_faults.load()));
    if (g_hasPlayer) LogLine("player pos=(%.3f, %.3f, %.3f) maxDistance=%.1f", g_playerPos[0], g_playerPos[1], g_playerPos[2], static_cast<double>(g_maxDistance));
    else LogLine("player: none (no world); distance filter off, dist and dz logged as -1");
    char line[1024];
    for (const Record& r : distinct) {
        if (!PassesDistance(r)) continue;
        ++written;
        FormatRecord(r, g_hasPlayer ? g_playerPos : nullptr, line, sizeof(line));
        LogLine("%s", line);
    }
    LogLine("distinct probes written: %zu (of %zu distinct in last %llu frames)", written, distinct.size(),
            static_cast<unsigned long long>(kRecentFrames));
    LogLine("count table: enum type count (raw records, filtered by NPC setting)");
    for (const auto& kv : CountPairs(view))
        LogLine("  enum=%d type=%u count=%zu", kv.first.first, kv.first.second, kv.second);
    LogLine("---- end snapshot ----");
    g_msgText = "snapshot written";
    g_msgUntilMs = GetTickCount64() + 3000;
}

void StartSession(uint64 frame)
{
    g_session.clear();
    g_sessionCounts.clear();
    g_tSession.clear();
    g_samples.clear();
    g_samples.reserve(kMaxSamples);
    g_sessionRecords = 0;
    g_sessionOverflow = 0;
    g_sessionStartFrame = frame;
    g_sessionEndFrame = frame;
    g_recording = true;
    g_sessionIncludeNpcs = g_includeNpcs.load();
    g_lastSessionValid = false;
    if (g_hasPlayer) LogLine("---- session start frame=%llu player pos=(%.3f, %.3f, %.3f) NPCs=%d ----",
                             static_cast<unsigned long long>(frame), g_playerPos[0], g_playerPos[1], g_playerPos[2],
                             g_sessionIncludeNpcs ? 1 : 0);
    else LogLine("---- session start frame=%llu player: none NPCs=%d ----", static_cast<unsigned long long>(frame),
                 g_sessionIncludeNpcs ? 1 : 0);
    g_msgText = "recording started";
    g_msgUntilMs = GetTickCount64() + 3000;
}

void StopSession(uint64 frame)
{
    g_recording = false;
    g_sessionEndFrame = frame;
    g_lastSessionValid = true;

    LogLine("---- session report start frame=%llu end frame=%llu records=%llu distinct=%zu overflowDrops=%llu samples=%zu ----",
            static_cast<unsigned long long>(g_sessionStartFrame), static_cast<unsigned long long>(g_sessionEndFrame),
            static_cast<unsigned long long>(g_sessionRecords), g_session.size(),
            static_cast<unsigned long long>(g_sessionOverflow), g_samples.size());
    for (const Sample& s : g_samples)
        LogLine("sample frame=%llu pos=(%.3f, %.3f, %.3f)", static_cast<unsigned long long>(s.frame), s.pos[0], s.pos[1], s.pos[2]);

    // Map order is enum, type, then position: sorted by enum then type.
    char line[1024];
    for (const auto& kv : g_session) {
        const SessionEntry& e = kv.second;
        const Sample* ref = NearestSample(e.first.frame);
        FormatRecord(e.first, ref ? ref->pos : nullptr, line, sizeof(line));
        LogLine("key count=%llu firstFrame=%llu lastFrame=%llu anyTrue=%d | %s",
                static_cast<unsigned long long>(e.count), static_cast<unsigned long long>(e.firstFrame),
                static_cast<unsigned long long>(e.lastFrame), e.anyTrue ? 1 : 0, line);
    }
    LogLine("session count table: enum type count");
    for (const auto& kv : g_sessionCounts)
        LogLine("  enum=%d type=%u count=%llu", kv.first.first, kv.first.second, static_cast<unsigned long long>(kv.second));

    LogLine("testers: %zu tables gathered (NPCs included: %d)", g_tSession.size(), g_sessionIncludeNpcs ? 1 : 0);
    char enumBuf[512];
    for (const auto& kv : g_tSession) {
        const TesterSess& t = kv.second;
        bool known = true;
        const bool has112 = TesterHas112(t, known);
        FormatEnums(t.enums, t.storedEnums, enumBuf, sizeof(enumBuf));
        LogLine("tester table=0x%llX calls=%llu enumCount=%u enums=[%s]%s has112=%s lastIsPlayer=%d probesDistinct=%zu probesDropped=%llu",
                static_cast<unsigned long long>(kv.first), static_cast<unsigned long long>(t.calls), t.enumCount, enumBuf,
                t.enumCount > t.storedEnums ? " (truncated)" : "",
                has112 ? "yes" : (known ? "no" : "unknown"), t.lastIsPlayer ? 1 : 0, t.probes.size(),
                static_cast<unsigned long long>(t.probesDropped));
        for (const auto& pk : t.probes)
            LogLine("  tester table=0x%llX gathered type=%u pos=(%.3f, %.3f, %.3f) count=%llu isPlayer=%d",
                    static_cast<unsigned long long>(kv.first), pk.second.type,
                    pk.second.pos[0], pk.second.pos[1], pk.second.pos[2], static_cast<unsigned long long>(pk.second.count),
                    pk.second.isPlayer ? 1 : 0);
    }

    char expC[256];
    FormatExpC(expC, sizeof(expC));
    LogLine("experiment C (LoF from hang) at session end: %s", expC);
    LogLine("  hay memory: %d entries (frame %llu)", 0, static_cast<unsigned long long>(frame));
    for (size_t i = 0; i < kHayMemCap; ++i) {
        const HayEntry& e = g_hayMem[i];
        if (!e.valid) continue;
        const float dx = e.pos[0] - g_playerPos[0];
        const float dy = e.pos[1] - g_playerPos[1];
        LogLine("  hay pos=(%.3f, %.3f, %.3f) age=%llu entity=0x%llX horiz=%.2f dz=%.2f",
                e.pos[0], e.pos[1], e.pos[2], static_cast<unsigned long long>(frame - e.lastSeen),
                static_cast<unsigned long long>(e.entity), static_cast<double>(std::sqrt(dx * dx + dy * dy)),
                static_cast<double>(g_hasPlayer ? g_playerPos[2] - e.pos[2] : 0.0f));
    }
    LogLine("  last attempt: frame=%llu result=%d hayPos=(%.3f, %.3f, %.3f) playerPos=(%.3f, %.3f, %.3f) horiz=%.2f dz=%.2f",
            static_cast<unsigned long long>(g_lastAttempt.frame), g_lastAttempt.result,
            g_lastAttempt.hayPos[0], g_lastAttempt.hayPos[1], g_lastAttempt.hayPos[2],
            g_lastAttempt.playerPos[0], g_lastAttempt.playerPos[1], g_lastAttempt.playerPos[2],
            static_cast<double>(g_lastAttempt.horiz), static_cast<double>(g_lastAttempt.dz));
    char eb[512];
    FormatExpE(eb, sizeof(eb));
    LogLine("experiment E (eject on hay) at session end: %s probeType=%u relabelHay=%d relabels=%u", eb, static_cast<unsigned>(g_ejectProbeType.load()), g_relabelHay.load() ? 1 : 0, static_cast<unsigned>(g_relabelCount.load()));
    LogLine("  last E attempt: frame=%llu result=%d hayPos=(%.3f, %.3f, %.3f) playerPos=(%.3f, %.3f, %.3f) horiz=%.2f dz=%.2f dest=(%.3f, %.3f, %.3f) heightDiff=%.3f horizDiff=%.3f",
            static_cast<unsigned long long>(g_lastEject.frame), g_lastEject.result,
            g_lastEject.hayPos[0], g_lastEject.hayPos[1], g_lastEject.hayPos[2],
            g_lastEject.playerPos[0], g_lastEject.playerPos[1], g_lastEject.playerPos[2],
            static_cast<double>(g_lastEject.horiz), static_cast<double>(g_lastEject.dz),
            g_lastEject.dest[0], g_lastEject.dest[1], g_lastEject.dest[2],
            static_cast<double>(g_lastEject.heightDiff), static_cast<double>(g_lastEject.horizDiff));
    LogLine("  experiment F (range widening): widenCalls=%llu widenWritten=%llu enabled=%d lastAttempt calls=%u widened=%u orig=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f] now=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f]",
            static_cast<unsigned long long>(g_widenCalls.load()), static_cast<unsigned long long>(g_widenWritten.load()),
            g_widenEjectRanges.load() ? 1 : 0, g_lastEject.ranges.calls, g_lastEject.ranges.widened,
            static_cast<double>(g_lastEject.ranges.orig[0]), static_cast<double>(g_lastEject.ranges.orig[1]),
            static_cast<double>(g_lastEject.ranges.orig[2]), static_cast<double>(g_lastEject.ranges.orig[3]),
            static_cast<double>(g_lastEject.ranges.orig[4]), static_cast<double>(g_lastEject.ranges.orig[5]),
            static_cast<double>(g_lastEject.ranges.orig[6]),
            static_cast<double>(g_lastEject.ranges.now[0]), static_cast<double>(g_lastEject.ranges.now[1]),
            static_cast<double>(g_lastEject.ranges.now[2]), static_cast<double>(g_lastEject.ranges.now[3]),
            static_cast<double>(g_lastEject.ranges.now[4]), static_cast<double>(g_lastEject.ranges.now[5]),
            static_cast<double>(g_lastEject.ranges.now[6]));
    LogLine("  experiment G (on-demand gatherer query): calls=%llu found(total)=%llu last=%u faults=%llu enabled=%d",
            static_cast<unsigned long long>(g_qCalls.load()), static_cast<unsigned long long>(g_qProbesFound.load()),
            static_cast<unsigned>(g_qLastFound.load()), static_cast<unsigned long long>(g_qFaults.load()),
            g_queryOn.load() ? 1 : 0);
    LogLine("---- end session report ----");
    g_msgText = "session report written";
    g_msgUntilMs = GetTickCount64() + 3000;
}

void ToggleSession(uint64 frame)
{
    if (g_recording) StopSession(frame);
    else StartSession(frame);
}

// ---------------------------------------------------------------------------------------------------------
// J) Trajectory recorder (v0.16). Read-only: see the header comment. Frame side only; the game is read only through
// GetPlayerPosGuarded, which GameFrame calls. Arm data comes from origins::GetArmSnapshot (atomics).
// ---------------------------------------------------------------------------------------------------------
constexpr size_t kTrajMaxSamples = 2400;
constexpr double kTrajMaxSec = 10.0;
constexpr double kTrajTailSec = 1.0;

struct TrajSample
{
    double t;          // QPC seconds since the arm
    float pos[3];      // player position (valid when hasPlayer)
    bool hasPlayer;
    bool armed;        // the Origins clips were armed in this frame
};

struct TrajStart
{
    uint32 serial;
    uint64 attemptFrame;
    int attemptResult;
    float playerPos[3];
    float hayPos[3];
    float dest[3];
    float heightDiff;
    float horizDiff;
    float dz;
    int flight;        // chosen variant flight (frames)
    float target;      // target flight (frames), -1 when not available
    float drop;        // drop (m), -1 when not available
};

std::atomic<bool> g_trajOn{ true };        // menu checkbox, default ON
bool g_trajStopped = false;                // a fault stopped the recorder for the session
bool g_trajRecording = false;
uint32 g_trajSeenSerial = 0;               // last arm serial seen by the frame callback
double g_trajArmTime = 0.0;                // QPC seconds of the recorded arm
TrajStart g_trajStart{};
std::vector<TrajSample> g_traj;            // reserved at init
char g_trajLast[256] = "";                 // menu status line

void WriteTrajLine(std::ofstream& o, const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    o << buf << "\n";
}

// ---------------------------------------------------------------------------------------------------------
// N) Eject action sampler (v0.24). Read-only, frame side only; same window and sample count as the recorder.
// ---------------------------------------------------------------------------------------------------------
constexpr size_t kActSize = 0x290;

struct ActSample
{
    double t;                  // QPC seconds since the arm (same value as the trajectory sample)
    float z;                   // player z, or -1 when there is no player position
    bool ok;                   // the action bytes were read
    uint8_t bytes[0x290];      // copy of the action object (kActSize bytes)
};

std::vector<ActSample> g_act;  // reserved at init
uint64 g_actPtr = 0;           // action object sampled by the current recording (g_injectedEject at its start)

// Plain SEH function: no C++ objects, so __try is allowed.
bool ReadActionBytesGuarded(uint64 action, uint8_t* out)
{
    if (action < 0x10000ULL) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(action), kActSize);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Writes action_<stamp>_<serial>.csv for the samples of the current recording. Called by FinishTraj.
void WriteActionCsv(const std::string& stamp, uint32 serial)
{
    const std::string name = "action_" + stamp + "_" + std::to_string(serial) + ".csv";
    const fs::path path = AbsolutePathInThisDLLDirectory("BackEjectLoF\\traj\\" + name);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LogLine("action sampler: could not write %s", path.string().c_str());
        return;
    }

    const ActSample* firstOk = nullptr;
    size_t okCount = 0;
    for (const ActSample& s : g_act) {
        if (!s.ok) continue;
        ++okCount;
        if (!firstOk) firstOk = &s;
    }

    WriteTrajLine(out, "# BackEjectLoF 0.27.1 eject action sampler (read-only)");
    WriteTrajLine(out, "# action=0x%llX", static_cast<unsigned long long>(g_actPtr));
    WriteTrajLine(out, "# samples=%zu", g_act.size());
    WriteTrajLine(out, "# ok_samples=%zu", okCount);
    if (!firstOk) {
        out.close();
        LogLine("action sampler: no ok samples, wrote header only to %s (samples=%zu)", name.c_str(), g_act.size());
        return;
    }

    for (size_t o = 0; o < kActSize; o += 4) {
        uint32 iv = 0;
        float fv = 0.0f;
        memcpy(&iv, firstOk->bytes + o, 4);
        memcpy(&fv, &iv, 4);
        WriteTrajLine(out, "# o%03X f=%.6g i=0x%08X", static_cast<unsigned>(o), static_cast<double>(fv), static_cast<unsigned>(iv));
    }

    // Changed offsets: 4-byte values that differ between any two ok samples (compared with the first ok sample).
    std::vector<size_t> changed;
    for (size_t o = 0; o < kActSize; o += 4) {
        uint32 ref = 0;
        memcpy(&ref, firstOk->bytes + o, 4);
        for (const ActSample& s : g_act) {
            if (!s.ok) continue;
            uint32 v = 0;
            memcpy(&v, s.bytes + o, 4);
            if (v != ref) { changed.push_back(o); break; }
        }
    }

    std::string header = "t,z";
    char cb[64];
    for (size_t o : changed) {
        snprintf(cb, sizeof(cb), ",o%03X", static_cast<unsigned>(o));
        header += cb;
    }
    out << header << "\n";

    for (const ActSample& s : g_act) {
        if (!s.ok) continue;
        snprintf(cb, sizeof(cb), "%.4f,%.3f", s.t, static_cast<double>(s.z));
        std::string row = cb;
        for (size_t o : changed) {
            uint32 v = 0;
            memcpy(&v, s.bytes + o, 4);
            float fv = 0.0f;
            memcpy(&fv, &v, 4);
            const uint32 expo = (v >> 23) & 0xFFu;
            const bool isZero = (v & 0x7FFFFFFFu) == 0;
            if ((expo == 0 || expo == 0xFF) && !isZero) snprintf(cb, sizeof(cb), ",i%08X", static_cast<unsigned>(v));
            else snprintf(cb, sizeof(cb), ",%.6g", static_cast<double>(fv));
            row += cb;
        }
        out << row << "\n";
    }
    out.close();

    LogLine("action sampler: wrote %s, samples=%zu ok=%zu changedOffsets=%zu", name.c_str(), g_act.size(), okCount, changed.size());
}

// Writes the recording to traj_<serial>.csv, then logs one line. reason says why the recording ended.
void FinishTraj(const char* reason)
{
    g_trajRecording = false;
    const TrajStart& st = g_trajStart;
    // session stamp (first recording of this game session), so a new session does not overwrite older files
    static const std::string stamp = [] { char b[32]; std::time_t now = std::time(nullptr); std::tm tmv{};
        localtime_s(&tmv, &now); std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tmv); return std::string(b); }();
    const std::string name = "traj_" + stamp + "_" + std::to_string(st.serial) + ".csv";
    const fs::path path = AbsolutePathInThisDLLDirectory("BackEjectLoF\\traj\\" + name);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LogLine("trajectory: could not write %s", path.string().c_str());
        snprintf(g_trajLast, sizeof(g_trajLast), "could not write %s", name.c_str());
        return;
    }

    WriteTrajLine(out, "# BackEjectLoF 0.27.1 trajectory (read-only record of one Origins eject)");
    WriteTrajLine(out, "# serial=%u", static_cast<unsigned>(st.serial));
    WriteTrajLine(out, "# finish=%s", reason);
    WriteTrajLine(out, "# attempt_frame=%llu", static_cast<unsigned long long>(st.attemptFrame));
    WriteTrajLine(out, "# attempt_result=%d", st.attemptResult);
    WriteTrajLine(out, "# playerPos=%.3f,%.3f,%.3f", static_cast<double>(st.playerPos[0]),
                  static_cast<double>(st.playerPos[1]), static_cast<double>(st.playerPos[2]));
    WriteTrajLine(out, "# hayPos=%.3f,%.3f,%.3f", static_cast<double>(st.hayPos[0]),
                  static_cast<double>(st.hayPos[1]), static_cast<double>(st.hayPos[2]));
    WriteTrajLine(out, "# dest=%.3f,%.3f,%.3f", static_cast<double>(st.dest[0]),
                  static_cast<double>(st.dest[1]), static_cast<double>(st.dest[2]));
    WriteTrajLine(out, "# heightDiff=%.3f", static_cast<double>(st.heightDiff));
    WriteTrajLine(out, "# horizDiff=%.3f", static_cast<double>(st.horizDiff));
    WriteTrajLine(out, "# dz=%.3f", static_cast<double>(st.dz));
    WriteTrajLine(out, "# flight_frames=%d", st.flight);
    WriteTrajLine(out, "# target_frames=%.2f", static_cast<double>(st.target));
    WriteTrajLine(out, "# drop_m=%.3f", static_cast<double>(st.drop));
    WriteTrajLine(out, "# samples=%zu", g_traj.size());
    WriteTrajLine(out, "t,x,y,z,armed");

    const TrajSample* last = nullptr;   // last sample with a player position
    float minZ = 0.0f;
    size_t valid = 0;
    char line[160];
    for (const TrajSample& s : g_traj) {
        if (s.hasPlayer) {
            std::snprintf(line, sizeof(line), "%.4f,%.4f,%.4f,%.4f,%d", s.t, static_cast<double>(s.pos[0]),
                          static_cast<double>(s.pos[1]), static_cast<double>(s.pos[2]), s.armed ? 1 : 0);
            if (valid == 0 || s.pos[2] < minZ) minZ = s.pos[2];
            ++valid;
            last = &s;
        } else {
            std::snprintf(line, sizeof(line), "%.4f,nan,nan,nan,%d", s.t, s.armed ? 1 : 0);
        }
        out << line << "\n";
    }
    out.close();
    WriteActionCsv(stamp, st.serial);   // v0.24 (N): action sampler file, same stamp and serial

    const double dur = g_traj.empty() ? 0.0 : g_traj.back().t;
    if (!last) {
        LogLine("trajectory: wrote %s, samples=%zu, no player position in any sample (%s)", name.c_str(), g_traj.size(), reason);
        snprintf(g_trajLast, sizeof(g_trajLast), "last: %s, no player samples", name.c_str());
        return;
    }
    const double dx = static_cast<double>(last->pos[0] - st.hayPos[0]);
    const double dy = static_cast<double>(last->pos[1] - st.hayPos[1]);
    const double horiz = std::sqrt(dx * dx + dy * dy);
    LogLine("trajectory: wrote %s, samples=%zu (player %zu), duration=%.2f s, minZ=%.3f, final=(%.3f, %.3f, %.3f), final horiz to hay=%.2f m (%s)",
            name.c_str(), g_traj.size(), valid, dur, static_cast<double>(minZ), static_cast<double>(last->pos[0]),
            static_cast<double>(last->pos[1]), static_cast<double>(last->pos[2]), horiz, reason);
    snprintf(g_trajLast, sizeof(g_trajLast), "last: %s, %zu samples, %.2f s", name.c_str(), g_traj.size(), dur);
}

// Starts a recording for a new arm. The attempt values are the last experiment E attempt (FinishEject runs after the
// arm, but always before the frame callback that sees the new serial).
void StartTraj(const origins::ArmSnapshot& a)
{
    g_traj.clear();
    TrajStart& st = g_trajStart;
    st.serial = a.serial;
    st.attemptFrame = g_lastEject.frame;
    st.attemptResult = g_lastEject.result;
    memcpy(st.playerPos, g_lastEject.playerPos, sizeof(st.playerPos));
    memcpy(st.hayPos, g_lastEject.hayPos, sizeof(st.hayPos));
    memcpy(st.dest, g_lastEject.dest, sizeof(st.dest));
    st.heightDiff = g_lastEject.heightDiff;
    st.horizDiff = g_lastEject.horizDiff;
    st.dz = g_lastEject.dz;
    st.flight = a.flight;
    st.target = a.target;
    st.drop = a.drop;
    g_trajArmTime = a.armTime;
    g_trajRecording = true;
    LogLine("trajectory: recording arm serial %u (variant f%d, drop %.2f m, target %.1f frames, window %.2f s)",
            static_cast<unsigned>(a.serial), a.flight, static_cast<double>(a.drop), static_cast<double>(a.target),
            static_cast<double>(origins::WindowFor(a.flight)));
    g_act.clear();                              // v0.24 (N)
    g_actPtr = g_injectedEject.load();
    LogLine("action sampler: action 0x%llX", static_cast<unsigned long long>(g_actPtr));
}

// Frame callback: starts, samples and ends recordings. playerFault is the fault flag of this frame's player read.
void TrajFrame(uint64 frame, bool playerFault)
{
    if (playerFault) {
        if (!g_trajStopped) {
            g_trajStopped = true;
            LogLine("trajectory recorder OFF after a fault in the player position read (frame %llu, serial %u): no further recording this session",
                    static_cast<unsigned long long>(frame), static_cast<unsigned>(g_trajStart.serial));
        }
        g_trajRecording = false;
        g_traj.clear();
        return;
    }

    const origins::ArmSnapshot a = origins::GetArmSnapshot();
    if (a.serial != g_trajSeenSerial) {
        g_trajSeenSerial = a.serial;
        if (g_trajRecording) FinishTraj("a new arm started");
        if (g_trajOn.load() && !g_trajStopped && a.serial != 0) StartTraj(a);
    }
    if (!g_trajRecording) return;

    const double t = origins::NowSec() - g_trajArmTime;
    const char* reason = nullptr;
    if (t >= static_cast<double>(origins::WindowFor(g_trajStart.flight)) + kTrajTailSec) reason = "window + 1 s";
    else if (t >= kTrajMaxSec) reason = "10 s cap";
    else if (g_traj.size() >= kTrajMaxSamples) reason = "sample cap";
    if (reason) { FinishTraj(reason); return; }

    TrajSample s{};
    s.t = t;
    s.hasPlayer = g_hasPlayer;
    if (g_hasPlayer) memcpy(s.pos, g_playerPos, sizeof(s.pos));
    s.armed = a.armed;
    g_traj.push_back(s);

    if (g_act.size() < kTrajMaxSamples) {       // v0.24 (N): same frame and time as the trajectory sample
        ActSample as{};
        as.t = t;
        as.z = g_hasPlayer ? g_playerPos[2] : -1.0f;
        as.ok = ReadActionBytesGuarded(g_actPtr, as.bytes);
        g_act.push_back(as);
    }
}

// Menu: the checkbox and the recorder status.
void DrawTrajMenu()
{
    bool on = g_trajOn.load();
    if (ImGui::Checkbox("Record eject trajectories (v0.16)", &on)) g_trajOn.store(on);
    if (g_trajStopped) ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Trajectory recorder OFF after a fault (this session, see log)");
    else if (g_trajRecording) ImGui::Text("Recording arm serial %u", static_cast<unsigned>(g_trajStart.serial));
    if (g_trajLast[0]) ImGui::TextUnformatted(g_trajLast);
}

#include "CameraDiag.inl"   // K) v0.19: camera selector diagnostic (namespace camdiag)
#include "LofCamera.inl"     // L) v0.20: LoF camera during the eject (namespace lofcam)
#include "HayCamera.inl"     // M) v0.23: early hay camera during the eject (namespace haycam)
#include "GraphVars.inl"     // O) v0.25: graph variable recorder (namespace graphvars)
#include "StateLog.inl"      // P) v0.26: player state log (namespace statelog)
#include "CtorProbe.inl"     // Q) v0.27: state constructor probe (namespace ctorprobe)

// Runs every frame, menu open or closed. Game calls are guarded.
void GameFrame()
{
    const uint64 frame = ++g_frame;

    uint64 playerEntity = 0;
    bool playerFault = false;
    g_hasPlayer = GetPlayerPosGuarded(g_playerPos, playerEntity, playerFault);
    g_playerEntity.store(playerEntity);
    origins::Frame(g_hasPlayer);
    DrainRing();
    DrainGather();
    PruneRecent(frame);
    if (g_recording) SampleIfDue(frame);
    TrajFrame(frame, playerFault);      // v0.16 (J): trajectory recorder, after origins::Frame so its disarm is seen
    camdiag::Frame(frame);              // v0.19 (K): camera selector diagnostic, read-only
    lofcam::Frame();                    // v0.20 (L): LoF rule search and arm edge log
    haycam::Frame();                    // v0.23 (M): early hay camera rule search and force window
    graphvars::Frame();                 // v0.25 (O): graph variable recorder, read-only
    statelog::Frame();                  // v0.26 (P): player state log, read-only
    ctorprobe::Frame();                 // v0.27 (Q): state constructor probe, read-only

    if (g_faults.load() >= kMaxFaults && !g_faultLogged) {
        g_faultLogged = true;
        g_capture.store(false);
        LogLine("capture turned off after %u faults in the probe hooks", static_cast<unsigned>(g_faults.load()));
    }

    if (g_expCFault.exchange(false)) {
        char expC[256];
        FormatExpC(expC, sizeof(expC));
        LogLine("experiment C turned OFF after a fault (frame %llu): %s", static_cast<unsigned long long>(frame), expC);
    }
    // Experiment I (v0.13): a registry fault turns the query off; logged here.
    if (g_registryFault.exchange(false)) {
        LogLine("hay registry query turned OFF after a fault (frame %llu): calls=%llu faults=%llu",
                static_cast<unsigned long long>(frame), static_cast<unsigned long long>(g_rCalls.load()),
                static_cast<unsigned long long>(g_rFaults.load()));
    }
    // Experiment E: fault, toggle, attempt and counter logs (same rate limits as C).
    if (g_expEFault.exchange(false)) {
        char eb[512];
        FormatExpE(eb, sizeof(eb));
        LogLine("experiment E turned OFF after a fault (frame %llu): %s", static_cast<unsigned long long>(frame), eb);
    }
    const bool expEOn = g_expE.load();
    if (expEOn != g_expELastLogged) {
        LogLine("experiment E (eject on hay) %s (frame %llu)", expEOn ? "ON" : "OFF", static_cast<unsigned long long>(frame));
        g_expELastLogged = expEOn;
    }
    const uint64 attemptSeqE = g_eAttemptSeq.load(std::memory_order_relaxed);
    if (attemptSeqE != g_eLoggedSeq && frame - g_eLastLogFrame >= kAttemptLogInterval) {
        g_eLoggedSeq = attemptSeqE;
        g_eLastLogFrame = frame;
        const LastEject& le = g_lastEject;
        LogLine("experiment E attempt #%llu: frame=%llu result=%d hayPos=(%.2f, %.2f, %.2f) playerPos=(%.2f, %.2f, %.2f) horiz=%.2f dz=%.2f dest=(%.2f, %.2f, %.2f) heightDiff=%.3f horizDiff=%.3f",
                static_cast<unsigned long long>(attemptSeqE), static_cast<unsigned long long>(le.frame), le.result,
                le.hayPos[0], le.hayPos[1], le.hayPos[2], le.playerPos[0], le.playerPos[1], le.playerPos[2],
                static_cast<double>(le.horiz), static_cast<double>(le.dz),
                le.dest[0], le.dest[1], le.dest[2], static_cast<double>(le.heightDiff), static_cast<double>(le.horizDiff));
        const RangeSnap& rs = le.ranges;
        LogLine("  eject ranges: calls=%u widened=%u valid=%d orig=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f] now=[%.1f %.1f %.1f %.1f %.1f %.1f %.1f] (order: heightMin, horizMax, distMax, durExt, curveMax, curveMin, expCurveMin)",
                rs.calls, rs.widened, rs.valid ? 1 : 0,
                static_cast<double>(rs.orig[0]), static_cast<double>(rs.orig[1]), static_cast<double>(rs.orig[2]), static_cast<double>(rs.orig[3]),
                static_cast<double>(rs.orig[4]), static_cast<double>(rs.orig[5]), static_cast<double>(rs.orig[6]),
                static_cast<double>(rs.now[0]), static_cast<double>(rs.now[1]), static_cast<double>(rs.now[2]), static_cast<double>(rs.now[3]),
                static_cast<double>(rs.now[4]), static_cast<double>(rs.now[5]), static_cast<double>(rs.now[6]));
        LogLine("  on-demand query: calls=%llu found(total)=%llu last=%u faults=%llu enabled=%d",
                static_cast<unsigned long long>(g_qCalls.load()), static_cast<unsigned long long>(g_qProbesFound.load()),
                static_cast<unsigned>(g_qLastFound.load()), static_cast<unsigned long long>(g_qFaults.load()),
                g_queryOn.load() ? 1 : 0);
        LogLine("  hay registry: calls=%llu listSize=%u remembered=%llu faults=%llu enabled=%d",
                static_cast<unsigned long long>(g_rCalls.load()), static_cast<unsigned>(g_rListSize.load()),
                static_cast<unsigned long long>(g_rRemembered.load()), static_cast<unsigned long long>(g_rFaults.load()),
                g_registryOn.load() ? 1 : 0);
    }
    const uint64 sumE = g_eAttempts.load() + g_eCreated.load() + g_eRejected.load() + g_eRemovedVanilla.load()
                        + g_eAppended.load() + g_eSkipped.load() + g_eFaults.load() + g_ef1Calls.load() + g_ef1Forced.load();
    if (sumE != g_lastCounterSumE && frame - g_lastCounterLogFrameE >= kCounterLogInterval) {
        g_lastCounterSumE = sumE;
        g_lastCounterLogFrameE = frame;
        char eb[512];
        FormatExpE(eb, sizeof(eb));
        LogLine("experiment E counters: %s", eb);
    }

    const bool expOn = g_expC.load();
    if (expOn != g_expCLastLogged) {
        LogLine("experiment C (LoF from hang) %s (frame %llu)", expOn ? "ON" : "OFF", static_cast<unsigned long long>(frame));
        g_expCLastLogged = expOn;
    }

    // Counter line at most once per kCounterLogInterval frames, and only when something changed.
    const uint64 sum = g_cAttempts.load() + g_cCreated.load() + g_cAppended.load() + g_cRejected.load()
                       + g_cSkipped.load() + g_cFaults.load() + g_ff1Calls.load() + g_ff1Forced.load() + g_modeCalls.load();
    if (sum != g_lastCounterSum && frame - g_lastCounterLogFrame >= kCounterLogInterval) {
        g_lastCounterSum = sum;
        g_lastCounterLogFrame = frame;
        char expC[256];
        FormatExpC(expC, sizeof(expC));
        LogLine("experiment C counters: %s; last: frame=%llu result=%d hay=(%.2f, %.2f, %.2f) player=(%.2f, %.2f, %.2f)",
                expC, static_cast<unsigned long long>(g_lastAttempt.frame), g_lastAttempt.result,
                g_lastAttempt.hayPos[0], g_lastAttempt.hayPos[1], g_lastAttempt.hayPos[2],
                g_lastAttempt.playerPos[0], g_lastAttempt.playerPos[1], g_lastAttempt.playerPos[2]);
    }

    // One line per attempt, at most every kAttemptLogInterval frames. Attempts in between are coalesced to the newest.
    const uint64 attemptSeq = g_attemptSeq.load(std::memory_order_relaxed);
    if (attemptSeq != g_attemptLoggedSeq && frame - g_lastAttemptLogFrame >= kAttemptLogInterval) {
        g_attemptLoggedSeq = attemptSeq;
        g_lastAttemptLogFrame = frame;
        const LastAttempt& la = g_lastAttempt;
        LogLine("experiment C attempt #%llu: frame=%llu result=%d hayPos=(%.2f, %.2f, %.2f) playerPos=(%.2f, %.2f, %.2f) horiz=%.2f dz=%.2f",
                static_cast<unsigned long long>(attemptSeq), static_cast<unsigned long long>(la.frame), la.result,
                la.hayPos[0], la.hayPos[1], la.hayPos[2], la.playerPos[0], la.playerPos[1], la.playerPos[2],
                static_cast<double>(la.horiz), static_cast<double>(la.dz));
    }

    const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !g_f7Down) ToggleSession(frame);
    g_f7Down = f7;

    const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6 && !g_f6Down) g_expE.store(!g_expE.load());     // F6 toggles experiment E (C is menu-only now)
    g_f6Down = f6;
}

// ---------------------------------------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------------------------------------
void FmtVec(const float v[3], char* buf, size_t n)
{
    snprintf(buf, n, "%.2f, %.2f, %.2f", v[0], v[1], v[2]);
}

void DrawProbeTable(const std::vector<const Record*>& shown)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("probes", 12, flags, ImVec2(0.0f, 300.0f))) return;
    ImGui::TableSetupColumn("action");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("result");
    ImGui::TableSetupColumn("distance");
    ImGui::TableSetupColumn("dz");
    ImGui::TableSetupColumn("pos");
    ImGui::TableSetupColumn("pos40");
    ImGui::TableSetupColumn("pos50");
    ImGui::TableSetupColumn("dir");
    ImGui::TableSetupColumn("dest");
    ImGui::TableSetupColumn("heightDiff");
    ImGui::TableSetupColumn("fitness");
    ImGui::TableHeadersRow();

    char a[32], b[96], c[96], d[96], e[96], f[96], g[96];
    for (const Record* r : shown) {
        ImGui::TableNextRow();
        int col = 0;
        const char* name = ActionName(r->enumAction);
        if (name) snprintf(a, sizeof(a), "%d %s", r->enumAction, name);
        else snprintf(a, sizeof(a), "%d", r->enumAction);
        ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(a);
        ImGui::TableSetColumnIndex(col++); ImGui::Text("%u", r->type);
        ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(r->result ? "yes" : "no");
        ImGui::TableSetColumnIndex(col++);
        if (g_hasPlayer) { snprintf(b, sizeof(b), "%.2f", static_cast<double>(DistanceToPlayer(*r))); ImGui::TextUnformatted(b); }
        else ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(col++);
        if (g_hasPlayer) ImGui::Text("%.2f", static_cast<double>(r->pos[2] - g_playerPos[2]));
        else ImGui::TextUnformatted("-");
        FmtVec(r->pos, c, sizeof(c));   ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(c);
        FmtVec(r->p40, d, sizeof(d));   ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(d);
        FmtVec(r->p50, e, sizeof(e));   ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(e);
        FmtVec(r->dir, f, sizeof(f));   ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(f);
        FmtVec(r->dest, g, sizeof(g));  ImGui::TableSetColumnIndex(col++); ImGui::TextUnformatted(g);
        ImGui::TableSetColumnIndex(col++); ImGui::Text("%.3f", static_cast<double>(r->heightDiff));
        ImGui::TableSetColumnIndex(col++); ImGui::Text("%.4f", static_cast<double>(r->fitness));
    }
    ImGui::EndTable();
}

void DrawCountTable(const std::vector<Record>& view)
{
    const auto counts = CountPairs(view);
    if (!ImGui::BeginTable("counts", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn("action enum");
    ImGui::TableSetupColumn("type");
    ImGui::TableSetupColumn("count");
    ImGui::TableHeadersRow();
    for (const auto& kv : counts) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", kv.first.first);
        ImGui::TableSetColumnIndex(1); ImGui::Text("%u", kv.first.second);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%zu", kv.second);
    }
    ImGui::EndTable();
}

// Live per-tester-table summary over the last kRecentFrames frames.
void DrawTesterTable()
{
    struct Live { uint64 calls; uint32 enumCount; uint32 lastNew; };
    std::map<uint64, Live> live;
    for (const GatherRecord& g : g_gatherRecent) {
        if (!ViewOk(g.isPlayer)) continue;
        Live& L = live[g.testerTable];
        ++L.calls;
        L.enumCount = g.enumCount;
        L.lastNew = g.newCount;
    }
    if (!ImGui::BeginTable("testers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                          ImGuiTableFlags_SizingFixedFit, ImVec2(0.0f, 160.0f))) return;
    ImGui::TableSetupColumn("tester table");
    ImGui::TableSetupColumn("calls (60 frames)");
    ImGui::TableSetupColumn("enum count");
    ImGui::TableSetupColumn("last gathered");
    ImGui::TableHeadersRow();
    for (const auto& kv : live) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("0x%llX", static_cast<unsigned long long>(kv.first));
        ImGui::TableSetColumnIndex(1); ImGui::Text("%llu", static_cast<unsigned long long>(kv.second.calls));
        ImGui::TableSetColumnIndex(2); ImGui::Text("%u", kv.second.enumCount);
        ImGui::TableSetColumnIndex(3); ImGui::Text("%u", kv.second.lastNew);
    }
    ImGui::EndTable();
}

void DrawExpC()
{
    bool on = g_expC.load();
    if (ImGui::Checkbox("Experiment C: LoF from hang (menu only)", &on)) g_expC.store(on);
    ImGui::Text("attempts %llu, created %llu, appended %llu, rejected %llu, skipped %llu, faults %llu",
                static_cast<unsigned long long>(g_cAttempts.load()), static_cast<unsigned long long>(g_cCreated.load()),
                static_cast<unsigned long long>(g_cAppended.load()), static_cast<unsigned long long>(g_cRejected.load()),
                static_cast<unsigned long long>(g_cSkipped.load()), static_cast<unsigned long long>(g_cFaults.load()));
    const uint64 frame = g_frame.load();
    ImGui::Text("Hay memory (%zu max, 60 s freshness):", kHayMemCap);
    if (ImGui::BeginTable("hay", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0.0f, 140.0f))) {
        ImGui::TableSetupColumn("pos");
        ImGui::TableSetupColumn("age (frames)");
        ImGui::TableSetupColumn("entity");
        ImGui::TableSetupColumn("horiz (m)");
        ImGui::TableSetupColumn("dz (m, + = above)");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < kHayMemCap; ++i) {
            const HayEntry& e = g_hayMem[i];
            if (!e.valid) continue;
            const float dx = e.pos[0] - g_playerPos[0];
            const float dy = e.pos[1] - g_playerPos[1];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%.2f, %.2f, %.2f", e.pos[0], e.pos[1], e.pos[2]);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%llu", static_cast<unsigned long long>(frame - e.lastSeen));
            ImGui::TableSetColumnIndex(2); ImGui::Text("0x%llX", static_cast<unsigned long long>(e.entity));
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", static_cast<double>(std::sqrt(dx * dx + dy * dy)));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", static_cast<double>(g_hasPlayer ? g_playerPos[2] - e.pos[2] : 0.0f));
        }
        ImGui::EndTable();
    }
    bool force = g_forceFinal.load();
    if (ImGui::Checkbox("Force final filter for the injected LoF (needs experiment C on)", &force)) g_forceFinal.store(force);
    ImGui::Text("final filter: calls %llu (orig true %llu, false %llu), forced %llu",
                static_cast<unsigned long long>(g_ff1Calls.load()), static_cast<unsigned long long>(g_ff1OrigTrue.load()),
                static_cast<unsigned long long>(g_ff1OrigFalse.load()), static_cast<unsigned long long>(g_ff1Forced.load()));
    ImGui::Text("mode: calls %llu, last mode %llu, last result %llu; hooks %s",
                static_cast<unsigned long long>(g_modeCalls.load()), static_cast<unsigned long long>(g_lastMode.load()),
                static_cast<unsigned long long>(g_lastModeResult.load()), g_lofInstalled ? "installed" : "not installed");
    const LastAttempt& la = g_lastAttempt;
    ImGui::Text("Last attempt: result %d, frame %llu, hay (%.2f, %.2f, %.2f), player (%.2f, %.2f, %.2f)",
                la.result, static_cast<unsigned long long>(la.frame),
                la.hayPos[0], la.hayPos[1], la.hayPos[2], la.playerPos[0], la.playerPos[1], la.playerPos[2]);
}

void DrawExpE()
{
    bool on = g_expE.load();
    if (ImGui::Checkbox("Experiment E: back eject lands on the hay (F6)", &on)) g_expE.store(on);
    bool force = g_forceEjectFinal.load();
    if (ImGui::Checkbox("Force eject final filter for our action", &force)) g_forceEjectFinal.store(force);
    {
        int pt = static_cast<int>(g_ejectProbeType.load());
        ImGui::Text("Eject target probe type:"); ImGui::SameLine();
        if (ImGui::RadioButton("35 hay##ept", pt == 35)) { g_ejectProbeType.store(35); LogLine("eject probe type -> 35"); }
        ImGui::SameLine();
        if (ImGui::RadioButton("28 ground##ept", pt == 28)) { g_ejectProbeType.store(28); LogLine("eject probe type -> 28"); }
        bool rl = g_relabelHay.load();
        if (ImGui::Checkbox("Relabel accepted eject as hay (35)", &rl)) { g_relabelHay.store(rl); LogLine("relabel hay -> %d", rl ? 1 : 0); }
        ImGui::SameLine(); ImGui::Text("relabels: %u", static_cast<unsigned>(g_relabelCount.load()));
    }
    ImGui::Text("attempts %llu, created %llu, appended %llu, rejected %llu, vanilla removed %llu, faults %llu",
                static_cast<unsigned long long>(g_eAttempts.load()), static_cast<unsigned long long>(g_eCreated.load()),
                static_cast<unsigned long long>(g_eAppended.load()), static_cast<unsigned long long>(g_eRejected.load()),
                static_cast<unsigned long long>(g_eRemovedVanilla.load()), static_cast<unsigned long long>(g_eFaults.load()));
    ImGui::Text("skipped %llu (noHay %llu, notBelow %llu, tooFar %llu, stale %llu)",
                static_cast<unsigned long long>(g_eSkipped.load()), static_cast<unsigned long long>(g_eSkipNoHay.load()),
                static_cast<unsigned long long>(g_eSkipNotBelow.load()), static_cast<unsigned long long>(g_eSkipTooFar.load()),
                static_cast<unsigned long long>(g_eSkipStale.load()));
    ImGui::Text("eject final filter: %s; calls %llu (AL zero %llu, nonzero %llu), forced %llu, last AL %llu",
                g_ejectFinalInstalled ? "hooked" : "not hooked", static_cast<unsigned long long>(g_ef1Calls.load()),
                static_cast<unsigned long long>(g_ef1Zero.load()), static_cast<unsigned long long>(g_ef1NonZero.load()),
                static_cast<unsigned long long>(g_ef1Forced.load()), static_cast<unsigned long long>(g_ef1LastAL.load()));
    const LastEject& le = g_lastEject;
    ImGui::Text("Last E attempt: result %d, frame %llu, hay (%.2f, %.2f, %.2f), player (%.2f, %.2f, %.2f), horiz %.2f, dz %.2f",
                le.result, static_cast<unsigned long long>(le.frame), le.hayPos[0], le.hayPos[1], le.hayPos[2],
                le.playerPos[0], le.playerPos[1], le.playerPos[2], static_cast<double>(le.horiz), static_cast<double>(le.dz));
    ImGui::Text("  created dest (%.2f, %.2f, %.2f), heightDiff %.3f, horizDiff %.3f",
                le.dest[0], le.dest[1], le.dest[2], static_cast<double>(le.heightDiff), static_cast<double>(le.horizDiff));
    bool queryOn = g_queryOn.load();
    if (ImGui::Checkbox("Query game haystack search while hanging", &queryOn)) g_queryOn.store(queryOn);
    ImGui::Text("on-demand query: calls %llu, probes found %llu, last %u, faults %llu",
                static_cast<unsigned long long>(g_qCalls.load()), static_cast<unsigned long long>(g_qProbesFound.load()),
                static_cast<unsigned>(g_qLastFound.load()), static_cast<unsigned long long>(g_qFaults.load()));
    bool registryOn = g_registryOn.load();
    if (ImGui::Checkbox("Query hay registry while hanging (v0.13)", &registryOn)) g_registryOn.store(registryOn);
    ImGui::Text("hay registry: calls %llu, list size %u, remembered %llu, faults %llu",
                static_cast<unsigned long long>(g_rCalls.load()), static_cast<unsigned>(g_rListSize.load()),
                static_cast<unsigned long long>(g_rRemembered.load()), static_cast<unsigned long long>(g_rFaults.load()));
    bool widen = g_widenEjectRanges.load();
    if (ImGui::Checkbox("Widen eject ranges for our eject (experiment F)", &widen)) g_widenEjectRanges.store(widen);
    ImGui::Text("range setter: %s; widenCalls %llu, widened %llu",
                g_rangeInstalled ? "hooked" : "not hooked", static_cast<unsigned long long>(g_widenCalls.load()),
                static_cast<unsigned long long>(g_widenWritten.load()));
    ImGui::Text("  last attempt ranges: calls %u, widened %u, valid %d",
                le.ranges.calls, le.ranges.widened, le.ranges.valid ? 1 : 0);
    ImGui::Text("  orig  heightMin %.1f horizMax %.1f distMax %.1f durExt %.1f curveMax %.1f curveMin %.1f expCurveMin %.1f",
                static_cast<double>(le.ranges.orig[0]), static_cast<double>(le.ranges.orig[1]), static_cast<double>(le.ranges.orig[2]),
                static_cast<double>(le.ranges.orig[3]), static_cast<double>(le.ranges.orig[4]), static_cast<double>(le.ranges.orig[5]),
                static_cast<double>(le.ranges.orig[6]));
    ImGui::Text("  now   heightMin %.1f horizMax %.1f distMax %.1f durExt %.1f curveMax %.1f curveMin %.1f expCurveMin %.1f",
                static_cast<double>(le.ranges.now[0]), static_cast<double>(le.ranges.now[1]), static_cast<double>(le.ranges.now[2]),
                static_cast<double>(le.ranges.now[3]), static_cast<double>(le.ranges.now[4]), static_cast<double>(le.ranges.now[5]),
                static_cast<double>(le.ranges.now[6]));
}

void DrawMenu()
{
    ImGui::Text("BackEjectLoF 0.27.1 (probe inspector, slots 0x4B, 0x21, 0x25, LoF 0x76/0x73, eject 0x76/0x64)");
    ImGui::Text("Action hook: installed %zu / %zu, skipped %zu", g_installedCount, kRowCount, g_skippedCount);
    ImGui::Text("Tester hook: installed %zu / %zu, skipped %zu; back-eject hook: %s", g_tInstalledCount, kTesterRowCount,
                g_tSkippedCount, g_backInstalled ? "installed" : "not installed");
    ImGui::Text("dropped records %llu, faults %u",
                static_cast<unsigned long long>(g_dropped.load()), static_cast<unsigned>(g_faults.load()));

    if (g_recording) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "RECORDING (%llu records, %zu distinct, %llu overflow drops)",
                           static_cast<unsigned long long>(g_sessionRecords), g_session.size(),
                           static_cast<unsigned long long>(g_sessionOverflow));
    } else if (g_lastSessionValid) {
        ImGui::Text("Last session: %llu records, %zu distinct (report in log)",
                    static_cast<unsigned long long>(g_sessionRecords), g_session.size());
    } else {
        ImGui::TextDisabled("Not recording. F7 or the button starts a session.");
    }
    if (ImGui::Button(g_recording ? "Stop recording (F7)" : "Start recording (F7)")) {
        ToggleSession(g_frame.load());
    }
    ImGui::Separator();

    bool capture = g_capture.load();
    if (ImGui::Checkbox("Capture", &capture)) g_capture.store(capture);
    ImGui::SameLine();
    ImGui::Text("recent records: %zu (last %llu frames)", g_recent.size(), static_cast<unsigned long long>(kRecentFrames));
    bool npcs = g_includeNpcs.load();
    if (ImGui::Checkbox("Include NPCs (live tables and next session report)", &npcs)) g_includeNpcs.store(npcs);

    if (g_hasPlayer) ImGui::Text("Player position: %.2f, %.2f, %.2f", g_playerPos[0], g_playerPos[1], g_playerPos[2]);
    else ImGui::TextDisabled("Player position: none (no world yet)");

    ImGui::InputText("Probe types (empty = all)", g_typeText, sizeof(g_typeText));
    ImGui::SliderFloat("Max distance from player (m)", &g_maxDistance, 1.0f, 300.0f, "%.0f");
    ImGui::Separator();

    const std::set<uint32> types = ParseTypes(g_typeText);
    std::vector<Record> view;
    for (const Record& r : g_recent) if (ViewOk(r.isPlayer)) view.push_back(r);
    const std::vector<Record> distinct = Distinct(view);
    std::vector<const Record*> shown;
    size_t matched = 0;
    for (const Record& r : distinct) {
        if (!PassesType(types, r.type) || !PassesDistance(r)) continue;
        ++matched;
        if (shown.size() < kMaxTableRows) shown.push_back(&r);
    }
    ImGui::Text("Distinct probes matching filter: %zu (showing %zu)", matched, shown.size());
    DrawProbeTable(shown);

    ImGui::Separator();
    ImGui::Text("Action enum x probe type (raw records, NPC setting applied):");
    DrawCountTable(view);

    ImGui::Separator();
    ImGui::Text("Parkour testers (gather hook, last %llu frames):", static_cast<unsigned long long>(kRecentFrames));
    DrawTesterTable();

    ImGui::Separator();
    DrawExpC();
    ImGui::Separator();
    DrawExpE();
    ImGui::Separator();
    origins::Menu();
    DrawTrajMenu();                     // v0.16 (J)
    camdiag::Menu();                    // v0.19 (K)
    lofcam::Menu();                     // v0.20 (L)
    haycam::Menu();                     // v0.23 (M)
    graphvars::Menu();                  // v0.25 (O)
    statelog::Menu();                   // v0.26 (P)
    ctorprobe::Menu();                  // v0.27 (Q)

    ImGui::Separator();
    if (ImGui::Button("Write snapshot")) WriteSnapshot("button");
    ImGui::SameLine();
    if (ImGui::Button("Clear")) { g_recent.clear(); g_gatherRecent.clear(); }
    if (GetTickCount64() < g_msgUntilMs) ImGui::TextUnformatted(g_msgText);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------------------
class BackEjectLoFPlugin : public ACUPluginInterfaceVirtuals
{
public:
    virtual uint64 GetThisPluginVersion() override { return MAKE_VERSION_NUMBER_UINT64(0, 16, 0, 0); }

    virtual void InitStage_WhenPluginAPIDeemedCompatible() override
    {
        fs::create_directories(AbsolutePathInThisDLLDirectory("BackEjectLoF"));
        g_log.open(AbsolutePathInThisDLLDirectory("BackEjectLoF\\BackEjectLoF-log.log"), std::ios::trunc);
        LogLine("BackEjectLoF 0.27.1 loaded (slots 0x4B, 0x21, 0x25, LoF 0x76/0x73, eject 0x76/0x64; experiment C default OFF, E default ON; trajectory recorder default ON)");
        origins::Load();
        g_drain.reserve(kRingSize);
        g_recent.reserve(kRingSize * 4);
        g_gDrain.reserve(kGatherRingSize);
        g_gatherRecent.reserve(kGatherRingSize * 4);
        g_samples.reserve(kMaxSamples);
        g_traj.reserve(kTrajMaxSamples);     // v0.16 (J)
        g_act.reserve(kTrajMaxSamples);      // v0.24 (N)
        graphvars::Init();                   // v0.25 (O): ring allocated once (about 1.2 MB)
    }

    virtual bool InitStage_WhenCodePatchesAreSafeToApply(ACUPluginLoaderInterface&) override
    {
        InstallProbeHooks();
        InstallTesterHooks();
        InstallBackEjectHook();
        InstallLofHooks();
        InstallEjectFinalHook();
        InstallEjectRangeHook();
        // v0.22: lofcam (section L) disabled: the user rejected the forced LoF camera (v0.21). Without Install no rule is ever swapped.
        // v0.24: haycam (section M) disabled: the user rejected it ("camera is still broken"). Without Install no rule is swapped.
        statelog::Install();                 // v0.26 (P): verifies the 7 bytes at 0x1427554E3, then activates the hook
        ctorprobe::Install();                // v0.27 (Q): verifies the 9 bytes at each constructor, then activates its hook
        return true;
    }

    virtual void EveryFrameEvenWhenMenuIsClosed() override
    {
        GameFrame();
    }

    virtual void EveryFrameWhenMenuIsOpen() override
    {
        DrawMenu();
    }
} g_backEjectLoFPlugin;
