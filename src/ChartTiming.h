#pragma once

#include <vector>

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

// Returns the smallest note start (in absolute beats) strictly after
// afterBeat, for this lane - measured relative to originBeat, the beat this
// clip's own pattern was first ever anchored at (see FreshOnsetForAllLanes),
// not absolute beat 0. A clip's cycle boundaries are fixed wherever it
// actually first started playing, however that lines up (or doesn't) with
// the song's own beat-0-aligned grid - so every later call against the same
// clip must keep passing that SAME originBeat, or its groove would snap onto
// cycle boundaries it was never actually phase-locked to.
double NextOnsetAfter(double originBeat, double afterBeat, const ChartClip& clip, int lane);

// Computes every lane's anchor for a clip's very first-ever start, given the
// exact beat that start happens at (originBeat): each lane's own earliest
// note in the pattern, offset by originBeat - so the clip begins playing
// (and judging) from its own true beginning the instant it starts, with no
// wait for a pattern-cycle boundary, regardless of where originBeat happens
// to land in the song's overall beat grid. A lane with no notes at all gets
// originBeat + clip.spanBeats, matching NextOnsetAfter's own placeholder
// convention for an empty lane. originBeat becomes this clip's own
// persistent phase reference from here on - every later NextOnsetAfter (or
// ComputeClipPhaseSeconds/ComputeLearnAdvanceSeconds/ComputeBreakAdvance)
// call against the same clip must keep using this exact value (converted to
// seconds where needed).
void FreshOnsetForAllLanes(double originBeat, const ChartClip& clip, double outBeats[kLaneCount]);

// Computes the wall-clock second at which loopCount full loops have
// completed, counted from loopStartSeconds, given a stem of stemDuration
// seconds - measured relative to originSeconds, the wall-clock second this
// clip's own pattern was first ever anchored at (== originBeat converted to
// seconds - see FreshOnsetForAllLanes), so a loop boundary always lands
// exactly where the real, phase-seeked audio actually wraps back to its own
// beginning, not wherever a beat-0-aligned grid would put it. Shared by a
// learn section's own advance floor and a break section's unconditional wait.
double ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration, int loopCount);

// If clip's declared spanBeats is shorter than one full loop of its actual
// audio (stemDurationSeconds, at the song's bpm), widens spanBeats to
// match the audio's own length - so a clip's loop boundary always lands
// where the real, phase-seeked audio actually wraps, not wherever the
// last MIDI note (or AlignToBarBoundary's own rounding) happened to end.
// Only re-tiles the pattern into the newly-widened space in whole
// repeats - a repeat whose own full span wouldn't entirely fit before the
// audio wraps is left out rather than partially included, since a
// leftover stretch shorter than one full pattern is far more often a
// deliberate trailing pause (a MIDI file with a bar of silence at the
// end, matching a stem that has a bar of room-tone/reverb tail past the
// last note) than an intentional partial repeat - tiling one in would
// silently invent notes the chart author never placed, appearing to the
// player as the pattern looping early while the audio is still finishing
// its real first pass. A clip whose pattern is meant to legitimately
// repeat multiple times within one audio loop (e.g. a 2-bar riff filling
// an 8-bar stem) is unaffected: every one of its repeats fits whole. No-op
// if clip's span already fills (or exceeds) one loop.
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

// Mirrors GameSession::BeginSection's Learn-case formula exactly: given the
// wall-clock second the section itself began (a Learn section now starts
// its clip and schedules its own advance immediately, exactly like Break -
// whether the player ever meets hits_required doesn't affect this at all,
// only the glow/volume-switch treatment does), the clip's actual loop-start
// instant, its measured stem duration, its declared loop_count, and the
// required preview lead time (tFallSeconds, == kNoteFallBeats worth of
// seconds), returns the second at which the section should hand off to the
// next one - the next loop boundary at/after the section's own start,
// floored by loop_count's own minimum, and extended by whole loops until at
// least tFallSeconds separates the section's start from the hand-off.
// originSeconds is this clip's own persistent phase reference (see
// FreshOnsetForAllLanes) - both sectionStartSeconds and loopStartSeconds are
// measured on the same absolute-wall-clock timeline as originSeconds itself
// (they are NOT pre-shifted by the caller).
double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double loopStartSeconds,
                                   double stemDuration, int loopCount, double tFallSeconds);

