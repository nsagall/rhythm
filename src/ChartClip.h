#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ChartMidi.h"
#include "LaneConfig.h"

// How a [learn] clip's judging responds to a miss. Meaningful only for a clip used in a [learn] section.
enum class LearnMode
{
    Pass,     // hitsRequired correct hits in a row locks the section in permanently.
    DontFail, // The same streak is reversible: a miss drops back to failing, re-earning it restores passing.
};

// Forward declared so ValidateArrangementAlignment can take a ChartSong parameter;
// ChartSong.h can't be included here since it holds a std::vector<ChartClip>.
class ChartSong;

// How far below a fresh join's own scheduled beat a caller should query
// ChartClip::NextOnsetAfter's own afterBeat parameter to reliably land on
// that clip's true first note, rather than skipping past it - see
// NextOnsetAfter's own doc comment for why querying "just before" a fresh
// join's own beat is the trick that makes one formula cover both a clip's
// first-ever appearance and a later reuse. Needs to comfortably exceed the
// real floating-point drift a long chain of section advances can
// accumulate (ComputeLearnAdvanceSeconds works from real, measured stem
// durations, not perfectly round beat values, so each hop adds a few
// millionths of a beat - confirmed real repro: after three chained learn
// sections, "A Real Good Time"'s chords section arrived about six
// millionths of a beat past its own true first onset, enough for the
// scheduled-beat query to land on its *second* note instead and the first
// one never appeared at all) while staying far below any realistic
// note-to-note spacing (chart authors never place two distinct onsets
// within a thousandth of a beat of each other).
constexpr double c_FreshJoinEpsilonBeats = 1e-3;

