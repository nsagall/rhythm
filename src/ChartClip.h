#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ChartMidi.h"
#include "LaneConfig.h"

// How a [learn] clip's judging responds to a miss - see GameSession/
// SectionInstance for the actual state machine. Only meaningful for a
// clip used in a [learn] section; ignored entirely for break/reset/
// background usage.
enum class LearnMode
{
    // A streak of hitsRequired correct hits in a row locks the section
    // in permanently for the rest of its run - once reached, further
    // misses never un-reach it.
    Pass,
    // The same streak is reversible: starts already "passing," any miss
    // immediately drops it to "failing," and re-earning hitsRequired in
    // a row brings it back to "passing." Independent of the separate
    // 3-consecutive-miss clip-stop penalty, which applies the same way
    // in both modes.
    DontFail,
};

// Forward declared only so ChartClip's own member functions below can take
// a ChartSong parameter (ValidateArrangementAlignment) - ChartSong itself
// (see ChartSong.h) holds the std::vector<ChartClip> that makes a full
// ChartClip definition unavailable to it, so the two headers can't include
// each other.
class ChartSong;

// A reusable audio+MIDI bundle: a stem file, its MIDI-authored note
// pattern, and the thresholds needed to judge/lock it in when played in a
// [learn] section. A clip is purely a definition - it does nothing on its
// own until a section block references it, and the same clip may be
// referenced by more than one section (e.g. once as a background layer,
// later as the thing being learned). laneNotes[i] holds lane i's own
// independent, 0-indexed-from-repetition-start note list (each lane's
// notes never overlap themselves, but different lanes' notes are
// otherwise unrelated); every lane repeats indefinitely over the same
// shared spanBeats, i.e. a lane's absolute note starts are
// n*spanBeats + note.startBeat for n = 0, 1, 2, ...
//
// wavFilePath/midiFilePath are derived from the chart's declared
// `name` field (chart directory + name + ".wav"/".mid"), not
// independently authored paths. The .mid file is optional - hasMidi is
// false (laneNotes/spanBeats left at their defaults) when it doesn't
// exist on disk, which is only a problem for a clip used in a [learn]
// section (rejected at load time); [break]/[reset]/[background] sections
// never touch MIDI data at all.
class ChartClip
{
public:
    // Stable identifier, also the file stem for wavFilePath/midiFilePath.
    // This is what section blocks reference via their `clip` field.
    std::wstring name;
    // Human-readable label only - never used for cross-referencing.
    std::wstring displayName;
    std::wstring wavFilePath;
    std::wstring midiFilePath;
    bool hasMidi = false;
    std::vector<LaneNote> laneNotes[c_LaneCount];
    double spanBeats = 4.0;
    int hitsRequired = 16;
    // Both resolved at load time: the clip's own start_tolerance_ms/
    // release_tolerance_ms if it declared one, otherwise the song's
    // global default (ChartSong::startToleranceMs/releaseToleranceMs) -
    // downstream code never needs to know which one applied.
    double startToleranceMs = 120.0;
    double releaseToleranceMs = 120.0;
    double initVolume = 1.0; // volume while the player is still learning this clip
    double volume = 1.0;     // volume once it's locked in and looping automatically, or during break/background playback
    // Declared per-clip only (no song-level default, unlike the
    // tolerances above) - see LearnMode's own comment.
    LearnMode learnMode = LearnMode::Pass;

    // ---------------------------------------------------------------
    // Timing/scheduling queries and mutators.
    //
    // Pure, stateless (beyond *this) timing/note-onset arithmetic shared by
    // the live game (GameSession) and the editor's analytical block
    // scheduler (src/editor/BlockSchedule.h), so the two can never compute
    // different answers for the same inputs. Every non-static one below
    // reads (or, for ExpandLaneNotesToFillClip, writes) only *this - no
    // wall-clock/other-instance-state dependency of any kind; the static
    // ones don't even need a clip.
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
    // ---------------------------------------------------------------

    // Returns this lane's next note onset (absolute beats) strictly after afterBeat.
    //   originBeat - current arrangement's shared origin (see the class comment above), not absolute beat 0.
    //   afterBeat  - returned onset is strictly after this beat.
    //   lane       - which lane to search.
    // Also covers this clip's very first onset in a fresh arrangement: querying at (joinBeat - epsilon)
    // returns joinBeat + this lane's own first note, since the alignment invariant guarantees
    // joinBeat already sits on one of this clip's own cycle boundaries from originBeat.
    double NextOnsetAfter(double originBeat, double afterBeat, int lane) const;

