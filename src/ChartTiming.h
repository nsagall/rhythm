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

// Returns this lane's next note onset (absolute beats) strictly after afterBeat.
//   originBeat - current arrangement's shared origin (see namespace comment), not absolute beat 0.
//   afterBeat  - returned onset is strictly after this beat.
//   clip       - clip whose pattern to tile.
//   lane       - which lane to search.
// Also covers a clip's very first onset in a fresh arrangement: querying at (joinBeat - epsilon)
// returns joinBeat + this lane's own first note, since the alignment invariant guarantees
// joinBeat already sits on one of this clip's own cycle boundaries from originBeat.
double NextOnsetAfter(double originBeat, double afterBeat, const ChartClip& clip, int lane);

// Computes the wall-clock second at which loopCount full loops complete.
//   originSeconds    - current arrangement's shared origin (see namespace comment).
//   loopStartSeconds - wall-clock second the clip's loop actually started.
//   stemDuration     - length of one loop of the clip's audio, in seconds.
//   loopCount        - number of full loops to count.
// Measured relative to originSeconds so the boundary lands where the real, phase-seeked audio
// actually wraps, not on a beat-0-aligned grid. Shared by Learn's advance floor and Break's wait.
double ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration, int loopCount);

// Widens a clip's declared spanBeats to match its actual audio length, if shorter.
//   clip                - [in/out] clip to widen; its lane notes are re-tiled into the new span.
//   stemDurationSeconds - clip's measured audio length.
//   bpm                 - song's tempo, to convert stemDurationSeconds to beats.
// Only whole pattern repeats are re-tiled in; a trailing partial repeat is left out, since a
// leftover shorter than one full pattern is usually a deliberate silent tail (room tone/reverb
// past the last note) rather than an intended partial repeat - tiling one in would invent notes
// the chart author never placed. A pattern that legitimately repeats several whole times within
// one audio loop is unaffected. No-op if the pattern already fills (or exceeds) one loop.
void ExpandLaneNotesToFillClip(ChartClip& clip, double stemDurationSeconds, double bpm);

// Checks whether a clip's pattern fits within one loop of its audio (the reverse case
// ExpandLaneNotesToFillClip doesn't handle).
//   clip                - clip to check.
//   stemDurationSeconds - clip's measured audio length.
//   bpm                 - song's tempo, to convert stemDurationSeconds to beats.
// True if spanBeats fits within one loop (small float tolerance); false means the audio would
// wrap before the pattern's last notes are reached. Both the game and editor reject a clip that
// fails this before using its notes.
bool ClipFitsOneLoop(const ChartClip& clip, double stemDurationSeconds, double bpm);

// Computes the wall-clock second a Learn section should hand off to the next section.
//   originSeconds       - current arrangement's shared origin (see namespace comment);
//                          sectionStartSeconds/loopStartSeconds share this same timeline.
//   sectionStartSeconds - wall-clock second the section itself began.
//   loopStartSeconds    - wall-clock second the clip's loop actually started.
//   stemDuration        - length of one loop of the clip's audio, in seconds.
//   loopCount           - section's declared loop_count.
//   tFallSeconds        - minimum preview lead time required (kNoteFallBeats worth of seconds).
// Mirrors GameSession::BeginSection's Learn case: a Learn section starts its clip and schedules
// its advance immediately, same as Break - whether the player meets hits_required doesn't affect
// this. Result is the next loop boundary at/after sectionStartSeconds, floored by loopCount, and
// extended by whole loops until at least tFallSeconds separates the start from the hand-off.
double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double loopStartSeconds,
                                   double stemDuration, int loopCount, double tFallSeconds);

// Result of ComputeBreakAdvance.
struct BreakAdvance
{
    int loopCount = 1;       // Final loop count (may exceed the requested one).
    double advanceSeconds = 0.0; // Wall-clock second the section should advance at.
};

// Computes the loop count and advance time for a Break section.
//   originSeconds      - current arrangement's shared origin (see namespace comment);
//                         loopStartSeconds shares this same timeline.
//   loopStartSeconds   - wall-clock second the clip's loop actually started.
//   stemDuration       - length of one loop of the clip's audio, in seconds.
//   requestedLoopCount - section's declared loop_count.
//   tFallSeconds       - minimum preview lead time required.
// Unlike Learn, a break's loop count is already known up front, so rather than waiting extra
// silence after it, the loop count itself is extended until playback covers at least
// tFallSeconds - the voice then stops naturally once its (possibly-extended) loops are done.
BreakAdvance ComputeBreakAdvance(double originSeconds, double loopStartSeconds, double stemDuration,
                                  int requestedLoopCount, double tFallSeconds);

// Extends advanceSeconds forward by whole stemDuration steps until it clears referenceSeconds by
// at least tFallSeconds.
//   advanceSeconds  - proposed advance time to extend.
//   referenceSeconds - instant the lead time is measured from ("now", or a hypothetical lock-in second).
//   stemDuration    - size of each extension step, in seconds.
//   tFallSeconds    - minimum lead time required.
// Shared "not enough preview lead time" rule used by GameSession::RegisterHit (fixing up an
// already-scheduled advance when Learn passing flips true) and BlockSchedule::Build (the same
// computation up front for a hypothetical perfect player). No-op if stemDuration <= 0. Distinct
// from ComputeLearnAdvanceSeconds's own internal extension, which always references
// sectionStartSeconds rather than "now".
double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
                                     double tFallSeconds);

// Computes the audio-seek phase (seconds into the loop) for a clip starting/resuming at nowSeconds.
//   originSeconds - current arrangement's shared origin (see namespace comment).
//   nowSeconds    - wall-clock second the clip is starting/resuming at.
//   clip          - clip being started.
//   stemDuration  - clip's measured audio length, in seconds.
//   bpm           - song's tempo.
// Uses clip.spanBeats (the pattern's exact cycle length) as the phase modulus when the clip has a
// pattern (hasMidi), rather than the audio's raw measured duration - real WAV export rarely lands
// a stem's sample count on an exact beat boundary, and that per-loop imprecision would otherwise
// compound into audible drift over many loops. Falls back to raw stemDuration for a clip with no
// pattern (Break/Background only), where nothing is compared against it. Returns exactly 0
// whenever nowSeconds lands on one of the clip's own cycle boundaries from originSeconds - always
// true for a fresh arrangement join, per the alignment invariant.
double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, const ChartClip& clip, double stemDuration,
                                double bpm);

// Returns every onset for a lane's pattern falling in (afterBeatExclusive, uptoBeatInclusive],
// ascending.
//   originBeat         - current arrangement's shared origin, same tiling convention as NextOnsetAfter.
//   afterBeatExclusive - lower bound, exclusive.
//   uptoBeatInclusive  - upper bound, inclusive.
//   notes              - lane's pattern notes to tile.
//   spanBeats          - clip's pattern cycle length.
// Unlike NextOnsetAfter (single next onset) this can return several onsets, or none. Used by
// GameSession::Update to auto-score every note a locked-in Pass-mode clip crossed since the last
// tick, without re-deriving NextOnsetAfter's tiling math. Deliberately onsets only, not
// spans/overlap - distinct from NoteLaneModel's rendering-only NotesInRange. Returns empty if
// notes is empty, spanBeats <= 0, or the range is empty/inverted.
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
