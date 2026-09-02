#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ChartMidi.h"    // LaneNote stored by value in m_laneNotes.
#include "LaneConfig.h"   // c_LaneCount sizes m_laneNotes.

// How a [learn] clip's judging responds to a miss.
enum class LearnMode
{
    Pass,     // A streak of hitsRequired correct hits locks the section in permanently.
    DontFail, // The streak is reversible: a miss drops back to failing, re-earning it restores passing.
};

class ChartSong;

// Amount to subtract from a fresh join's scheduled beat when querying NextOnsetAfter. Comfortably
// exceeds the floating-point drift a long chain of section advances accumulates (a few millionths
// of a beat per hop), while staying far below any realistic note-to-note spacing.
constexpr double c_FreshJoinEpsilonBeats = 1e-3;

// A reusable audio+MIDI bundle: a stem file, its MIDI-authored note pattern, and the judging
// thresholds for a [learn] section. An inert definition until a section block references it by name
// (and more than one section may reference the same clip).
//
// LaneNotes(i) is lane i's note list for one repetition; every lane repeats over the shared
// SpanBeats(), so a lane's absolute note starts are n*SpanBeats() + note.startBeat. HasMidi() is
// false when the clip has no .mid file, which only matters for [learn] usage.
class ChartClip
{
public:
    const std::wstring& Name() const { return m_name; }               // Stable identifier; also the file stem for WavFilePath()/MidiFilePath().
    const std::wstring& DisplayName() const { return m_displayName; } // Human-readable label only - never used for cross-referencing.
    const std::wstring& WavFilePath() const { return m_wavFilePath; }
    const std::wstring& MidiFilePath() const { return m_midiFilePath; }
    bool HasMidi() const { return m_hasMidi; }
    const std::vector<LaneNote>& LaneNotes(int lane) const { return m_laneNotes[lane]; }
    double SpanBeats() const { return m_spanBeats; }
    int HitsRequired() const { return m_hitsRequired; }
    // Resolved at load time: the clip's own declared value, or the song's global default.
    double StartToleranceMs() const { return m_startToleranceMs; }
    double ReleaseToleranceMs() const { return m_releaseToleranceMs; }
    double InitVolume() const { return m_initVolume; } // Volume while the player is still learning this clip.
    double Volume() const { return m_volume; }         // Volume once locked in, or during break/background playback.
    LearnMode Mode() const { return m_learnMode; }

    // ---------------------------------------------------------------
    // Timing/scheduling queries and mutators. Pure arithmetic over the parameters plus *this,
    // shared verbatim by GameSession and the editor's analytical block scheduler (BlockSchedule.h).
    //
    // originBeat/originSeconds below is always the current arrangement's shared origin - the
    // beat/second its first clip began at - never absolute 0 and never a per-clip value. Every
    // clip's spanBeats is a whole number of bars and every clip joins on one of its own bar
    // boundaries from this origin, enforced by ValidateArrangementAlignment.
    // ---------------------------------------------------------------

    // Returns this lane's next note onset, in absolute beats, strictly after afterBeat.
    //   originBeat - current arrangement's shared origin.
    //   afterBeat  - result is strictly greater than this beat.
    //   lane       - lane to search.
    // For a fresh join, a query at (joinBeat - c_FreshJoinEpsilonBeats) returns joinBeat plus this
    // lane's own first note.
    double NextOnsetAfter(double originBeat, double afterBeat, int lane) const;

    // Widens this clip's declared spanBeats to match its actual audio length, if the audio is longer.
    //   stemDurationSeconds - clip's measured audio length, in seconds.
    //   bpm                 - song tempo, to convert seconds to beats.
    // Only whole pattern repeats are re-tiled in; a trailing partial repeat is left silent. No-op
    // if the pattern already fills (or exceeds) one loop.
    void ExpandLaneNotesToFillClip(double stemDurationSeconds, double bpm);

    // Checks whether this clip's pattern fits within one loop of its audio.
    //   stemDurationSeconds - clip's measured audio length, in seconds.
    //   bpm                 - song tempo, to convert seconds to beats.
    // Returns true if spanBeats fits within one loop (small float tolerance); false means the audio
    // would wrap before the pattern's last notes are reached.
    bool ClipFitsOneLoop(double stemDurationSeconds, double bpm) const;