// Mirrors GameSession::BeginSection's Break-case formula: a break's
// loop_count is already known when it starts (unlike a learn clip's, which
// depends on future player input), so rather than waiting an arbitrary
// pause on top of it, the loop count itself is extended upward until the
// clip's own audio covers at least tFallSeconds - the voice then stops
// itself naturally and sample-accurately once its (possibly-extended)
// loops are done, instead of leaving real silence for whatever's left of
// the wait. originSeconds is this clip's own persistent phase reference
// (see FreshOnsetForAllLanes) - loopStartSeconds is measured on the same
// absolute-wall-clock timeline as originSeconds itself.
// ComputeBreakAdvance's result: the loop count it settled on (possibly
// larger than the caller's requested one - see the function's own comment
// above) and the wall-clock second the section should advance at, given
// that final loop count.
struct BreakAdvance
{
    int loopCount = 1;
    double advanceSeconds = 0.0;
};
BreakAdvance ComputeBreakAdvance(double originSeconds, double loopStartSeconds, double stemDuration,
                                  int requestedLoopCount, double tFallSeconds);

// Extends advanceSeconds forward by whole stemDuration-sized steps (never
// fewer than one, if it extends at all) until it clears referenceSeconds by
// at least tFallSeconds - the shared "not enough preview lead time yet"
// rule behind two different callers: GameSession::RegisterHit applies this
// reactively, the instant a Learn section's own IsPassing() flips true
// (referenceSeconds == "now"), to fix up an already-scheduled advance that
// turned out too close to the section's own start; BlockSchedule::Build's
// Learn case applies the equivalent closed-form computation up front for a
// hypothetical perfect player (referenceSeconds == that player's own
// lockInSeconds). Both need the exact same guarantee - the section's own
// advance must never land less than tFallSeconds after passing was
// reached, so the next section's own preview gets its full on-screen lead
// time - just computed at different moments against different reference
// instants. No-op (returns advanceSeconds unchanged) if stemDuration <= 0 -
// nothing to extend by. Not the same rule as ComputeLearnAdvanceSeconds's
// own internal extension (referenceSeconds there is always
// sectionStartSeconds, never lockInSeconds/"now") - a different guarantee,
// not a duplicate of this one, even though the shape of the loop is
// identical.
double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
                                     double tFallSeconds);

// Computes the phase (seconds, 0..cycleDurationSeconds) a clip's audio
// should seek to when starting to play at absolute elapsed time nowSeconds,
// measured relative to originSeconds (this clip's own persistent phase
// reference - see FreshOnsetForAllLanes), so it enters exactly in sync with
// the clip's own note pattern's beat grid rather than the raw audio file's
// own measured duration. The two are close but not necessarily bit-identical
// - real WAV export rarely lands a stem's sample count on an exact beat
// boundary (see ClipFitsOneLoop's own tolerance for the same reason) - and
// using the raw audio duration as the phase modulus lets that tiny per-loop
// imprecision compound: over the many loops elapsed since originSeconds by
// the time a clip re-enters late in a long song, the accumulated drift
// between "where the judged notes say the pattern is" (always exact,
// beat-based) and "where fmod(nowSeconds-originSeconds, stemDuration) says
// it is" can become large enough that the clip audibly starts partway
// through its own loop - even near the very end - instead of at its
// pattern's true beginning. For a first-ever start, nowSeconds ==
// originSeconds exactly, so this always returns 0 - the clip's own true
// beginning, with zero wait. Uses clip.spanBeats (the pattern's own exact
// cycle length, converted via the song's current bpm) whenever the clip
// actually has a pattern to sync to (hasMidi); falls back to the audio's own
// raw duration for a clip with no pattern at all (pure Break/Background
// material with no judged notes), since there's nothing to synchronize with
// beat-wise in that case - that fallback's own imprecision is harmless
// there, since nothing else is ever compared against it.
double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, const ChartClip& clip, double stemDuration,
                                double bpm);

// Returns every onset (this lane's own pattern, tiled every spanBeats from
// originBeat - same convention as NextOnsetAfter/FreshOnsetForAllLanes)
// whose absolute beat falls in (afterBeatExclusive, uptoBeatInclusive],
// ascending. Unlike NextOnsetAfter (single next onset) this can return
// several - or none - depending on how far apart the two bounds are; used by
// GameSession::Update() to auto-score every note a Pass-mode section's
// locked-in clip has crossed since the last tick, without re-deriving
// NextOnsetAfter's own bar-tiling math a second time. Deliberately onsets
// only, not spans/overlap - NoteLaneModel's own NotesInRange is a separate,
// rendering-only function with a different contract and a different
// consumer; this one stays in ChartTiming so GameSession can reach it
// without depending on NoteLaneModel.h (which itself depends on
// GameSession.h - see NoteLaneModel.h's own include). Returns an empty
// vector if notes is empty, spanBeats <= 0, or the range is empty/inverted.
std::vector<double> OnsetsInRange(double originBeat, double afterBeatExclusive, double uptoBeatInclusive,
                                   const std::vector<LaneNote>& notes, double spanBeats);

} // namespace ChartTiming
