# Pulse Macro v1.4.6

Candidate-aware frame analysis: first failure on each side ends that side, and hidden checkpoints are placed/selected closer to the exact shifted click while preserving validated native-state restores and safe fallbacks. Dense checkpoint validation now supports overlapping 60-tick verification windows.

Pulse Macro v1.4.6 with deterministic full-route playback auto-repair and input-aware checkpoint analysis.

Frame-window analysis builds hidden analyzer-owned native checkpoints near the actual macro inputs. Universal checkpoints remain safe for the full ±20-tick search, while candidate-aware selection can use closer checkpoints for +frames and shallow -frames without restoring past the changed event. The default/hard cache limit is 256 checkpoints and can be lowered in settings. Candidate trials restore a nearby checkpoint, replay only the short suffix, shift one input, and still live or die under natural 240 Hz physics.

Checkpoint restores are verified before first use. Unreliable snapshots fall back to an earlier usable checkpoint or the level start instead of aborting the whole run. Version 1.4.5 also includes the v1.4.1 current-checkpoint/full-reset hardening, 0.75-second periodic fallback mode, runtime/ETA display, and nonfatal uncertainty warnings.

Standalone/core tests pass 6/6. Target: Geometry Dash 2.2081 / Geode 5.10.1 / Win64. A GitHub Actions Win64 build workflow is included.

Pulse Macro v1.3.12 nonfatal uncertainty hotfix.

Frame-window analysis no longer aborts when a failed shifted-input boundary changes outcome or failure tick on its one-update-at-a-time double-check. The boundary is treated conservatively as failed using the first observed failure tick, analysis continues, and the run is marked uncertain. When analysis finishes, Pulse displays a warning that the saved results COULD be inaccurate. Stable double-checks behave exactly as before; baseline/scheduler/TPS errors are still fatal because silently continuing through those could corrupt every window.

Pulse Macro v1.3.11 candidate-reset hotfix.

The 16:15 log proved the first -1f candidate actually ran and failed at tick 91, then the following reset hit the old step-60 pre-shift pose guard. Candidate trials now re-establish the sampled pre-shift reset trajectory on every Early/Late trial instead of requiring every reset to match the previous reset byte-for-byte. This does not position-correct the player and does not make a shifted input pass: after the shifted input, the candidate still lives or dies under natural physics. The opening baseline and final baseline remain the determinism guards for the saved reference, and failed boundaries are still repeated for consistency.

Pulse Macro v1.3.10 reset-canonical hotfix. Frame-window analysis now re-establishes the canonical pre-input trajectory on the first real candidate reset. This addresses the observed step-60 mismatch that appears immediately after a successful full baseline, without disabling pre-shift validation: only poses strictly before the first shifted input are re-anchored, and every later candidate plus the final baseline remains strict.

# Pulse Macro 1.3.9

**1.3.9 completion fallback:** frame analysis no longer requires `PlayLayer::levelComplete` to fire during the hidden/test-mode replay. A trial succeeds when GD reports completion **or** the replay survives through the exact recorded endpoint. This fixes the case where the analyzer visibly reaches 100% and then incorrectly reports `Reference replay desynced`. Deaths still fail normally, shifted-input physics are still natural/uncorrected, and baseline pose validation remains active.

# Pulse Macro 1.3.6

One mod for macros and built-in Frame Windows. Frame-window validation now ignores dormant Player 2 coordinates/velocity outside dual mode, while still validating both players during actual dual gameplay. This fixes false "Replay differs before the shifted input" aborts caused by GD resetting inactive P2 state. Position checks now ignore dormant player 2 outside dual mode. Future mismatch messages report the step and active-player distances. Allow small position drift is enabled by default: playback continues within 5 game units in distance, or your larger Position tolerance, without correcting positions. Timing checks, larger mismatches, and deaths still stop playback; analysis validation is unchanged. Pausing preserves the active take; Resume continues it. P is an additional pause/resume key. Saved frame references now reload correctly for the selected macro slot.

**F6** starts both recordings from zero. Practice deaths keep recording active; checkpoint restores rewind both timelines. Create checkpoints after F6. **F8** saves; completion also saves. Leave practice before **F7** playback.

**Pause → Replay** has names, 20 slots, New Recording, Save, Delete, and Undo Delete.

Saving starts analysis by default. Turn off **Analyze after Pulse saves** while building a route if desired. **F4** opens Analyze / Cancel / Input details. **F3** toggles the counter. Analysis resets the level and clears native checkpoints.

Wave windows now measure survival until the next direction change: press OR release. Other modes use the next press. Existing recordings learn mode during baseline replay. Later failures do not narrow an earlier click. The last click uses the recording end. Input details show early/late offsets, failed boundary ticks, and nominal milliseconds at 240 Hz. ? means unmeasured; 21+ is a lower bound. Failed boundaries are repeated one update at a time; unstable results are rejected. Baseline errors display a reason. Restart and use F4 → Analyze again to replace old widths; existing reference inputs remain usable.

Keep 240 Hz physics, CBF disabled with Physics Bypass off, and other bots/Lock Delta/frame extrapolation off. Speedhack compatibility uses fixed game-time steps.

Remove the older standalone Frame Window Lab package and restart GD. Existing 1.3.0 references remain supported. References from the older separate mods used a different clock and need a new F6 recording.

Windows x64 / GD 2.2081 / Geode 5.10.1. Five automated suites pass; combined UI, native practice restoration, and Mega Hack speedhack remain unverified live.

Rated levels use normal input/completion paths. Credit depends on GD and other mods; automation is not a human run.








v1.4.5 preserves fully measured runs when only the final post-analysis validation replay fails, while clearly marking them unverified.


### v1.4.6 playback repair
A reproducible playback death can now trigger a bounded nearby-frame search. Pulse retries the original route first, then tests recent jump transitions at nearby 240 Hz offsets. A candidate only passes if the full remaining macro survives; the saved repair uses the center of the widest passing offset region and is confirmed once more. The successful confirmation trajectory replaces the stale recorded positions, and the pre-repair macro is kept as `.bak`.