    // Simplifies this clip's MIDI-derived pattern for easy mode, in place.
    //   bpm - song tempo, to convert timing constants to beats.
    // Thins each lane's own notes, then thins across lanes, then collapses remaining simultaneous
    // notes to one per chord. No-op unless HasMidi(). Must run before ExpandLaneNotesToFillClip.
    void ApplyEasyModeTransform(double bpm);

    // Returns the audio-seek phase (seconds into the loop) for this clip starting/resuming at nowSeconds.
    //   originSeconds - current arrangement's shared origin, in seconds.
    //   nowSeconds    - wall-clock second this clip is starting/resuming at.
    //   stemDuration  - length of one loop of the clip's audio, in seconds (used only without a MIDI pattern).
    //   bpm           - song tempo.
    // Uses spanBeats when this clip has a pattern, not the raw audio duration.
    double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, double stemDuration, double bpm) const;

    // Returns the wall-clock second at which loopCount full loops complete.
    //   originSeconds    - current arrangement's shared origin, in seconds.
    //   loopStartSeconds - wall-clock second the clip's loop actually started.
    //   stemDuration     - length of one loop of the clip's audio, in seconds.
    //   loopCount        - number of full loops to count.
    // Asserts loopStartSeconds is already exactly one of the clip's own loop boundaries.
    static double ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration,
                                           int loopCount);

    // Returns the wall-clock second a Learn section should hand off to the next section.
    //   originSeconds       - current arrangement's shared origin, in seconds.
    //   sectionStartSeconds - wall-clock second the section (and its clip) began.
    //   stemDuration        - length of one loop of the clip's audio, in seconds.
    //   loopCount           - section's declared loop_count.
    //   tFallSeconds        - minimum preview lead time required, in seconds (c_NoteFallBeats worth).
    // A Learn section always restarts its clip fresh, so this is ComputeLoopFloorSeconds's loopCount
    // floor from sectionStartSeconds, extended until tFallSeconds separates start from hand-off.
    static double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double stemDuration,
                                              int loopCount, double tFallSeconds);

    // Result of ComputeBreakAdvance.
    struct BreakAdvance
    {
        // Final loop count (may exceed the requested one).
        int loopCount = 1;

        // Wall-clock second the section should advance at.
        double advanceSeconds = 0.0;
    };

    // Returns the loop count and advance time for a Break section.
    //   originSeconds      - current arrangement's shared origin, in seconds.
    //   loopStartSeconds   - wall-clock second the clip's loop actually started.
    //   stemDuration       - length of one loop of the clip's audio, in seconds.
    //   requestedLoopCount - section's declared loop_count.
    //   tFallSeconds       - minimum preview lead time required, in seconds.
    // The loop count itself is extended (not padded with silence) until playback covers at least
    // tFallSeconds.
    static BreakAdvance ComputeBreakAdvance(double originSeconds, double loopStartSeconds, double stemDuration,
                                             int requestedLoopCount, double tFallSeconds);

    // Extends advanceSeconds forward by whole stemDuration steps until it clears referenceSeconds
    // by at least tFallSeconds.
    //   advanceSeconds   - proposed advance time to extend, in seconds.
    //   referenceSeconds - instant the lead time is measured from, in seconds.
    //   stemDuration     - size of each extension step, in seconds.
    //   tFallSeconds     - minimum lead time required, in seconds.
    // Returns the extended advance time. No-op if stemDuration <= 0.
    static double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
                                                double tFallSeconds);

    // Returns every onset in (afterBeatExclusive, uptoBeatInclusive], ascending - a lane's own
    // pattern tiled every spanBeats from originBeat (same convention as NextOnsetAfter).
    //   originBeat         - current arrangement's shared origin.
    //   afterBeatExclusive - lower bound, exclusive.
    //   uptoBeatInclusive  - upper bound, inclusive.
    //   notes              - one lane's own note list (e.g. someClip.LaneNotes(lane)).
    //   spanBeats          - the pattern's own cycle length (e.g. someClip.SpanBeats()).
    // Returns an empty vector if notes is empty, spanBeats <= 0, or the range is empty/inverted.
    static std::vector<double> OnsetsInRange(double originBeat, double afterBeatExclusive,
                                              double uptoBeatInclusive, const std::vector<LaneNote>& notes,
                                              double spanBeats);

    // A judged clip's two timing inputs to ValidateArrangementAlignment, captured before
    // ExpandLaneNotesToFillClip may widen spanBeats. A clip currently PLAYING advances in multiples
    // of its real stemDurationSeconds; a clip JOINING an arrangement aligns to its authoredSpanBeats.
    struct ClipAlignmentInfo
    {
        // Bar-aligned pattern length before any widening, in beats.
        double authoredSpanBeats = 0.0;

        // Real, AudioEngine-measured stem duration, in seconds.
        double stemDurationSeconds = 0.0;
    };

    // Checks that every clip's authored length is a whole number of bars and that every clip lands
    // on one of its own bar boundaries wherever it joins an arrangement.
    //   song      - the chart to validate.
    //   clipInfo  - each judged clip's own ClipAlignmentInfo, keyed by address into song.clips.
    //   outErrors - filled with one message per violation found.
    // Returns false if the chart can't satisfy the invariant. Call once every judged clip's real
    // stem duration is known, but before ExpandLaneNotesToFillClip runs. A join right after a Learn
    // section (whose real advance is unbounded) is checked against every possible loop count: its
    // authored length must evenly divide the Learn clip's own real length.
    static bool ValidateArrangementAlignment(const ChartSong& song,
                                              const std::unordered_map<const ChartClip*, ClipAlignmentInfo>& clipInfo,
                                              std::vector<std::wstring>& outErrors);

