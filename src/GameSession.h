#pragma once

#include <string>
#include <vector>

#include "AudioEngine.h"
#include "ChartFile.h"
#include "LaneConfig.h"
#include "SectionInstance.h"
#include "SongClock.h"

// The stages a game session moves through, in order, once per song.
enum class GamePhase
{
    Idle,
    CountIn,
    Learning, // a section is active - covers learn/break/reset/background alike; see CurrentSectionKind() for which
    Complete,
};

// Drives the chart's ordered list of sections, one at a time. Each section
// has a kind ([learn]/[break]/[reset]/[background] in the .chart file) and,
// except for [reset], references a reusable clip (a stem + MIDI pattern +
// judging thresholds):
//   - Learn ([learn]): judges key presses/releases against the clip's
//     MIDI-derived pattern (via SongClock), exactly as a lone "learn one
//     clip at a time" flow always has. Each of the kLaneCount lanes
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
//     see ClipVoice::originEstablished. The section's first candidate
//     advance is computed that same instant (loop_count full loops,
//     floored the same way a break section's own wait is, via
//     ChartTiming::ComputeLearnAdvanceSeconds). Hitting hits_required worth
//     of the shared streak locks the clip in - IsLockedIn() flips true, its
//     volume switches from init_volume to volume, and misses stop mattering
//     (no more stopping the loop after 3 in a row). The section only
//     actually advances once BOTH the current candidate advance arrives AND
//     the clip is locked in by then - if it isn't, the clip's loop simply
//     repeats (the candidate advance pushes back by one more full loop) and
//     the same check happens again at that new instant, however many times
//     it takes. A clip that's never proving itself just keeps looping
//     forever rather than being abandoned - see Update()'s own comment.
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
//
// Live, per-section-run judging state (streak, isLockedIn, pending-advance,
// lane holds, judged notes) lives in SectionInstance, not here directly -
// GameSession just owns "the current one" and replaces it wholesale every
// time a new section begins. A clip's own playback voice (is it playing,
// where its groove started) is tracked separately, per clip index, in the
// private ClipVoice below - shared across every section that ever
// references that clip, since two sections reusing the same clip must
// never disagree about it.
//
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

    // Returns the current section's clip's own persistent phase origin, in
    // beats (see ClipVoice::originEstablished) - the note lane needs this
    // to tile CurrentClip()'s pattern into absolute beat-space
    // (NotesInRange) exactly the way it's actually judged, since a clip's
    // cycle boundaries are no longer at multiples of spanBeats from
    // absolute beat 0. 0 if there's no current clip (harmless default,
    // never read in that case).
    double CurrentClipOriginBeat() const;

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
    // lock in). Drives the glowing note outline, the confetti burst, and
    // the init_volume -> volume switch, same as always - but now also
    // gates the section's own advance directly: while this is false, the
    // section's candidate advance (PendingAdvanceAtSeconds()) keeps pushing
    // back by a full loop every time it's reached, instead of ever actually
    // firing. See BeginSection's/Update()'s own Learn-case comments.
    bool IsLockedIn() const;

    // Returns the wall-clock second the current section is *currently*
    // scheduled to advance at, or a negative value if nothing is pending.
    // For a break section (or an already-locked-in learn section) this is
    // final. For a learn section not yet locked in, it's only provisional -
    // every time this instant is reached without IsLockedIn() having gone
    // true, it pushes back by one more full loop (see Update()'s own
    // comment) - so a caller polling this every frame will see it hold
    // steady, then jump forward a whole loop, however many times it takes.
    // Lets the note lane derive a guaranteed-to-fire deadline for a
    // locked-in clip's explosion, independent of whether (or when) the next
    // clip's own notes become visible.
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
    // itself the right moment to preview what comes after it. Also
    // returns nullptr the whole time the current section is a not-yet-
    // locked-in learn section: it doesn't yet know whether it's about to
    // advance or repeat itself another loop, so there's nothing legitimate
    // to preview - the clip's own notes simply keep scrolling by
    // themselves in that case (see CurrentClip()), no preview needed.
    // Returns nullptr if there's nothing to preview right now.
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

    // Returns PreviewClip()'s own persistent phase origin, in beats -
    // mirrors PreviewFirstOnsetBeatForLane's own anchoring choice exactly
    // (a not-yet-established clip's predicted origin is the same
    // transition beat PreviewFirstOnsetBeatForLane already uses), so the
    // note lane can tile PreviewClip()'s pattern into the same
    // absolute-beat-space its onsets are computed in (NotesInRange).
    // Returns a negative value if there's nothing to preview right now.
    double PreviewClipOriginBeat() const;

