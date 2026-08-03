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
    Learning, // a section is active - covers learn/break/reset/background alike; see CurrentSectionKind() for which
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
// has a kind ([learn]/[break]/[reset]/[background] in the .chart file) and,
// except for [reset], references a reusable clip (a stem + MIDI pattern +
// judging thresholds):
//   - Learn ([learn]): judges key presses/releases against the clip's
//     MIDI-derived pattern (via SongClock), exactly as a lone "learn one
//     instrument at a time" flow always has. Each of the kLaneCount lanes
//     tracks its own note sequence completely independently - its own
//     next-expected-note pointer, its own press/release judging, its own
//     retry-on-mistimed-press behavior - the lanes share nothing but the
//     streak/consecutive-miss counters, so a chart author can require
//     simultaneous presses across lanes just by placing simultaneous notes
//     in the MIDI file, with no special "chord" bookkeeping anywhere in
//     here. The clip's loop starts playing (at init_volume) the instant the
//     section begins - not gated on any press, and (if this is the clip's
//     very first start, ever) always at its own true pattern beginning, no
//     matter where in the song's overall beat grid that happens to fall -
//     see m_clipOriginEstablished. The section advances on a fixed schedule
//     computed that same instant
//     (loop_count full loops, floored the same way a break section's own
//     wait is, via ChartTiming::ComputeLearnAdvanceSeconds), independent of
//     whether the player ever plays a single note. Hitting hits_required
//     worth of the shared streak locks the clip in - IsLockedIn() flips
//     true, its volume switches from init_volume to volume, and misses stop
//     mattering (no more stopping the loop after 3 in a row) - but none of
//     that touches the section's own advance timing at all, which was
//     already decided. If the section's own advance arrives before the
//     player ever locks in, the clip simply stops (it never proved itself,
//     so it doesn't join the arrangement) instead of continuing to play.
//   - Break ([break]): stops every clip currently playing, starts this
//     section's clip looping, and blocks advancing until loop_count full
//     loops complete - no judging happens. Advancing is also floored at
//     kNoteFallBeats of real time from when the section began, same as a
//     locked-in learn section, so the next section's notes always get
//     their full on-screen travel time to preview before going live.
//   - Reset ([reset]): stops every clip currently playing (a silence gate,
//     no clip of its own) and advances immediately - zero elapsed time,
//     exactly like Background below (neither ever occupies
//     CurrentSectionIndex()/CurrentSectionKind() for an observable moment).
//   - Background ([background]): queues this section's clip to start
//     playing (without stopping anything else) at the moment the *next*
//     section begins, then keeps looping indefinitely - exactly like a
//     locked-in learn clip - until a later break/reset section's
//     StopAll() (or Stop()/Start()) silences it; loop_count has no effect
//     on background sections. This section itself takes zero time and
//     never blocks.
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
    // Learn section's live judging (count-in, break/reset/
    // background, or no chart loaded at all), or a lane the current clip
    // never places any notes in at all. Lets the caller show its own "no note there"
    // feedback for a press this lane will otherwise just silently ignore.
    // Call CatchUpCountIn() first if a real press just happened - see its
    // own comment for why.
    bool IsLaneJudgeable(int lane) const;

    // If still in the count-in but real elapsed time has already reached
    // its end, begins the first section immediately instead of waiting for
    // the next Update() tick to notice. The count-in now ends exactly at
    // the first section's own first note (see CountInSeconds()), so a
    // press landing right around that instant is common, not a fluke - and
    // without this, IsLaneJudgeable's phase check would reject it outright
    // (count-in still technically in progress) even though it falls well
    // within that note's own tolerance window, purely because Update()
    // hadn't ticked yet to notice the boundary had passed. Safe to call
    // unconditionally; a no-op once the count-in has already ended by any
    // path.
    void CatchUpCountIn();

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
    // no current section or it's a [reset] section (the only kind with no clip).
    const ChartClip* CurrentClip() const;

    // Returns the current section's kind. Only meaningful when
    // CurrentClip() != nullptr or Phase() == Learning; returns
    // SectionKind::Learn as a harmless default otherwise.
    SectionKind CurrentSectionKind() const;

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

    // True whenever the current section (learn or break) has a scheduled
    // advance at all - which is to say, true for essentially the entire
    // time either kind is current, since both schedule their own advance
    // the instant they begin, independent of the player. Reset can never be
    // the current section here - it advances immediately, with no waiting
    // period of its own.
    bool IsAwaitingAdvance() const;

    // True once the current learn section's shared streak has met its
    // clip's hits_required - always false for a break section (nothing to
    // lock in). Purely a visual/audio-treatment signal now (the glowing
    // note outline, the confetti burst, the init_volume -> volume switch,
    // and misses no longer stopping the clip's loop) - it has no effect on
    // when the section actually advances, which was already decided the
    // instant the section began. If this is still false when the section's
    // own advance arrives, the clip simply stops rather than continuing to
    // play into later sections.
    bool IsLockedIn() const;

    // Returns the wall-clock second at which the current section's
    // already-scheduled advance will actually happen, or a negative value
    // if nothing is pending. Lets the note lane derive a guaranteed-to-fire
    // deadline for a locked-in clip's explosion, independent of whether (or
    // when) the next clip's own notes become visible.
    double PendingAdvanceAtSeconds() const;

    // Returns the clip whose dots should be shown as an early preview while
    // the player can't act yet - during the count-in, or the current
    // section's own awaiting-advance hold (learn or break alike). Only
    // ever returns non-null when the very next persistent section (see
    // NextPersistentSectionAtOrAfter) is itself Learn: skips forward over
    // any intervening Background or Reset section (neither ever occupies
    // real screen time - both collapse instantly and never delay
    // anything), but does NOT skip over an intervening Break section,
    // since that section's own screen time hasn't happened yet and is
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
    // its kind. scheduledBeat is the ideal beat this transition was
    // scheduled for (e.g. CountInSeconds() or m_pendingAdvanceAtSeconds
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

    // Starts clipIndex's stem looping now (phase-aligned to its own
    // persistent origin - see EnsureClipOriginEstablished/
    // m_clipOriginEstablished - at the given volume) if it isn't already
    // playing, and records the start time in m_clipLoopStartSeconds[clipIndex]
    // for loop_count to measure from. Idempotent per clip - safe to call on
    // a clip that's already playing (e.g. already running as a background
    // layer). finiteLoopCount == 0 (the default) loops forever, for a clip
    // whose eventual stop time isn't known yet (learn/background). A
    // positive value is for a clip whose total loop_count is already known
    // right now (a break section) - it's handed straight to AudioEngine so
    // the voice stops itself naturally and sample-accurately, rather than
    // relying on a later polled StopClipLoop() call to catch the exact
    // instant (which can let a fraction of a second of the loop's
    // beginning bleed through first).
    void StartClipLoop(int clipIndex, double volume, int finiteLoopCount = 0);

    // Ensures clipIndex has a persistent pattern origin - the wall-clock
    // second its own audio and (for a Learn clip) judged-note timeline are
    // measured relative to - establishing it to nowSeconds the first time
    // this is ever called for that clip and leaving it untouched on every
    // later call. Called explicitly, ahead of any origin-dependent
    // computation, by BeginSection's Learn and Break cases (each needs the
    // origin before StartClipLoop itself would otherwise establish it);
    // StartClipLoop also calls it as a safety net, covering Background and
    // any other path that starts a clip without going through Learn/Break.
    // Returns true only when this call just established it - the caller
    // needs to know, since a fresh origin calls for
    // ChartTiming::FreshOnsetForAllLanes instead of plain NextOnsetAfter.
    bool EnsureClipOriginEstablished(int clipIndex, double nowSeconds);

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

    // Returns how long the count-in lasts, in seconds: one full bar at the
    // song's own tempo/time signature, so it's always musically aligned
    // instead of an arbitrary wall-clock duration.
    double CountInSeconds() const;

    // NextOnsetAfter/FreshOnsetForAllLanes moved to ChartTiming.h (shared
    // with the editor's analytical block scheduler) - see that header for
    // their doc comments.

    // Returns the lane note whose phase-within-span matches
    // absoluteStartBeat's phase, or nullptr if none does (shouldn't happen
    // for a beat that came from NextOnsetAfter against the same clip/lane)
    // - used to look up a note's duration once its press has been judged correct.
    const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double absoluteStartBeat) const;

    // Returns the wall-clock seconds at which PreviewClip() will actually
    // go live, or a negative value if there's nothing to preview.
    double PreviewTransitionSeconds() const;

    // Returns the section index PreviewClip()/PreviewFirstOnsetBeatForLane()
    // are currently previewing, or -1 if nothing is being previewed right
    // now - the shared logic both of those built on top of, factored out
    // so PreviewFirstOnsetBeatForLane() can look up the previewed clip's
    // own index (needed for the m_clipOriginEstablished check) without
    // duplicating PreviewClip()'s own section-walking logic.
    int PreviewSectionIndex() const;

    // Returns the index of the first section at or after startIndex whose
    // kind is neither Background nor Reset (neither ever persists as
    // "current" - BeginSection always recurses straight through both
    // within the same call - so neither is ever a real waiting/preview
    // point), or -1 if none remain. Used by PreviewClip() to find the next
    // real stopping point; whether that section is itself Learn is checked
    // separately by the caller, since an intervening Break section is a
    // real stopping point too, just not a previewable one.
    int NextPersistentSectionAtOrAfter(int startIndex) const;

    // ExpandLaneNotesToFillClip moved to ChartTiming.h (shared with the
    // editor's analytical block scheduler).

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

    // ComputeLoopFloorSeconds moved to ChartTiming.h (shared with the
    // editor's analytical block scheduler).

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
    // concurrently now (the current learn/break clip, plus zero or more
    // background layers), so this replaced a pair of single scalars.
    // Both sized to m_song.clips.size() on every LoadChart.
    std::vector<bool> m_clipIsPlaying;
    std::vector<double> m_clipLoopStartSeconds;

    QueuedBackground m_queuedBackground;

    double m_nextExpectedBeat[kLaneCount] = {};
    LaneHold m_laneHolds[kLaneCount];
    JudgementResult m_lastJudgement = JudgementResult::None;

    // Set immediately on entering a learn or break section - both schedule
    // their own advance to m_pendingAdvanceAtSeconds the instant they
    // begin, regardless of the player. (Reset never sets this - it
    // advances immediately, within the same BeginSection call, with
    // nothing left to await.)
    bool m_hasPendingAdvance = false;
    double m_pendingAdvanceAtSeconds = 0.0;

    // True once the current learn section's shared streak has met its
    // clip's hits_required - see IsLockedIn()'s own comment for what this
    // does and doesn't affect. Reset to false in BeginSection alongside
    // m_streak/m_consecutiveMisses; always false for a break section.
    bool m_lockedIn = false;

    // Per clip index: false until that clip has been started for the very
    // first time (Learn, Break, or Background alike - possibly long after
    // other clips have already started the song off). Sized/reset alongside
    // m_clipIsPlaying. While false for a given clip, StartClipLoop (and, for
    // a Learn/Break section, EnsureClipOriginEstablished called ahead of it)
    // establishes m_clipOriginSeconds[clipIndex] to that exact instant - the
    // clip's own persistent phase reference from then on, used by every
    // later ChartTiming call against it (NextOnsetAfter, ComputeClipPhaseSeconds,
    // ComputeLearnAdvanceSeconds, ComputeBreakAdvance) instead of absolute
    // beat/second 0. This is what makes a clip always start playing (audibly
    // and visually) from its own true beginning the instant it starts,
    // whatever arbitrary point in the song's overall beat grid that happens
    // to be - a different clip playing in the background is entirely
    // unaffected, and simply keeps looping on its own already-established
    // origin (see GameSession.cpp's ComputeLearnAdvanceSeconds callsite
    // comment for why this was needed: two clips' first appearances
    // genuinely don't need to share a beat grid with each other). Once a
    // given clip has been started once, every later restart (a 3-miss
    // recovery, or a later section reusing the same clip) keeps using that
    // SAME established origin, keeping its groove internally continuous
    // instead of restarting it from scratch each time. Keyed per clip (not a
    // single whole-song flag): a *different* clip's first start can happen
    // at any point in the song, independent of whether some other clip
    // already established its own origin first.
    std::vector<bool> m_clipOriginEstablished;
    std::vector<double> m_clipOriginSeconds;
};
