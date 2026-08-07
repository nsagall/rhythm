#pragma once

#include <string>
#include <unordered_map>
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
//     see ClipInstance::startSeconds. The section's first candidate
//     advance is computed that same instant (loop_count full loops,
//     floored the same way a break section's own wait is, via
//     ChartTiming::ComputeLearnAdvanceSeconds). Hitting hits_required worth
//     of the shared streak starts the clip passing - IsPassing() flips
//     true, its volume switches from init_volume to volume. In the default
//     Pass mode (ChartClip::learnMode), that's permanent for the rest of
//     the section's run and misses stop mattering (no more stopping the
//     loop after 3 in a row); in DontFail mode it's reversible - see
//     IsPassing()'s and SectionInstance::RegisterMiss's own comments. The
//     section only actually advances once BOTH the current candidate
//     advance arrives AND the clip is passing by then - if it isn't, the
//     clip's loop simply repeats (the candidate advance pushes back by one
//     more full loop) and the same check happens again at that new
//     instant, however many times it takes. A clip that's never proving
//     itself just keeps looping forever rather than being abandoned - see
//     Update()'s own comment.
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
// Live, per-section-run judging state (streak, isPassing, pending-advance,
// lane holds, judged notes) lives in SectionInstance, not here directly -
// GameSession just owns "the current one" and replaces it wholesale every
// time a new section begins. A clip's own playback voice (is it playing,
// where its groove started) is tracked separately, per clip, in the
// private ClipInstance below - shared across every section that ever
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

    // Freezes the judging clock and every currently-playing clip's audio in
    // place - Update() becomes a no-op, and OnPress ignores new presses,
    // until Resume(). A no-op if already paused. Doesn't touch Phase() or
    // any judging state (streak/passing/holds) - pausing is purely "stop
    // time from advancing," not a phase of its own, so a caller mid-Learning
    // stays mid-Learning across a pause exactly as it was.
    void Pause();

    // Resumes from Pause(): re-anchors the clock so no time appears to have
    // passed during the pause, and resumes every clip's audio from exactly
    // where Pause() left it (see SongClock::Resume/AudioEngine::ResumeAll).
    // A no-op if not currently paused.
    void Resume();

    bool IsPaused() const;

    // Registers a key-down for the given lane at the current moment;
    // judges it against that lane's next expected note if the current
    // section is a "learn" section. A no-op while paused (see Pause()).
    void OnPress(int lane);

    // True exactly when OnPress(lane) would actually judge a press right
    // now (whether it turns out to be a timely hit or a mistimed miss) -
    // false whenever there's structurally nothing to press: outside a
    // Learn section's live judging (count-in, break/reset/
    // background, or no chart loaded at all), or a lane the current clip
    // never places any notes in at all. Deliberately NOT false while paused
    // (unlike OnPress itself) - a caller gating lane input on pause (see
    // MainWindow::OnKeyDown) should do so before ever reaching this, rather
    // than through it, so a paused press stays silently ignored instead of
    // reading as "no note there" and flashing a miss. Lets the caller show
    // its own "no note there" feedback for a press this lane will otherwise
    // just silently ignore. Call CatchUpCountIn() first if a real press
    // just happened - see its own comment for why.
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
    // against the note that lane was holding, if any. Not gated by phase,
    // play mode, or pause - a hold already in flight resolves on its own
    // merits even if the section has since started passing, so it still
    // paints its true outcome instead of being abandoned mid-air; releasing
    // one while paused judges it against the clock's frozen instant, same
    // as any other read of it while paused.
    void OnRelease(int lane);

    // Advances count-in/miss-detection/hold-timeout timing; call once per
    // frame. A complete no-op while paused (see Pause()) - nothing about
    // judging, timeouts, or section advancement progresses until Resume().
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

    // Returns the running score for the song currently loaded/playing: the
    // sum of every section's already-banked score plus whatever the current
    // section has built up so far but hasn't banked yet - see
    // RegisterHit/RegisterMiss/Update()'s own banking comment. Reset to 0 by
    // every Start(); persists across a Pause()/Resume() and past
    // GamePhase::Complete (the caller reads it once the session reaches
    // Complete, before the next Start() zeroes it again).
    int CurrentScore() const;

    // Returns the current section's not-yet-banked score alone (m_sessionScore -
    // see CurrentScore()'s own comment): the amount still at risk to the next
    // real miss, and what a "points building up" display should show apart
    // from the permanent running total. 0 whenever nothing's been judged yet
    // this section, including immediately after a bank/wipe - see
    // ConsumeScoreEvents for the moment-of-transfer amount instead.
    int PendingScore() const;

    // Returns the beat of the next note this lane is awaiting a press for.
    double NextExpectedBeatForLane(int lane) const;

    // Returns the current section's clip's own persistent phase origin, in
    // beats (see ClipInstance::startSeconds) - the note lane needs this
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

    // One judgement (Hit or Miss - RegisterHit/RegisterMiss are the only
    // producers, so None never appears here), on a specific lane, exactly as
    // it was at the instant it happened - passing is captured then rather
    // than left for the caller to re-query later, since IsPassing() may have
    // already moved on (e.g. a DontFail miss flipping it) by the time this
    // event is actually drained.
    struct JudgementEvent
    {
        JudgementResult result = JudgementResult::None;
        int lane = -1;
        bool passing = false;

        // Only meaningful when result == Hit (a Miss has no "how close" to
        // report): false if the press that earned this hit landed more than
        // half of the effective start tolerance away from the note's own
        // onset - still a correct, in-tolerance press, just not a precise
        // one, and scored for less (see GameSession::RegisterHit/
        // ScoreForHit). The note lane's ripple renders yellow instead of
        // green for one of these, rather than treating every Hit alike.
        bool precise = true;
    };

    // Returns and clears every judgement RegisterHit/RegisterMiss recorded
    // since the last call - unlike ConsumeLastJudgement (a single scalar,
    // overwritten by whichever judgement happens to land last), this never
    // drops one: OnPress/OnRelease produce at most one each, but Update()'s
    // press-phase/hold timeouts can judge several lanes in a single call.
    // The intended reader is whatever's responsible for a judgement's visual
    // feedback (see NoteLane::ShowJudgement) - draining this after every
    // OnPress/OnRelease/Update call makes a timeout-driven miss go through
    // that exact same feedback path as an explicit mistimed press/release,
    // instead of the two being handled differently.
    std::vector<JudgementEvent> ConsumeJudgementEvents();

    // One transfer in or out of the pending (not-yet-banked) score pool -
    // Banked when a section finishes and m_sessionScore folds permanently
    // into m_bankedScore (Update()'s own banking comment), Lost when a real
    // miss wipes m_sessionScore back to 0 (RegisterMiss). Only ever pushed
    // with amount > 0 - a section finishing (or a miss landing) with nothing
    // pending produces no event, since there'd be nothing for a "points
    // banking" animation to show moving.
    struct ScoreEvent
    {
        enum class Kind
        {
            Banked,
            Lost,
        };
        Kind kind = Kind::Banked;
        int amount = 0;
    };

    // Returns and clears every ScoreEvent since the last call - same
    // drain-and-clear contract as ConsumeJudgementEvents, for the same
    // reason (Update()'s own banking can fire independently of any
    // OnPress/OnRelease call, so nothing here should be allowed to drop one
    // between calls). Intended reader: the note lane's points-banking
    // animation.
    std::vector<ScoreEvent> ConsumeScoreEvents();

    // Returns how a specific lane note (identified by its start beat) was
    // judged, for the note lane to color it once it's passed the line.
    // Returns None if that note hasn't been judged yet, or is too old to
    // still be tracked.
    JudgementResult OnsetJudgement(double startBeat, int lane) const;

    // Whether the same lane note's OnsetJudgement Hit was precise - see
    // SectionInstance::OnsetPrecise. Meaningless (and defaults to true) when
    // OnsetJudgement doesn't return Hit for the same (startBeat, lane).
    bool OnsetPrecise(double startBeat, int lane) const;

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
    // pass). Drives the glowing note outline, the confetti burst, and the
    // init_volume -> volume switch, same as always - but now also gates the
    // section's own advance directly: while this is false, the section's
    // candidate advance (PendingAdvanceAtSeconds()) keeps pushing back by a
    // full loop every time it's reached, instead of ever actually firing.
    // See BeginSection's/Update()'s own Learn-case comments. In Pass mode
    // (ChartClip::learnMode), once true this never reverts for the rest of
    // the section's run. In DontFail mode, it's reversible - a miss can
    // drop this back to false (see RegisterMiss/OnPress/Update()'s own
    // miss paths), and re-earning hits_required in a row brings it back.
    bool IsPassing() const;

    // Returns the wall-clock second the current section is *currently*
    // scheduled to advance at, or a negative value if nothing is pending.
    // For a break section this is final. For a learn section not currently
    // passing, it's only provisional - every time this instant is reached
    // without IsPassing() being true, it pushes back by one more full loop
    // (see Update()'s own comment) - so a caller polling this every frame
    // will see it hold steady, then jump forward a whole loop, however many
    // times it takes. Lets the note lane derive a guaranteed-to-fire
    // deadline for a passing clip's explosion, independent of whether (or
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
    // itself the right moment to preview what comes after it. Also
    // returns nullptr the whole time the current section is a not-currently-
    // passing learn section: it doesn't yet know whether it's about to
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
    // phase origin (startSeconds). Looked up by the clip's own address in
    // m_song.clips (see m_clipInstances below), not by index - shared by
    // every section that ever references that clip: a later section
    // reusing an already-started clip continues its existing groove rather
    // than restarting it (see startSeconds), so this state fundamentally
    // belongs to the clip, not to whichever SectionInstance happens to be
    // current right now.
    struct ClipInstance
    {
        const ChartClip* chartClip = nullptr;
        bool isPlaying = false;
        double loopStartSeconds = 0.0;

        // This clip's own persistent phase reference, set once - the
        // instant this clip is first started (Learn, Break, or Background
        // alike - possibly long after other clips have already started the
        // song off), by EnsureClipInstance (called explicitly ahead of any
        // origin-dependent computation, and defensively by StartClipLoop) -
        // and never touched again after that. Used by every later
        // ChartTiming call against this clip (NextOnsetAfter,
        // ComputeClipPhaseSeconds, ComputeLearnAdvanceSeconds,
        // ComputeBreakAdvance) instead of absolute beat/second 0. This is
        // what makes a clip always start playing (audibly and visually)
        // from its own true beginning the instant it starts, whatever
        // arbitrary point in the song's overall beat grid that happens to
        // be - a different clip playing in the background is entirely
        // unaffected, and simply keeps looping on its own
        // already-established origin (see GameSession.cpp's
        // ComputeLearnAdvanceSeconds callsite comment for why this was
        // needed: two clips' first appearances genuinely don't need to
        // share a beat grid with each other). Once a given clip has been
        // started once, every later restart (a 3-miss recovery, or a later
        // section reusing the same clip) keeps using that SAME established
        // value, keeping its groove internally continuous instead of
        // restarting it from scratch each time - a stopped clip's audio
        // simply isn't sounding, it never forgets where its own pattern is.
        double startSeconds = 0.0;
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
    // are left alone once already passing, in Pass mode (frozen at their
    // passing value) - they no longer drive anything at that point. In
    // DontFail mode a hit while already passing is equally a no-op (there's
    // nothing further to earn until a miss drops it back to failing). lane
    // is only for the JudgementEvent this pushes (see ConsumeJudgementEvents) -
    // every caller already knows it, since a hit is always judged against a
    // specific lane's own note. wasPrecise is the press's own accuracy (see
    // SectionInstance::LaneHoldWasPrecise) - scores fewer points via
    // ScoreForHit when false, and is forwarded into the JudgementEvent so
    // the note lane's ripple can render yellow instead of green for it.
    void RegisterHit(int lane, bool wasPrecise);

    // Starts clipIndex's stem looping now (phase-aligned to its own
    // persistent origin - see EnsureClipInstance/ClipInstance::
    // startSeconds - at the given volume) if it isn't already playing, and
    // records the start time in that clip's own ClipInstance::
    // loopStartSeconds for loop_count to measure from. Idempotent per clip -
    // safe to call on a clip that's already playing (e.g. already running
    // as a background layer). finiteLoopCount == 0 (the default) loops
    // forever, for a clip whose eventual stop time isn't known yet (learn/
    // background). A positive value is for a clip whose total loop_count is
    // already known right now (a break section) - it's handed straight to
    // AudioEngine so the voice stops itself naturally and sample-
    // accurately, rather than relying on a later polled StopClipLoop() call
    // to catch the exact instant (which can let a fraction of a second of
    // the loop's beginning bleed through first).
    void StartClipLoop(int clipIndex, double volume, int finiteLoopCount = 0);

    // Ensures clip has a ClipInstance with a persistent pattern origin - the
    // wall-clock second its own audio and (for a Learn clip) judged-note
    // timeline are measured relative to - creating the entry and
    // establishing startSeconds to nowSeconds the first time this is ever
    // called for that clip, and leaving it untouched on every later call.
    // Called explicitly, ahead of any origin-dependent computation, by
    // BeginSection's Learn and Break cases (each needs the origin before
    // StartClipLoop itself would otherwise establish it); StartClipLoop
    // also calls it as a safety net, covering Background and any other path
    // that starts a clip without going through Learn/Break. Returns true
    // only when this call just created the entry - the caller needs to
    // know, since a fresh origin calls for ChartTiming::FreshOnsetForAllLanes
    // instead of plain NextOnsetAfter.
    bool EnsureClipInstance(const ChartClip* clip, double nowSeconds);

    // Stops clipIndex's stem if it's playing.
    void StopClipLoop(int clipIndex);

    // Records a miss: resets the shared streak, and stops the current
    // section's clip loop after 3 in a row (unchanged in both modes). In
    // Pass mode, a full no-op once already passing - further misses
    // shouldn't stop the clip or unfreeze the streak display once it's
    // proven itself, and (unlike every other case) this call doesn't even
    // push a JudgementEvent: the note lane already flashes its own
    // synthetic "hit" for these notes instead (see NoteLaneModel::
    // BuildScene's passLineHitLanes), so a real Miss event here would just
    // fight it for the same lane's flash. In DontFail mode, a miss while
    // passing instead drops the section back to failing (see
    // SectionInstance::RegisterMiss) and reverts the clip's volume to
    // init_volume. Before passing, a miss's only effect on the section's
    // own advance timing is indirect: resetting the streak makes passing
    // (and therefore advancing) take longer to reach, possibly costing the
    // clip another full loop's repeat - see Update()'s own comment. lane is
    // only for the JudgementEvent this pushes (see ConsumeJudgementEvents) -
    // every caller already knows it, since a miss is always judged against
    // a specific lane's own note.
    void RegisterMiss(int lane);

    // Moves this lane's next-expected-note pointer forward to the next note after it.
    void AdvanceExpectedNote(int lane);

    // Returns the start-tolerance window (seconds) to judge a press
    // against clip with, given clip's current playback state. In
    // normal mode this is just the chart-declared value; in easy mode it's
    // widened by kEasyModeToleranceMultiplier unconditionally, and by a
    // further kEasyModeStoppedToleranceMultiplier on top of that while the
    // clip isn't currently playing - either because it hasn't started yet
    // or because RegisterMiss stopped it after too many misses - to help
    // the player get back on track.
    double EffectiveStartToleranceSeconds(const ChartClip& clip) const;

    // Returns how long the count-in lasts, in seconds: one full bar at the
    // song's own tempo/time signature, so it's always musically aligned
    // instead of an arbitrary wall-clock duration.
    double CountInSeconds() const;

    // NextOnsetAfter/FreshOnsetForAllLanes moved to ChartTiming.h (shared
    // with the editor's analytical block scheduler) - see that header for
    // their doc comments.

    // Returns the lane note whose phase-within-span matches
    // absoluteStartBeat's phase (measured relative to originBeat - this
    // clip's own persistent phase reference, see ClipInstance::
    // startSeconds - not absolute beat 0), or nullptr if none does
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
    // own ClipInstance (needed for the startSeconds-established check)
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
    // RHYTHM_DEBUG_JUDGEMENTS tracing. precise is only meaningful when
    // result == JudgementResult::Hit - see SectionInstance::OnsetPrecise.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise = true);

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<StemHandle> m_stemHandles; // one full-loop stem per clip, indexed by clip index

    // A background clip queued by a `background` section, to actually
    // start the moment the *next* BeginSection() call happens. clipIndex
    // -1 means nothing queued. Kept as an index (not a pointer) since it's
    // realized directly via StartClipLoop(int clipIndex, ...), which itself
    // needs an index for m_stemHandles - not a lookup used to recover an
    // object identity, just carried from queueing site to realize site.
    struct QueuedBackground
    {
        int clipIndex = -1;
        int loopCount = 0;
    };

    SongClock m_clock;
    GamePhase m_phase = GamePhase::Idle;

    // See Pause()/Resume()/IsPaused().
    bool m_paused = false;

    // Set once from LoadChart's easyMode argument and left alone for the
    // rest of this chart's lifetime - see ApplyEasyModeTransform,
    // OnPress/OnRelease's judging differences, and RegisterMiss's grace check.
    bool m_easyMode = false;

    // The live judging state of whichever section is current right now
    // (SectionIndex() == -1 if none is) - replaced wholesale by BeginSection
    // every time a new section begins. See SectionInstance's own comment.
    SectionInstance m_currentInstance;

    // Per-clip playback voices, keyed by each clip's own address in
    // m_song.clips (stable for the lifetime of a loaded chart - see
    // LoadChart) - not by index. Entries are created lazily, the first time
    // a clip is ever started (see EnsureClipInstance), and never erased
    // except by a fresh LoadChart/Start(). See ClipInstance's own comment
    // for why this lives here (per clip), not on SectionInstance (per
    // section).
    std::unordered_map<const ChartClip*, ClipInstance> m_clipInstances;

    QueuedBackground m_queuedBackground;
    JudgementResult m_lastJudgement = JudgementResult::None;

    // See CurrentScore(). m_comboCount is the running count of consecutive
    // hits with no intervening real miss (a miss that doesn't even produce a
    // JudgementEvent - see RegisterMiss's alreadyPassingInPassMode case -
    // leaves it untouched, since the player was never shown a miss for it
    // either); RegisterHit scores each hit off of it, then increments it.
    //
    // m_sessionScore/m_bankedScore split the running total in two:
    // m_sessionScore is what the *current* section alone has built up since
    // it began, entirely at risk - a real miss (the same one that zeroes
    // m_comboCount) wipes it back to 0, same as the combo. m_bankedScore is
    // the sum of every section that's actually finished (a Learn section
    // locking in and reaching its own advance, or a Break section
    // completing its wait - see Update()'s own banking comment) - once
    // there, it's permanent for the rest of the song, immune to any later
    // section's miss. CurrentScore() is simply their sum.
    int m_bankedScore = 0;
    int m_sessionScore = 0;
    int m_comboCount = 0;
    // See ConsumeJudgementEvents - populated by RegisterHit/RegisterMiss,
    // drained (and cleared) there.
    std::vector<JudgementEvent> m_judgementEvents;
    // See ConsumeScoreEvents - populated by Update() (banking) and
    // RegisterMiss (wiping), drained (and cleared) there.
    std::vector<ScoreEvent> m_scoreEvents;
};