private:
    // One clip's playback voice: is it currently playing, where (in
    // wall-clock seconds) its current loop started, and its persistent
    // phase origin - see ClipVoice::originEstablished below. Indexed by
    // clip index, one per clip in the loaded song, and shared by every
    // section that ever references that clip: a later section reusing an
    // already-started clip continues its existing groove rather than
    // restarting it (see originEstablished), so this state fundamentally
    // belongs to the clip, not to whichever SectionInstance happens to be
    // current right now.
    struct ClipVoice
    {
        bool isPlaying = false;
        double loopStartSeconds = 0.0;

        // False until this clip has been started for the very first time
        // (Learn, Break, or Background alike - possibly long after other
        // clips have already started the song off). While false,
        // StartClipLoop (and, for a Learn/Break section,
        // EnsureClipOriginEstablished called ahead of it) establishes
        // originSeconds to that exact instant - this clip's own persistent
        // phase reference from then on, used by every later ChartTiming
        // call against it (NextOnsetAfter, ComputeClipPhaseSeconds,
        // ComputeLearnAdvanceSeconds, ComputeBreakAdvance) instead of
        // absolute beat/second 0. This is what makes a clip always start
        // playing (audibly and visually) from its own true beginning the
        // instant it starts, whatever arbitrary point in the song's
        // overall beat grid that happens to be - a different clip playing
        // in the background is entirely unaffected, and simply keeps
        // looping on its own already-established origin (see
        // GameSession.cpp's ComputeLearnAdvanceSeconds callsite comment
        // for why this was needed: two clips' first appearances genuinely
        // don't need to share a beat grid with each other). Once a given
        // clip has been started once, every later restart (a 3-miss
        // recovery, or a later section reusing the same clip) keeps using
        // that SAME established origin, keeping its groove internally
        // continuous instead of restarting it from scratch each time.
        bool originEstablished = false;
        double originSeconds = 0.0;
    };

    // Begins (or resumes) the section at the given index, dispatching on
    // its kind. scheduledBeat is the ideal beat this transition was
    // scheduled for (e.g. CountInSeconds() or the previous instance's own
    // pending-advance seconds, converted to beats) - used instead of the
    // actually-polled clock position to pick a learn section's first note,
    // so it's deterministic and matches what PreviewFirstOnsetBeatForLane()
    // already predicted. Before dispatching, kicks off any background clip
    // queued by the previous section, since "the next section begins" is
    // exactly this call.
    void BeginSection(int sectionIndex, double scheduledBeat);

    // Records a hit: advances the shared streak, resets the shared miss
    // counter, and starts the current section's clip loop (phase-aligned,
    // at init_volume) if it isn't already playing. The streak/miss counters
    // are left alone once already locked in (frozen at their lock-in
    // value) - they no longer drive anything at that point.
    void RegisterHit();

    // Starts clipIndex's stem looping now (phase-aligned to its own
    // persistent origin - see EnsureClipOriginEstablished/
    // ClipVoice::originEstablished - at the given volume) if it isn't
    // already playing, and records the start time in
    // m_clipVoices[clipIndex].loopStartSeconds for loop_count to measure
    // from. Idempotent per clip - safe to call on a clip that's already
    // playing (e.g. already running as a background layer).
    // finiteLoopCount == 0 (the default) loops forever, for a clip whose
    // eventual stop time isn't known yet (learn/background). A positive
    // value is for a clip whose total loop_count is already known right
    // now (a break section) - it's handed straight to AudioEngine so the
    // voice stops itself naturally and sample-accurately, rather than
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
    // section's clip loop after 3 in a row. A no-op once already locked
    // in - further misses shouldn't stop the clip or unfreeze the streak
    // display once it's proven itself. Before lock-in, a miss's only
    // effect on the section's own advance timing is indirect: resetting
    // the streak makes lock-in (and therefore advancing) take longer to
    // reach, possibly costing the clip another full loop's repeat - see
    // Update()'s own comment.
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
    // absoluteStartBeat's phase (measured relative to originBeat - this
    // clip's own persistent phase reference, see ClipVoice::
    // originEstablished - not absolute beat 0), or nullptr if none does
    // (shouldn't happen for a beat that came from NextOnsetAfter/
    // FreshOnsetForAllLanes against the same clip/lane/origin) - used to
    // look up a note's duration once its press has been judged correct.
    const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat) const;

    // Returns the wall-clock seconds at which PreviewClip() will actually
    // go live, or a negative value if there's nothing to preview.
    double PreviewTransitionSeconds() const;

    // Returns the section index PreviewClip()/PreviewFirstOnsetBeatForLane()
    // are currently previewing, or -1 if nothing is being previewed right
    // now - the shared logic both of those built on top of, factored out
    // so PreviewFirstOnsetBeatForLane() can look up the previewed clip's
    // own index (needed for the ClipVoice::originEstablished check)
    // without duplicating PreviewClip()'s own section-walking logic.
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
    // look up later - forwards to m_currentInstance, adding
    // RHYTHM_DEBUG_JUDGEMENTS tracing.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result);

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<StemHandle> m_stemHandles; // one full-loop stem per clip, indexed by clip index

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

    // Set once from LoadChart's easyMode argument and left alone for the
    // rest of this chart's lifetime - see ApplyEasyModeTransform,
    // OnPress/OnRelease's judging differences, and RegisterMiss's grace check.
    bool m_easyMode = false;

    // The live judging state of whichever section is current right now
    // (SectionIndex() == -1 if none is) - replaced wholesale by BeginSection
    // every time a new section begins. See SectionInstance's own comment.
    SectionInstance m_currentInstance;

    // Per-clip playback voices, indexed the same way m_stemHandles is -
    // sized to m_song.clips.size() on every LoadChart. See ClipVoice's own
    // comment for why this lives here (per clip), not on SectionInstance
    // (per section).
    std::vector<ClipVoice> m_clipVoices;

    QueuedBackground m_queuedBackground;
    JudgementResult m_lastJudgement = JudgementResult::None;
};
