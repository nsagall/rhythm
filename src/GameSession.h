#pragma once

#include <string>
#include <vector>

#include "AudioEngine.h"
#include "ChartFile.h"
#include "LaneConfig.h"
#include "SongClock.h"

// The stages a game session moves through, in order, once per song.
enum class GamePhase
{
    Idle,
    CountIn,
    Learning, // a section is active - covers learn/solo/background alike; see CurrentPlayMode() for which
    Complete,
};

// Result of the most recently judged press/release, for the UI to flash
// the note lane. Cleared once read via ConsumeLastJudgement().
enum class JudgementResult
{
    None,
    Hit,
    Miss,
};

// Drives the chart's ordered list of sections, one at a time. Each section
// references a reusable clip (a stem + MIDI pattern + judging thresholds)
// and a play_mode:
//   - Learn: judges key presses/releases against the clip's MIDI-derived
//     pattern (via SongClock), exactly as a lone "learn one instrument at a
//     time" flow always has. Each of the kLaneCount lanes tracks its own
//     note sequence completely independently - its own next-expected-note
//     pointer, its own press/release judging, its own retry-on-mistimed-
//     press behavior - the lanes share nothing but the streak/consecutive-
//     miss counters, so a chart author can require simultaneous presses
//     across lanes just by placing simultaneous notes in the MIDI file,
//     with no special "chord" bookkeeping anywhere in here. The clip's
//     loop starts playing (phase-aligned, at init_volume) on the player's
//     first correct press on any lane, stops after 3 consecutive misses,
//     and once the shared streak reaches the clip's hits_required, it
//     locks in - dots keep coming and keep being judged for a while longer
//     (the loop just keeps playing, switching init_volume -> volume once
//     the section actually advances), but misses no longer stop the loop
//     or affect timing once locked in, since there's nothing left to lose.
//     A chart-declared loop_count (on the section, default 1) sets a floor
//     under this: the clip must complete at least that many full loops
//     (counted from when it first started playing) before advancing, even
//     if the player locks in well before the first loop finishes -
//     loop_count=1 never changes anything, since the natural "wait for the
//     next loop boundary" behavior already guarantees at least one full
//     loop. A clip with a declared intro_bars instead starts playing
//     automatically (no press needed) and holds off judging/dots until
//     that many bars pass.
//   - Solo: stops every clip currently playing, starts this section's clip
//     (if any) looping, and blocks advancing until loop_count full loops
//     complete - no judging happens. An empty-clip solo just stops
//     everything (a silence gate). Either way, advancing is also floored
//     at kNoteFallBeats of real time from when the section began, same as
//     a locked-in learn section, so the next section's notes always get
//     their full on-screen travel time to preview before going live.
//   - Background: queues this section's clip to start playing (without
//     stopping anything else) at the moment the *next* section begins, then
//     keeps looping indefinitely - exactly like a locked-in learn clip -
//     until a later solo section's StopAll() (or Stop()/Start()) silences
//     it; loop_count has no effect on background sections. This section
//     itself takes zero time and never blocks.
// UI-agnostic - knows nothing about HWNDs or input devices.
class GameSession
{
public:
    explicit GameSession(AudioEngine& audioEngine);

    // Parses and validates a chart and loads all its clips' stems into the
    // audio engine. Returns false if the chart fails validation or any of
    // its stems can't be loaded, with outError describing every problem
    // found (one per line) so the caller can show it and ask for a
    // different file. When easyMode is true, every clip's MIDI-derived
    // pattern is simplified before it's used - see ApplyEasyModeTransform -
    // and judging itself changes (see OnPress/OnRelease/RegisterMiss).
    bool LoadChart(const std::wstring& chartFilePath, bool easyMode, std::wstring& outError);

    // Starts gameplay from the beginning of the loaded chart.
    void Start();

    // Stops all playback and returns to Idle.
    void Stop();

    // Registers a key-down for the given lane at the current moment;
    // judges it against that lane's next expected note if the current
    // section is a "learn" section.
    void OnPress(int lane);