    // Widens this clip's declared spanBeats to match its actual audio length, if shorter.
    //   stemDurationSeconds - clip's measured audio length.
    //   bpm                 - song's tempo, to convert stemDurationSeconds to beats.
    // Only whole pattern repeats are re-tiled in; a trailing partial repeat is left out, since a
    // leftover shorter than one full pattern is usually a deliberate silent tail (room tone/reverb
    // past the last note) rather than an intended partial repeat - tiling one in would invent notes
    // the chart author never placed. A pattern that legitimately repeats several whole times within
    // one audio loop is unaffected. No-op if the pattern already fills (or exceeds) one loop.
    void ExpandLaneNotesToFillClip(double stemDurationSeconds, double bpm);

    // Checks whether this clip's pattern fits within one loop of its audio (the reverse case
    // ExpandLaneNotesToFillClip doesn't handle).
    //   stemDurationSeconds - clip's measured audio length.
    //   bpm                 - song's tempo, to convert stemDurationSeconds to beats.
    // True if spanBeats fits within one loop (small float tolerance); false means the audio would
    // wrap before the pattern's last notes are reached. Both the game and editor reject a clip that
    // fails this before using its notes.
    bool ClipFitsOneLoop(double stemDurationSeconds, double bpm) const;

    // Computes the audio-seek phase (seconds into the loop) for this clip starting/resuming at nowSeconds.
    //   originSeconds - current arrangement's shared origin (see the class comment above).
    //   nowSeconds    - wall-clock second this clip is starting/resuming at.
    //   stemDuration  - length of one loop of the clip's audio, in seconds (used only when this
    //                   clip has no MIDI pattern of its own to sync to instead).
    //   bpm           - song's tempo.
    // Uses spanBeats (converted via bpm), not the raw audio duration, whenever this clip has a
    // pattern (hasMidi) - keeps the audio phase-locked to the judged beat grid instead of drifting
    // from it over many loops, since a real stem's measured length rarely lands exactly on a beat.
    double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, double stemDuration, double bpm) const;

    // Computes the wall-clock second at which loopCount full loops complete.
    //   originSeconds    - current arrangement's shared origin (see the class comment above).
    //   loopStartSeconds - wall-clock second the clip's loop actually started.
    //   stemDuration     - length of one loop of the clip's audio, in seconds.
    //   loopCount        - number of full loops to count.
    // Measured relative to originSeconds so the boundary lands where the real, phase-seeked audio
    // actually wraps, not on a beat-0-aligned grid. Shared by Learn's advance floor and Break's wait.
    static double ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration,
                                           int loopCount);

    // Computes the wall-clock second a Learn section should hand off to the next section.
    //   originSeconds       - current arrangement's shared origin (see the class comment above);
    //                          sectionStartSeconds/loopStartSeconds share this same timeline.
    //   sectionStartSeconds - wall-clock second the section itself began.
    //   loopStartSeconds    - wall-clock second the clip's loop actually started.
    //   stemDuration        - length of one loop of the clip's audio, in seconds.
    //   loopCount           - section's declared loop_count.
    //   tFallSeconds        - minimum preview lead time required (c_NoteFallBeats worth of seconds).
    // Mirrors GameSession::BeginSection's Learn case: a Learn section starts its clip and schedules
    // its advance immediately, same as Break - whether the player meets hits_required doesn't affect
    // this. Result is the next loop boundary at/after sectionStartSeconds, floored by loopCount, and
    // extended by whole loops until at least tFallSeconds separates the start from the hand-off.
    static double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds,
                                              double loopStartSeconds, double stemDuration, int loopCount,
                                              double tFallSeconds);

    // Result of ComputeBreakAdvance.
    struct BreakAdvance
    {
        int loopCount = 1;           // Final loop count (may exceed the requested one).
        double advanceSeconds = 0.0; // Wall-clock second the section should advance at.
    };

    // Computes the loop count and advance time for a Break section.
    //   originSeconds      - current arrangement's shared origin (see the class comment above);
    //                         loopStartSeconds shares this same timeline.
    //   loopStartSeconds   - wall-clock second the clip's loop actually started.
    //   stemDuration       - length of one loop of the clip's audio, in seconds.
    //   requestedLoopCount - section's declared loop_count.
    //   tFallSeconds       - minimum preview lead time required.
    // Unlike Learn, a break's loop count is already known up front, so rather than waiting extra
    // silence after it, the loop count itself is extended until playback covers at least
    // tFallSeconds - the voice then stops naturally once its (possibly-extended) loops are done.
    static BreakAdvance ComputeBreakAdvance(double originSeconds, double loopStartSeconds, double stemDuration,
                                             int requestedLoopCount, double tFallSeconds);

    // Extends advanceSeconds forward by whole stemDuration steps until it clears referenceSeconds by
    // at least tFallSeconds.
    //   advanceSeconds   - proposed advance time to extend.
    //   referenceSeconds - instant the lead time is measured from ("now", or a hypothetical lock-in second).
    //   stemDuration     - size of each extension step, in seconds.
    //   tFallSeconds     - minimum lead time required.
    // Shared "not enough preview lead time" rule used by GameSession::RegisterHit (fixing up an
    // already-scheduled advance when Learn passing flips true) and BlockSchedule::Build (the same
    // computation up front for a hypothetical perfect player). No-op if stemDuration <= 0. Distinct
    // from ComputeLearnAdvanceSeconds's own internal extension, which always references
    // sectionStartSeconds rather than "now".
    static double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
                                                double tFallSeconds);

    // Returns every onset (a lane's own pattern, tiled every spanBeats from originBeat - same
    // convention as NextOnsetAfter) whose absolute beat falls in (afterBeatExclusive,
    // uptoBeatInclusive], ascending.
    //   originBeat         - current arrangement's shared origin (see the class comment above).
    //   afterBeatExclusive - lower bound, exclusive.
    //   uptoBeatInclusive  - upper bound, inclusive.
    //   notes              - one lane's own note list (e.g. someClip.laneNotes[lane]).
    //   spanBeats          - the pattern's own cycle length (e.g. someClip.spanBeats).
    // Static (rather than a non-static query against a single clip) since every real caller already
    // has a specific lane's own note list and span in hand, not a whole clip, and there's no other
    // per-clip state this needs. Unlike NextOnsetAfter (single next onset) this can return several -
    // or none - depending on how far apart the two bounds are; used by GameSession::Update() to
    // auto-score every note a Pass-mode section's locked-in clip has crossed since the last tick,
    // without re-deriving NextOnsetAfter's own bar-tiling math a second time. Deliberately onsets
    // only, not spans/overlap - NoteLaneModel's own NotesInRange is a separate, rendering-only
    // function with a different contract and a different consumer. Returns an empty vector if notes
    // is empty, spanBeats <= 0, or the range is empty/inverted.
    static std::vector<double> OnsetsInRange(double originBeat, double afterBeatExclusive,
                                              double uptoBeatInclusive, const std::vector<LaneNote>& notes,
                                              double spanBeats);

    // A judged clip's own timing inputs to ValidateArrangementAlignment - kept
    // deliberately separate from spanBeats itself, since that field gets
    // overwritten by ExpandLaneNotesToFillClip to the raw audio's own
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
        // The clip's own bar-aligned pattern length, exactly as ChartSong::Load
        // (AlignToBarBoundary) produced it - before any ExpandLaneNotesToFillClip
        // widening.
        double authoredSpanBeats = 0.0;
        // The clip's real, AudioEngine-measured stem duration.
        double stemDurationSeconds = 0.0;
    };

    // Checks the whole-chart invariant every function above relies on (see the
    // class comment above): every clip's own authored length (ClipAlignmentInfo,
    // not spanBeats post-widening) is a whole number of bars, and every clip
    // lands on one of its own bar boundaries wherever it actually joins an
    // arrangement - see ClipAlignmentInfo's own comment for exactly which of a
    // clip's two lengths (authored vs. real/widened) governs which side of that
    // check. Static since this checks relationships across every clip/section
    // in the whole song, not a single clip. Call once every judged clip's real
    // stem duration is known, but before ExpandLaneNotesToFillClip runs on any
    // of them - clipInfo is keyed by address into song.clips, same convention
    // as BlockSchedule::Build; a clip with no entry (or hasMidi false) is
    // treated as having no pattern to align - nothing else in the chart needs
    // to relate to it bar-wise.
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
    static bool ValidateArrangementAlignment(const ChartSong& song,
                                              const std::unordered_map<const ChartClip*, ClipAlignmentInfo>& clipInfo,
                                              std::vector<std::wstring>& outErrors);
};
