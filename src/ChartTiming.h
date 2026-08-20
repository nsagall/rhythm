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

// Returns this lane's next note onset (absolute beats) strictly after afterBeat.
//   originBeat - beat this clip's pattern was first anchored at (see FreshOnsetForAllLanes); not beat 0.
//   afterBeat  - returned onset is strictly after this beat.
//   clip       - clip whose pattern to tile.
//   lane       - which lane to search.
// A clip's cycle boundaries are fixed wherever it actually first started playing, not the
// song's beat-0-aligned grid - every call for the same clip must pass the same originBeat.
double NextOnsetAfter(double originBeat, double afterBeat, const ChartClip& clip, int lane);

// Computes each lane's first onset for a clip's very first-ever start.
//   originBeat - beat this start happens at; becomes the clip's persistent phase reference for
//                every later call (NextOnsetAfter, ComputeClipPhaseSeconds, ComputeLearnAdvanceSeconds,
//                ComputeBreakAdvance) against this clip.
//   clip       - clip whose pattern to anchor.
//   outBeats   - [out] one onset beat per lane.
// Each lane starts from its own earliest note offset by originBeat, so the clip judges from its
// true beginning immediately rather than waiting for a cycle boundary. A lane with no notes gets
// originBeat + clip.spanBeats (matches NextOnsetAfter's empty-lane placeholder).
void FreshOnsetForAllLanes(double originBeat, const ChartClip& clip, double outBeats[kLaneCount]);

// Computes the wall-clock second at which loopCount full loops complete.
//   originSeconds    - wall-clock second this clip's pattern was first anchored at (originBeat
//                       converted to seconds - see FreshOnsetForAllLanes).
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
//   originSeconds      - clip's persistent phase reference (see FreshOnsetForAllLanes);
//                         sectionStartSeconds/loopStartSeconds share this same timeline.
//   sectionStartSeconds - wall-clock second the section itself began.
//   loopStartSeconds   - wall-clock second the clip's loop actually started.
//   stemDuration       - length of one loop of the clip's audio, in seconds.
//   loopCount          - section's declared loop_count.
//   tFallSeconds       - minimum preview lead time required (kNoteFallBeats worth of seconds).
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
//   originSeconds      - clip's persistent phase reference (see FreshOnsetForAllLanes);
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
//   originSeconds - clip's persistent phase reference (see FreshOnsetForAllLanes).
//   nowSeconds    - wall-clock second the clip is starting/resuming at.
//   clip          - clip being started.
//   stemDuration  - clip's measured audio length, in seconds.
//   bpm           - song's tempo.
// Uses clip.spanBeats (the pattern's exact cycle length) as the phase modulus when the clip has a
// pattern (hasMidi), rather than the audio's raw measured duration - real WAV export rarely lands
// a stem's sample count on an exact beat boundary, and that per-loop imprecision would otherwise
// compound over many loops until the clip audibly starts partway through its loop instead of at
// its pattern's true beginning. Falls back to raw stemDuration for a clip with no pattern
// (Break/Background only), where nothing is compared against it. Returns 0 for a first-ever start
// (nowSeconds == originSeconds).
double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, const ChartClip& clip, double stemDuration,
                                double bpm);

// Returns every onset for a lane's pattern falling in (afterBeatExclusive, uptoBeatInclusive],
// ascending.
//   originBeat          - clip's phase reference, same tiling convention as NextOnsetAfter/
//                          FreshOnsetForAllLanes.
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

} // namespace ChartTiming