    // True exactly when OnPress(lane) would actually judge a press right
    // now (whether it turns out to be a timely hit or a mistimed miss) -
    // false whenever there's structurally nothing to press: outside a
    // Learn section's live judging (count-in, intro, solo/background, or
    // no chart loaded at all), or a lane the current clip never places any
    // notes in at all. Lets the caller show its own "no note there"
    // feedback for a press this lane will otherwise just silently ignore.
    bool IsLaneJudgeable(int lane) const;

    // Registers a key-up for the given lane at the current moment; judges it
    // against the note that lane was holding, if any. Not gated by phase or
    // play mode - a hold already in flight resolves on its own merits even
    // if the section has since locked in, so it still paints its true
    // outcome instead of being abandoned mid-air.
    void OnRelease(int lane);

    // Advances count-in/miss-detection/hold-timeout timing; call once per frame.
    void Update();

    GamePhase Phase() const;
    const ChartSong& Song() const;
    int CurrentSectionIndex() const;

    // Returns the clip the current section refers to, or nullptr if there's
    // no current section or it's a solo section with an empty clip.
    const ChartClip* CurrentClip() const;

    // Returns the current section's play mode. Only meaningful when
    // CurrentClip() != nullptr or Phase() == Learning; returns
    // PlayMode::Learn as a harmless default otherwise.
    PlayMode CurrentPlayMode() const;

    int CurrentStreak() const;

    // Returns the beat of the next note this lane is awaiting a press for.
    double NextExpectedBeatForLane(int lane) const;

    const SongClock& Clock() const;

    // Returns the audio engine stem handle for a clip, for debugging.
    StemHandle DebugStemHandle(int clipIndex) const;

    // Returns and clears the most recent judgement (Hit/Miss/None).
    JudgementResult ConsumeLastJudgement();

    // Returns how a specific lane note (identified by its start beat) was
    // judged, for the note lane to color it once it's passed the line.
    // Returns None if that note hasn't been judged yet, or is too old to
    // still be tracked.
    JudgementResult OnsetJudgement(double startBeat, int lane) const;

    // True while this lane's press was judged correct and its release
    // hasn't been judged yet (early, on time, or via a timeout Miss).
    bool IsLaneHeld(int lane) const;

    // The start beat of the note this lane is currently holding. Only
    // meaningful when IsLaneHeld(lane) is true.
    double LaneHoldStartBeat(int lane) const;

    // True once the current section has locked in (learn) or finished
    // starting its clip (solo) and is just waiting for the scheduled time
    // before the next section begins. For a solo section, no judging
    // happens either way. For a learn section, presses keep being judged
    // as normal throughout this window - dots keep coming and keep turning
    // red/green - but misses stop affecting anything (the streak display
    // freezes and the clip loop no longer stops from consecutive misses),
    // since the section's fate is already sealed.
    bool IsAwaitingAdvance() const;

    // Returns the wall-clock second at which the current section's
    // already-scheduled advance will actually happen, or a negative value
    // if nothing is pending. Lets the note lane derive a guaranteed-to-fire
    // deadline for a locked-in clip's explosion, independent of whether (or
    // when) the next clip's own notes become visible.
    double PendingAdvanceAtSeconds() const;

    // True while the current learn section's chart-declared intro_bars are
    // still playing automatically, before dots/judging begin.
    bool IsInIntro() const;

    // Returns the clip whose dots should be shown as an early preview while
    // the player can't act yet - during the count-in, a clip's own
    // intro_bars, or the current section's own awaiting-advance hold (learn
    // or solo alike). Only ever returns non-null when the very next
    // non-Background section is itself Learn: skips forward over any
    // intervening Background section (since those collapse instantly and
    // never delay anything), but does NOT skip over an intervening Solo
    // section, since that solo's own screen time hasn't happened yet and is
    // itself the right moment to preview what comes after it. Returns
    // nullptr if there's nothing to preview right now.
    const ChartClip* PreviewClip() const;

