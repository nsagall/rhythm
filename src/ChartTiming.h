#pragma once

#include "ChartFile.h"
#include "LaneConfig.h"

// Pure, stateless timing/note-onset arithmetic shared by the live game
// (GameSession) and the editor's analytical block scheduler
// (src/editor/BlockSchedule.h) - moved here (rather than staying private to
// GameSession) so the two can never compute different answers for the same
// inputs. Every function here operates only on its parameters, with no
// wall-clock/instance-state dependency of any kind.
namespace ChartTiming
{

// Returns the smallest note start (in absolute beats) strictly after afterBeat, for this lane.
double NextOnsetAfter(double afterBeat, const ChartClip& clip, int lane);

// Returns the absolute beat of this lane's note in the first full pattern
// cycle that starts at or after afterBeat, preserving every lane's
// authored relative offset within that shared cycle - used only for the
// song's very first note(s), where independent per-lane anchoring (see
// NextOnsetAfter) risks landing different lanes on different pattern
// cycles and corrupting notes that are meant to land together (or apart)
// exactly as authored.
double FirstReachableOnset(double afterBeat, const ChartClip& clip, int lane);

// Computes every lane's anchor for a clip's very first judged note in one
// call. Tries each lane's own next reachable note first (NextOnsetAfter,
// searched independently per lane) and uses those directly if they all
// land in the same pattern cycle (no desync risk in that case); falls back
// to FirstReachableOnset's slower, always-safe per-lane behavior only when
// the lanes' own candidates would actually land in different cycles.
void FirstReachableOnsetForAllLanes(double afterBeat, const ChartClip& clip, double outBeats[kLaneCount]);

// Computes the wall-clock second at which loopCount full loops have
// completed, counted from loopStartSeconds, given a stem of stemDuration
// seconds. Shared by a learn section's lock-in floor, a break section's
// unconditional wait, and a background layer's self-stop time.
double ComputeLoopFloorSeconds(double loopStartSeconds, double stemDuration, int loopCount);

// If clip's declared spanBeats is shorter than one full loop of its actual
// audio (stemDurationSeconds, at the song's bpm), tiles each lane's notes
// to fill the whole loop and widens spanBeats to match, so a repeating
// pattern loops on the audio's own bar boundary instead of shrinking to
// "last note's end" and leaving the back half of every loop silent/
// ungraded. A trailing repeat that would be cut off mid-note (its start
// fits before the loop wraps but its duration wouldn't finish in time)
// drops that note rather than shipping one that could never be
// legitimately pressed and released. No-op if clip's span already fills
// (or exceeds) one loop.
void ExpandLaneNotesToFillClip(ChartClip& clip, double stemDurationSeconds, double bpm);

// The reverse case ExpandLaneNotesToFillClip doesn't handle: true if
// clip's declared spanBeats fits within one loop of stemDurationSeconds
// (small floating-point tolerance included), false if the MIDI pattern is
// longer than a single loop of the audio - meaning the audio would already
// have wrapped back to its start before the pattern's own last notes are
// reached, so notes would be judged against a moment the audio isn't
// actually at anymore. Both the live game (GameSession::LoadChart) and the
// editor's analytical scheduler reject a clip that fails this before doing
// anything else with its notes.
bool ClipFitsOneLoop(const ChartClip& clip, double stemDurationSeconds, double bpm);

// Mirrors GameSession::SchedulePendingAdvance's formula exactly: given a
// learn section's lock-in instant (the wall-clock second its shared streak
// met hits_required), the clip's actual loop-start instant, its measured
// stem duration, its declared loop_count, and the required preview lead
// time (tFallSeconds, == kNoteFallBeats worth of seconds), returns the
// second at which the section should hand off to the next one - the next
// loop boundary at/after lock-in, floored by loop_count's own minimum, and
// extended by whole loops until at least tFallSeconds separates lock-in
// from the hand-off.
double ComputeLearnAdvanceSeconds(double lockInSeconds, double loopStartSeconds, double stemDuration, int loopCount,
                                   double tFallSeconds);

// Mirrors GameSession::BeginSection's Break-case formula: a break's
// loop_count is already known when it starts (unlike a learn clip's, which
// depends on future player input), so rather than waiting an arbitrary
// pause on top of it, the loop count itself is extended upward until the
// clip's own audio covers at least tFallSeconds - the voice then stops
// itself naturally and sample-accurately once its (possibly-extended)
// loops are done, instead of leaving real silence for whatever's left of
// the wait.
struct BreakAdvance
{
    int loopCount = 1;
    double advanceSeconds = 0.0;
};
BreakAdvance ComputeBreakAdvance(double loopStartSeconds, double stemDuration, int requestedLoopCount,
                                  double tFallSeconds);

// Computes the phase (seconds, 0..cycleDurationSeconds) a clip's audio
// should seek to when starting to play at absolute elapsed time
// nowSeconds, so it enters exactly in sync with the clip's own note
// pattern's beat grid rather than the raw audio file's own measured
// duration. The two are close but not necessarily bit-identical - real
// WAV export rarely lands a stem's sample count on an exact beat boundary
// (see ClipFitsOneLoop's own tolerance for the same reason) - and using
// the raw audio duration as the phase modulus lets that tiny per-loop
// imprecision compound: over the many loops elapsed by the time a clip
// starts late in a long song, the accumulated drift between "where the
// judged notes say the pattern is" (always exact, beat-based) and "where
// fmod(nowSeconds, stemDuration) says it is" can become large enough that
// the clip audibly starts partway through its own loop - even near the
// very end - instead of at its pattern's true beginning. Uses
// clip.spanBeats (the pattern's own exact cycle length, converted via the
// song's current bpm) whenever the clip actually has a pattern to sync to
// (hasMidi); falls back to the audio's own raw duration for a clip with
// no pattern at all (pure Break/Background material with no judged
// notes), since there's nothing to synchronize with beat-wise in that
// case - that fallback's own imprecision is harmless there, since nothing
// else is ever compared against it.
double ComputeClipPhaseSeconds(double nowSeconds, const ChartClip& clip, double stemDuration, double bpm);

} // namespace ChartTiming