private:
    // ChartSong::Load fills these fields in while parsing.
    friend class ChartSong;

    std::wstring m_name;
    std::wstring m_displayName;
    std::wstring m_wavFilePath;
    std::wstring m_midiFilePath;
    bool m_hasMidi = false;
    std::vector<LaneNote> m_laneNotes[c_LaneCount];
    double m_spanBeats = 4.0;
    int m_hitsRequired = 16;
    double m_startToleranceMs = 120.0;
    double m_releaseToleranceMs = 120.0;
    double m_initVolume = 1.0;
    double m_volume = 1.0;
    LearnMode m_learnMode = LearnMode::Pass;
};

// One clip's live playback state, shared by GameSession and the editor's analytical BlockSchedule.
// Set via SetContext/MarkStarted/MarkStopped. Usually one per clip in a
// std::unordered_map<const ChartClip*, ClipInstance>, keyed by the clip's address.
//
// LoopStartSeconds() is the real wall-clock instant the loop began. The timing methods below never
// read it - they take an explicit sectionStartSeconds/loopStartSeconds, the deterministic instant
// a section was scheduled for, not whatever instant playback actually began at.
class ClipInstance
{
public:
    bool IsPlaying() const { return m_isPlaying; }
    const ChartClip* Clip() const { return m_clip; }
    double OriginSeconds() const { return m_originSeconds; }
    double LoopStartSeconds() const { return m_loopStartSeconds; }
    double StemDurationSeconds() const { return m_stemDurationSeconds; }

    // Records which clip this instance represents and its current timing context, without marking
    // it playing. Callers needing the timing queries before audio starts call this first.
    //   clip                - the clip this instance represents.
    //   originSeconds       - current arrangement's shared origin, in seconds.
    //   stemDurationSeconds - measured length of one loop of the clip's audio, in seconds.
    // Idempotent to call again with the same values.
    void SetContext(const ChartClip& clip, double originSeconds, double stemDurationSeconds);

    // Marks this instance actually playing.
    //   loopStartSeconds - real wall-clock instant the loop began.
    void MarkStarted(double loopStartSeconds);

    // Marks this instance stopped. Its remembered clip/origin/stem-duration are left in place.
    void MarkStopped();

    // Same contract as ChartClip's identically-named methods, reading this instance's own
    // clip/originSeconds/stemDurationSeconds instead of taking them as parameters.
    double NextOnsetAfter(double bpm, double afterBeat, int lane) const;
    double ComputeClipPhaseSeconds(double nowSeconds, double bpm) const;
    double ComputeLearnAdvanceSeconds(double sectionStartSeconds, int loopCount, double tFallSeconds) const;
    ChartClip::BreakAdvance ComputeBreakAdvance(double loopStartSeconds, int requestedLoopCount,
                                                 double tFallSeconds) const;
    double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double tFallSeconds) const;
    std::vector<double> OnsetsInRange(double bpm, double afterBeatExclusive, double uptoBeatInclusive, int lane) const;

private:
    double OriginBeat(double bpm) const;

    const ChartClip* m_clip = nullptr;
    bool m_isPlaying = false;
    double m_originSeconds = 0.0;
    double m_loopStartSeconds = 0.0;
    double m_stemDurationSeconds = 0.0;
};
