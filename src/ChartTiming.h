#pragma once

#include <unordered_map>
#include <vector>

#include "ChartFile.h"
#include "LaneConfig.h"

// Pure, stateless timing/note-onset arithmetic shared by the live game
// (GameSession) and the editor's analytical block scheduler
// (src/editor/BlockSchedule.h) - moved here (rather than staying private to
// GameSession) so the two can never compute different answers for the same
// inputs. Every function here operates only on its parameters, with no
// wall-clock/instance-state dependency of any kind.
//
// All of it rests on one invariant, enforced by ValidateArrangementAlignment
// at load time rather than re-derived per call: every clip's spanBeats is a
// whole number of bars, and every clip, whenever it joins a group of other
// clips already sounding together (an "arrangement"), starts on one of its
// own bar boundaries measured from that arrangement's single shared origin -
// the wall-clock beat/second the arrangement's first clip began at. That
// makes originBeat/originSeconds below always the *arrangement's* origin,
// never a per-clip one: two clips sounding together necessarily agree on it,
// and a clip starting fresh still lands on its own true pattern beginning
// (see NextOnsetAfter) without needing a separate "first start" formula.
namespace ChartTiming
{

// Returns the smallest note start (in absolute beats) strictly after
// afterBeat, for this lane - measured relative to originBeat, the current
// arrangement's shared origin (see the namespace comment), not absolute beat
// 0. Also what to call for a clip's very first onset in a fresh arrangement:
// querying at (joinBeat - epsilon) returns joinBeat + this lane's own first
// note, since the alignment invariant guarantees joinBeat already sits
// exactly on one of this clip's own cycle boundaries from originBeat.
double NextOnsetAfter(double originBeat, double afterBeat, const ChartClip& clip, int lane);

// Computes the wall-clock second at which loopCount full loops have
// completed, counted from loopStartSeconds, given a stem of stemDuration
// seconds - measured relative to originSeconds, the current arrangement's
// shared origin (see the namespace comment), so a loop boundary always lands
// exactly where the real, phase-seeked audio actually wraps back to its own
// beginning. Shared by a learn section's own advance floor and a break
// section's unconditional wait.
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
// originSeconds is the current arrangement's shared origin (see the
// namespace comment) - sectionStartSeconds and loopStartSeconds are measured
// on that same absolute-wall-clock timeline (not pre-shifted by the caller).
double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double loopStartSeconds,
                                   double stemDuration, int loopCount, double tFallSeconds);

// Mirrors GameSession::BeginSection's Break-case formula: a break's
// loop_count is already known when it starts (unlike a learn clip's, which
// depends on future player input), so rather than waiting an arbitrary
// pause on top of it, the loop count itself is extended upward until the
// clip's own audio covers at least tFallSeconds - the voice then stops
// itself naturally and sample-accurately once its (possibly-extended)
// loops are done, instead of leaving real silence for whatever's left of
// the wait. originSeconds is the current arrangement's shared origin (see
// the namespace comment) - loopStartSeconds is measured on that same
// absolute-wall-clock timeline.
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
// measured relative to originSeconds (the current arrangement's shared
// origin - see the namespace comment), so it enters exactly in sync with the
// clip's own note pattern's beat grid rather than the raw audio file's own
// measured duration. The two are close but not necessarily bit-identical -
// real WAV export rarely lands a stem's sample count on an exact beat
// boundary (see ClipFitsOneLoop's own tolerance for the same reason) - and
// using the raw audio duration as the phase modulus lets that tiny per-loop
// imprecision compound into an audible drift over many loops. Returns
// exactly 0 (the clip's own true beginning) when nowSeconds lands on one of
// its cycle boundaries from originSeconds - always true for a fresh join,
// per the alignment invariant. Uses clip.spanBeats (the pattern's own exact
// cycle length, converted via the song's current bpm) whenever the clip
// actually has a pattern to sync to (hasMidi); falls back to the audio's own
// raw duration for a clip with no pattern at all (pure Break/Background
// material with no judged notes), since there's nothing to synchronize with
// beat-wise in that case - that fallback's own imprecision is harmless
// there, since nothing else is ever compared against it.
double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, const ChartClip& clip, double stemDuration,
                                double bpm);