    // Returns the beat position of the first note on this lane that
    // PreviewClip() will actually require once it goes live, computed the
    // same way BeginSection would at that moment - so the note lane can
    // filter out anything earlier and let notes scroll in one at a time
    // from the top edge per lane, instead of revealing a whole batch of
    // already-partially-scrolled-in notes at once when the preview first
    // becomes visible. Returns a negative value if there's nothing to
    // preview right now.
    double PreviewFirstOnsetBeatForLane(int lane) const;

private:
    // Begins (or resumes) the section at the given index, dispatching on
    // its play_mode. scheduledBeat is the ideal beat this transition was
    // scheduled for (e.g. kCountInSeconds or m_pendingAdvanceAtSeconds
    // converted to beats) - used instead of the actually-polled clock
    // position to pick a learn section's first note, so it's deterministic
    // and matches what PreviewFirstOnsetBeatForLane() already predicted.
    // Before dispatching, kicks off any background clip queued by the
    // previous section, since "the next section begins" is exactly this call.
    void BeginSection(int sectionIndex, double scheduledBeat);

    // Records a hit: advances the shared streak, resets the shared miss
    // counter, and starts the current section's clip loop (phase-aligned,
    // at init_volume) if it isn't already playing. The streak/miss counters
    // are left alone once already awaiting advance (frozen at their
    // lock-in value) - they no longer drive anything at that point.
    void RegisterHit();

    // Starts clipIndex's stem looping now (phase-aligned to the beat grid,
    // at the given volume) if it isn't already playing, and records the
    // start time in m_clipLoopStartSeconds[clipIndex] for loop_count to
    // measure from. Idempotent per clip - safe to call on a clip that's
    // already playing (e.g. already running as a background layer).
    // finiteLoopCount == 0 (the default) loops forever, for a clip whose
    // eventual stop time isn't known yet (learn/background). A positive
    // value is for a clip whose total loop_count is already known right
    // now (a solo section) - it's handed straight to AudioEngine so the
    // voice stops itself naturally and sample-accurately, rather than
    // relying on a later polled StopClipLoop() call to catch the exact
    // instant (which can let a fraction of a second of the loop's
    // beginning bleed through first).
    void StartClipLoop(int clipIndex, double volume, int finiteLoopCount = 0);

    // Stops clipIndex's stem if it's playing.
    void StopClipLoop(int clipIndex);

    // Records a miss: resets the shared streak, and stops the current
    // section's clip loop after 3 in a row. A no-op once already awaiting
    // advance - the track has already locked in, so further misses
    // shouldn't stop it or unfreeze the streak display.
    void RegisterMiss();

    // Moves this lane's next-expected-note pointer forward to the next note after it.
    void AdvanceExpectedNote(int lane);

    // Returns the start-tolerance window (seconds) to judge a press
    // against clip with, given clipIndex's current playback state. In
    // normal mode this is just the chart-declared value; in easy mode it's
    // widened by kEasyModeToleranceMultiplier unconditionally, and by a
    // further kEasyModeStoppedToleranceMultiplier on top of that while the
    // clip isn't currently playing - either because it hasn't started yet
    // or because RegisterMiss stopped it after too many misses - to help
    // the player get back on track.
    double EffectiveStartToleranceSeconds(const ChartClip& clip, int clipIndex) const;

    // Returns the smallest note start (in absolute beats) strictly after afterBeat, for this lane.
    double NextOnsetAfter(double afterBeat, const ChartClip& clip, int lane) const;

    // Returns the absolute beat of this lane's note in the first full
    // pattern cycle that starts at or after afterBeat, preserving every
    // lane's authored relative offset within that shared cycle - unlike
    // NextOnsetAfter, which searches each lane independently and can pick
    // a different cycle per lane, silently corrupting notes that are
    // meant to land together (or apart) exactly as authored. Only used
    // for a song's very first note(s) - see m_songHasStarted. Prefer
    // FirstReachableOnsetForAllLanes below over calling this directly, one
    // lane at a time - it only falls back to this (slower, always-a-fresh-
    // cycle) behavior when it's actually needed.
    double FirstReachableOnset(double afterBeat, const ChartClip& clip, int lane) const;