// A reusable audio+MIDI bundle: a stem file, its MIDI-authored note pattern, and the
// judging thresholds for a [learn] section. Purely a definition - a section block must
// reference it to do anything, and the same clip may be referenced by more than one section.
// LaneNotes(i) is lane i's own note list for one repetition; every lane repeats over the
// shared SpanBeats(), so a lane's absolute note starts are n*spanBeats + note.startBeat.
// HasMidi() is false when the clip has no .mid file - only a problem for [learn] usage.
//
// Fields are private and populated only by ChartSong::Load (a friend, since chart parsing builds
// a clip's fields up incrementally, one `key = value` line at a time, not via one constructor
// call) and by this class's own mutators (ExpandLaneNotesToFillClip, ApplyEasyModeTransform).
// Everyone else only ever reads a ChartClip, via the getters below.
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
    double InitVolume() const { return m_initVolume; } // volume while the player is still learning this clip
    double Volume() const { return m_volume; }         // volume once locked in, or during break/background playback
    LearnMode Mode() const { return m_learnMode; }

    // ---------------------------------------------------------------
    // Timing/scheduling queries and mutators. Pure, stateless (beyond *this) arithmetic shared by
    // GameSession and the editor's analytical block scheduler (BlockSchedule.h), so both compute
    // identical answers.
    //
    // originBeat/originSeconds below is always the current arrangement's shared origin - the
    // wall-clock beat/second its first clip began at - never absolute beat/second 0 and never a
    // per-clip value. Every clip's spanBeats is a whole number of bars, and every clip starts on
    // one of its own bar boundaries from this origin - enforced by ValidateArrangementAlignment.
    // ---------------------------------------------------------------

    // Returns this lane's next note onset (absolute beats) strictly after afterBeat.
    //   originBeat - current arrangement's shared origin.
    //   afterBeat  - returned onset is strictly after this beat.
    //   lane       - which lane to search.
    // Returns joinBeat + this lane's own first note when queried at (joinBeat - epsilon) for a fresh join.
    double NextOnsetAfter(double originBeat, double afterBeat, int lane) const;

    // Widens this clip's declared spanBeats to match its actual audio length, if shorter.
    //   stemDurationSeconds - clip's measured audio length.
    //   bpm                 - song's tempo, to convert stemDurationSeconds to beats.
    // Only whole pattern repeats are re-tiled in; a trailing partial repeat is left silent, not
    // invented. No-op if the pattern already fills (or exceeds) one loop.
    void ExpandLaneNotesToFillClip(double stemDurationSeconds, double bpm);

    // Checks whether this clip's pattern fits within one loop of its audio.
    //   stemDurationSeconds - clip's measured audio length.
    //   bpm                 - song's tempo, to convert stemDurationSeconds to beats.
    // Returns true if spanBeats fits within one loop (small float tolerance); false means the
    // audio would wrap before the pattern's last notes are reached.
    bool ClipFitsOneLoop(double stemDurationSeconds, double bpm) const;

    // Simplifies this clip's MIDI-derived pattern for easy mode, in place.
    //   bpm - song tempo, to convert timing constants to beats.
    // Thins each lane's own notes, then thins across lanes, then collapses remaining
    // simultaneous notes to one per chord. No-op unless HasMidi().
    // Must run before ExpandLaneNotesToFillClip.
    void ApplyEasyModeTransform(double bpm);

    // Computes the audio-seek phase (seconds into the loop) for this clip starting/resuming at nowSeconds.
    //   originSeconds - current arrangement's shared origin.
    //   nowSeconds    - wall-clock second this clip is starting/resuming at.
    //   stemDuration  - length of one loop of the clip's audio, in seconds (used only without a MIDI pattern).
    //   bpm           - song's tempo.
    // Uses spanBeats when this clip has a pattern, not the raw audio duration, so playback stays
    // phase-locked to the judged beat grid instead of drifting over many loops.
    double ComputeClipPhaseSeconds(double originSeconds, double nowSeconds, double stemDuration, double bpm) const;

    // Computes the wall-clock second at which loopCount full loops complete.
    //   originSeconds    - current arrangement's shared origin.
    //   loopStartSeconds - wall-clock second the clip's loop actually started.
    //   stemDuration     - length of one loop of the clip's audio, in seconds.
    //   loopCount        - number of full loops to count.
    // Asserts loopStartSeconds is already exactly one of the clip's own loop boundaries - guaranteed
    // by every real caller, so this can just add whole loops instead of re-deriving the boundary.
    static double ComputeLoopFloorSeconds(double originSeconds, double loopStartSeconds, double stemDuration,
                                           int loopCount);

    // Computes the wall-clock second a Learn section should hand off to the next section.
    //   originSeconds       - current arrangement's shared origin.
    //   sectionStartSeconds - wall-clock second the section (and its clip) began.
    //   stemDuration        - length of one loop of the clip's audio, in seconds.
    //   loopCount           - section's declared loop_count.
    //   tFallSeconds        - minimum preview lead time required (c_NoteFallBeats worth of seconds).
    // A Learn section always restarts its own clip fresh, never joining a still-open instance
    // (ValidateArrangementAlignment enforces this), so this is just ComputeLoopFloorSeconds's own
    // loopCount floor from sectionStartSeconds, extended until tFallSeconds separates start from hand-off.
    static double ComputeLearnAdvanceSeconds(double originSeconds, double sectionStartSeconds, double stemDuration,
                                              int loopCount, double tFallSeconds);

    // Result of ComputeBreakAdvance.
    struct BreakAdvance
    {
        int loopCount = 1;           // Final loop count (may exceed the requested one).
        double advanceSeconds = 0.0; // Wall-clock second the section should advance at.
    };

    // Computes the loop count and advance time for a Break section.
    //   originSeconds      - current arrangement's shared origin.
    //   loopStartSeconds   - wall-clock second the clip's loop actually started.
    //   stemDuration       - length of one loop of the clip's audio, in seconds.
    //   requestedLoopCount - section's declared loop_count.
    //   tFallSeconds       - minimum preview lead time required.
    // The loop count itself is extended (rather than waiting extra silence after it) until
    // playback covers at least tFallSeconds, so the voice stops naturally once its loops finish.
    static BreakAdvance ComputeBreakAdvance(double originSeconds, double loopStartSeconds, double stemDuration,
                                             int requestedLoopCount, double tFallSeconds);

    // Extends advanceSeconds forward by whole stemDuration steps until it clears referenceSeconds
    // by at least tFallSeconds.
    //   advanceSeconds   - proposed advance time to extend.
    //   referenceSeconds - instant the lead time is measured from.
    //   stemDuration     - size of each extension step, in seconds.
    //   tFallSeconds     - minimum lead time required.
    // Returns the extended advance time. No-op if stemDuration <= 0.
    static double ExtendAdvanceForFallLeadTime(double advanceSeconds, double referenceSeconds, double stemDuration,
                                                double tFallSeconds);

    // Returns every onset in (afterBeatExclusive, uptoBeatInclusive], ascending - a lane's own
    // pattern, tiled every spanBeats from originBeat (same convention as NextOnsetAfter).
    //   originBeat         - current arrangement's shared origin.
    //   afterBeatExclusive - lower bound, exclusive.
    //   uptoBeatInclusive  - upper bound, inclusive.
    //   notes              - one lane's own note list (e.g. someClip.LaneNotes(lane)).
    //   spanBeats          - the pattern's own cycle length (e.g. someClip.SpanBeats()).
    // Returns an empty vector if notes is empty, spanBeats <= 0, or the range is empty/inverted.
    static std::vector<double> OnsetsInRange(double originBeat, double afterBeatExclusive,
                                              double uptoBeatInclusive, const std::vector<LaneNote>& notes,
                                              double spanBeats);

    // A judged clip's own timing inputs to ValidateArrangementAlignment - captured
    // BEFORE ExpandLaneNotesToFillClip may widen spanBeats to the stem's real length.
    //
    // Two different lengths matter for two different roles: a clip currently PLAYING advances in
    // multiples of its real (possibly widened) stemDurationSeconds, but a clip JOINING an
    // arrangement only needs to align to its own authoredSpanBeats - starting partway into a widened
    // buffer, at a multiple of the authored repeat, is inaudible since every repeat is identical.
    struct ClipAlignmentInfo
    {
        double authoredSpanBeats = 0.0;   // Bar-aligned pattern length before any widening.
        double stemDurationSeconds = 0.0; // Real, AudioEngine-measured stem duration.
    };

    // Checks that every clip's authored length is a whole number of bars, and that every clip
    // lands on one of its own bar boundaries wherever it joins an arrangement.
    //   song      - the chart to validate.
    //   clipInfo  - each judged clip's own ClipAlignmentInfo, keyed by address into song.clips.
    //   outErrors - filled with one message per violation found.
    // Returns false if the chart can't satisfy the invariant. Call once every judged clip's real
    // stem duration is known, but before ExpandLaneNotesToFillClip runs. A join right after a Learn
    // section (whose real advance is unbounded) is checked against every possible loop count instead
    // of one exact beat: its authored length must evenly divide the Learn clip's own real length.
    static bool ValidateArrangementAlignment(const ChartSong& song,
                                              const std::unordered_map<const ChartClip*, ClipAlignmentInfo>& clipInfo,
                                              std::vector<std::wstring>& outErrors);