// Returns every onset (this lane's own pattern, tiled every spanBeats from
// originBeat - same convention as NextOnsetAfter) whose absolute beat falls
// in (afterBeatExclusive, uptoBeatInclusive], ascending. Unlike
// NextOnsetAfter (single next onset) this can return several - or none -
// depending on how far apart the two bounds are; used by GameSession::
// Update() to auto-score every note a Pass-mode section's locked-in clip has
// crossed since the last tick, without re-deriving NextOnsetAfter's own
// bar-tiling math a second time. Deliberately onsets only, not spans/overlap
// - NoteLaneModel's own NotesInRange is a separate, rendering-only function
// with a different contract and a different consumer; this one stays in
// ChartTiming so GameSession can reach it without depending on
// NoteLaneModel.h (which itself depends on GameSession.h - see
// NoteLaneModel.h's own include). Returns an empty vector if notes is empty,
// spanBeats <= 0, or the range is empty/inverted.
std::vector<double> OnsetsInRange(double originBeat, double afterBeatExclusive, double uptoBeatInclusive,
                                   const std::vector<LaneNote>& notes, double spanBeats);

// A judged clip's own timing inputs to ValidateArrangementAlignment - kept
// deliberately separate from ChartClip::spanBeats itself, since that field
// gets overwritten by ExpandLaneNotesToFillClip to the raw audio's own
// measured length whenever a clip's declared pattern is shorter than its
// stem (e.g. an 8-beat riff authored once but rendered into a 16-beat stem
// that repeats it twice, so the game gets one continuous, seamless-loop
// audio file instead of two - a real, legitimate, common authoring shape).
// Always capture authoredSpanBeats/stemDurationSeconds BEFORE calling
// ExpandLaneNotesToFillClip, never after.
//
// Whenever a clip widens this way, ValidateArrangementAlignment needs BOTH
// its authored length and its real one, for two genuinely different roles a
// clip can play, at different points in the very same chart:
//   - As the clip a Learn/Break section is CURRENTLY PLAYING: what governs
//     when that section's own advance can happen (and so what a later,
//     ambiguous-position clip's join must align to) is the real audio's own
//     wrap length, never the shorter authored one - ComputeLearnAdvanceSeconds/
//     ComputeBreakAdvance always step forward in whole multiples of
//     stemDuration, so a section can only ever hand off at a multiple of the
//     clip's real length, even though its two (or more) repeats are
//     identical.
//   - As the clip JOINING an arrangement: what matters for ITS OWN judging/
//     audio-phase correctness is only its authored length - starting
//     partway into its own widened audio buffer (at an exact multiple of
//     the authored repeat, just not of the whole widened length) is
//     inaudible and judges identically, since every repeat inside that
//     buffer is the same content.
// Get this backwards - using the widened length for a joining clip's own
// check, or the authored length for what a just-finished section's own
// advance actually steps by - and the check either rejects a perfectly safe
// chart or accepts a genuinely unsafe one.
struct ClipAlignmentInfo
{
    // The clip's own bar-aligned pattern length, exactly as ChartFile::Load
    // (AlignToBarBoundary) produced it - before any ExpandLaneNotesToFillClip
    // widening.
    double authoredSpanBeats = 0.0;
    // The clip's real, AudioEngine-measured stem duration.
    double stemDurationSeconds = 0.0;
};

// Checks the whole-chart invariant every function above relies on (see the
// namespace comment): every clip's own authored length (ClipAlignmentInfo,
// not ChartClip::spanBeats post-widening) is a whole number of bars, and
// every clip lands on one of its own bar boundaries wherever it actually
// joins an arrangement - see ClipAlignmentInfo's own comment for exactly
// which of a clip's two lengths (authored vs. real/widened) governs which
// side of that check. Call once every judged clip's real stem duration is
// known, but before ExpandLaneNotesToFillClip runs on any of them - clipInfo
// is keyed by address into song.clips, same convention as BlockSchedule::
// Build; a clip with no entry (or hasMidi false) is treated as having no
// pattern to align - nothing else in the chart needs to relate to it
// bar-wise.
//
// A join's exact beat is only ever ambiguous in one situation: right after a
// Learn section, whose real advance depends on how many extra loops an
// actual player needs to reach hits_required - unbounded, and not knowable
// at load time. Rather than assert something that could only fail live
// (mid-song, for a real player), that case is instead restricted to what's
// true for *every* possible loop count: whatever joins immediately after a
// Learn section (directly, or through a chain of zero-duration Background/
// Reset sections) must have an authored length that evenly divides that
// Learn clip's own real (possibly widened) length - never a longer,
// unrelated span. Every other transition (Break, Background realized after
// a Break/Reset, Reset itself) has a fully deterministic advance, so it's
// checked exactly.
//
// Returns false with outErrors describing every violation found (which two
// clips, and why) if the chart can't satisfy this - the chart author needs
// to fix the chart; nothing here is recoverable at runtime.
bool ValidateArrangementAlignment(const ChartSong& song,
                                   const std::unordered_map<const ChartClip*, ClipAlignmentInfo>& clipInfo,
                                   std::vector<std::wstring>& outErrors);

} // namespace ChartTiming