    // Computes every lane's anchor for the song's very first judged note
    // (or the first note after a clip's own intro_bars, if that's what's
    // being anchored) in one call. Tries each lane's own next reachable
    // note first (NextOnsetAfter, searched independently per lane) and
    // uses those directly if they all land in the same pattern cycle - no
    // desync risk in that case, and it starts judging as soon as each
    // lane's own next note is actually reachable instead of always
    // waiting for a fresh cycle, which matters a lot for a long-spanning
    // pattern (many bars) reached after a leading solo/background section
    // or a count-in offset that doesn't land near a bar boundary - the
    // wait could otherwise be most of an entire loop of dead air. Falls
    // back to FirstReachableOnset's per-lane, always-safe behavior only
    // when the lanes' own candidates would actually land in different
    // cycles, which is exactly the scenario that behavior exists to guard
    // against.
    void FirstReachableOnsetForAllLanes(double afterBeat, const ChartClip& clip, double outBeats[kLaneCount]) const;

    // Returns the lane note whose phase-within-span matches
    // absoluteStartBeat's phase, or nullptr if none does (shouldn't happen
    // for a beat that came from NextOnsetAfter against the same clip/lane)
    // - used to look up a note's duration once its press has been judged correct.
    const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double absoluteStartBeat) const;

    // Returns the wall-clock seconds at which PreviewClip() will actually
    // go live, or a negative value if there's nothing to preview.
    double PreviewTransitionSeconds() const;

    // Returns the index of the first section at or after startIndex whose
    // play_mode isn't Background (Background sections never persist as
    // "current" - BeginSection always recurses straight through them
    // within the same call - so they're never a real waiting/preview
    // point), or -1 if none remain. Used by PreviewClip() to find the next
    // real stopping point; whether that section is itself Learn is checked
    // separately by the caller, since an intervening Solo section is a
    // real stopping point too, just not a previewable one.
    int NextNonBackgroundSectionAtOrAfter(int startIndex) const;

    // If the clip's declared span is shorter than its stem's actual
    // duration, tiles each lane's notes independently to fill the whole
    // clip and widens spanBeats to match. A trailing repeat that would be
    // cut off mid-note (its start fits before the loop wraps but its
    // duration wouldn't finish in time) drops that note rather than
    // shipping one the player could never legitimately press and release.
    static void ExpandLaneNotesToFillClip(ChartClip& clip, double stemDurationSeconds, double bpm);

    // When easy mode is on, simplifies clip's MIDI-derived pattern before
    // it's tiled/judged: every note is rounded up to the next quarter-note
    // beat (never earlier - "combined" notes start at or after where the
    // originals began), multiple originals landing on the same beat within
    // a lane collapse into one, and a beat claimed by more than one lane
    // survives only in the lowest-indexed lane that claims it (no
    // simultaneous notes). Every surviving note is shortened to
    // kEasyModeNoteDurationBeats (half the quarter-note grid it's spaced
    // on), so consecutive notes always leave a visible gap instead of
    // running into each other. No-op unless clip.hasMidi. Must run before
    // ExpandLaneNotesToFillClip, while spanBeats still means "one
    // repetition's length" - tiling afterward just repeats whatever this
    // produces, so it never needs to know easy mode exists.
    static void ApplyEasyModeTransform(ChartClip& clip);

    // Computes the wall-clock second at which loopCount full loops have
    // completed, counted from loopStartSeconds, given a stem of
    // stemDuration seconds - shared by a learn section's lock-in floor, a
    // solo section's unconditional wait, and a background layer's
    // self-stop time. Returns loopStartSeconds itself if stemDuration <= 0.
    static double ComputeLoopFloorSeconds(double loopStartSeconds, double stemDuration, int loopCount);

    // Called once the shared streak meets a learn section's clip
    // requirement: schedules the advance to the next section (or
    // Complete), and the switch from init_volume to volume, for the next
    // time the clip's stem wraps back to the start of a playthrough - or
    // later still, if the section's loop_count demands more full loops
    // than have played since the clip started, or if that isn't already at
    // least kNoteFallBeats of real time away (so the next section's notes
    // always get their full on-screen travel time to preview before they
    // must be played, even if the player locks in right before a loop
    // boundary).
    void SchedulePendingAdvance();

    // Records a judgement for a specific lane note, for OnsetJudgement() to
    // look up later. Trims old entries so this can't grow unbounded.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result);

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<StemHandle> m_stemHandles; // one full-loop stem per clip, indexed by clip index

    struct JudgedLaneNote
    {
        double beat = 0.0;
        int lane = 0;
        JudgementResult result = JudgementResult::None;
    };
    std::vector<JudgedLaneNote> m_judgedNotes;

    // A lane currently mid-hold: its press was judged correct and its
    // release hasn't been judged yet (by a real key-up or by Update()'s
    // held-past-tolerance timeout).
    struct LaneHold
    {
        bool active = false;
        double startBeat = 0.0;
        double expectedEndBeat = 0.0;
    };

    // A background clip queued by a `background` section, to actually
    // start the moment the *next* BeginSection() call happens. clipIndex
    // -1 means nothing queued.
    struct QueuedBackground
    {
        int clipIndex = -1;
        int loopCount = 0;
    };

    SongClock m_clock;
    GamePhase m_phase = GamePhase::Idle;
    int m_currentSectionIndex = -1;
    int m_streak = 0;
    int m_consecutiveMisses = 0;

    // Set once from LoadChart's easyMode argument and left alone for the
    // rest of this chart's lifetime - see ApplyEasyModeTransform,
    // OnPress/OnRelease's judging differences, and RegisterMiss's grace check.
    bool m_easyMode = false;

    // One-note grace period (easy mode only): true until this Learn
    // section's first miss consumes it in RegisterMiss - that miss is then
    // fully forgiven (streak and consecutive-miss counter both left
    // untouched). Reset in BeginSection alongside m_streak/
    // m_consecutiveMisses. Never consulted when m_easyMode is false.
    bool m_easyGraceAvailable = false;

    // Per-clip playback bookkeeping - multiple clips can legitimately play
    // concurrently now (the current learn/solo clip, plus zero or more
    // background layers), so this replaced a pair of single scalars.
    // Both sized to m_song.clips.size() on every LoadChart.
    std::vector<bool> m_clipIsPlaying;
    std::vector<double> m_clipLoopStartSeconds;

    QueuedBackground m_queuedBackground;

    double m_nextExpectedBeat[kLaneCount] = {};
    LaneHold m_laneHolds[kLaneCount];
    JudgementResult m_lastJudgement = JudgementResult::None;

    // Set once a learn section's shared streak meets its clip's
    // requirement, or immediately on entering a solo section; new presses
    // stop being judged and the session advances at m_pendingAdvanceAtSeconds.
    bool m_hasPendingAdvance = false;
    double m_pendingAdvanceAtSeconds = 0.0;

    // Set while the current learn section's clip-declared intro_bars are
    // still playing automatically; presses aren't judged until
    // m_introEndSeconds is reached.
    bool m_isInIntro = false;
    double m_introEndSeconds = 0.0;

    // False until the very first note of the song has been anchored (at
    // the first learn section actually reached - possibly after a leading
    // solo/background section - either directly, or once its intro_bars
    // finish). While false, that anchor uses FirstReachableOnset instead
    // of plain NextOnsetAfter, searching every lane against the SAME
    // pattern cycle instead of each lane independently: scheduledBeat at
    // the start of a song is derived from kCountInSeconds (or that plus a
    // whole number of intro bars), a fixed real-world duration with no
    // musical meaning, so at most tempos it doesn't land on the pattern's
    // own bar boundary. Searching lanes independently there can land
    // different lanes on different pattern cycles - e.g. one lane's true
    // opening note gets skipped while another lane's doesn't, corrupting
    // notes that are meant to land together (or apart) exactly as
    // authored (this is exactly what produced a real reported bug: a
    // clip whose file starts with one note then a two-note beat instead
    // played its two-note beat first). Once any section is underway,
    // NextOnsetAfter's normal per-lane "pick up wherever the loop
    // naturally is" is exactly the right behavior, so this only ever
    // matters once, at the very start of a song.
    bool m_songHasStarted = false;
};