private:
    friend class ChartSong; // Populates every field below, incrementally, while parsing.

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

// One clip's own playback state - shared by GameSession (the live game) and the editor's analytical
// BlockSchedule, so both track "what's currently playing" and compute its timing identically.
// Set via SetContext/MarkStarted/MarkStopped rather than a single constructor, since the two
// consumers establish clip/originSeconds/stemDurationSeconds at slightly different points in their
// own control flow. Usually kept one per clip in a std::unordered_map<const ChartClip*,
// ClipInstance>, keyed by the clip's own address.
//
// LoopStartSeconds() is the real wall-clock instant this instance's loop actually began - used only
// for external drift-resync bookkeeping (GameSession::Update), never by the methods below, which
// always take their own sectionStartSeconds/loopStartSeconds as an explicit parameter instead: that
// value must stay the deterministic instant a section was scheduled for, not whatever wall-clock
// instant playback actually happened to begin at - see ComputeLearnAdvanceSeconds's own comment for
// why conflating the two is a real bug, not a style choice.
class ClipInstance
{
public:
    bool IsPlaying() const { return m_isPlaying; }
    const ChartClip* Clip() const { return m_clip; }
    double OriginSeconds() const { return m_originSeconds; }
    double LoopStartSeconds() const { return m_loopStartSeconds; }
    double StemDurationSeconds() const { return m_stemDurationSeconds; }

    // Establishes which clip this instance represents and its current timing context, without
    // marking it playing - callers that need NextOnsetAfter/ComputeLearnAdvanceSeconds/etc. ahead of
    // actually starting audio call this first. Idempotent to call again with the same values (every
    // real caller does, once via this and again via MarkStarted's own caller moments later).
    void SetContext(const ChartClip& clip, double originSeconds, double stemDurationSeconds);

    // Marks this instance actually playing, with its real loop-start instant recorded as
    // loopStartSeconds (see the class comment for why that's distinct from any deterministic
    // sectionStartSeconds a caller separately passes to the methods below).
    void MarkStarted(double loopStartSeconds);

    // Marks this instance stopped. Its remembered clip/origin/stem-duration are left in place.
    void MarkStopped();

    // Same contract as ChartClip's own identically-named methods, reading this instance's own
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
