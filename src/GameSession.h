#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "AudioEngine.h"
#include "ChartSong.h"
#include "LaneConfig.h"
#include "SectionInstance.h"
#include "SongClock.h"
#include "StreakTracker.h"

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
//     clip at a time" flow always has. Each of the c_LaneCount lanes
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
//     see m_arrangementOriginSeconds. The section's first candidate
//     advance is computed that same instant (loop_count full loops,
//     floored the same way a break section's own wait is, via
//     ChartClip::ComputeLearnAdvanceSeconds). Hitting hits_required worth
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
//     c_NoteFallBeats of real time from when the section began, same as a
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

    // Called by the caller instead of showing its own "nothing to press
    // here" feedback when IsLaneJudgeable(lane) is false - covers the case
    // where a press is too early to judge only because the section that
    // owns its note hasn't begun yet, not because there's genuinely nothing
    // there. A note's start-tolerance window is normally symmetric (see
    // OnPress), but the very first note of any section shares its onset
    // second with the section's own start instant (see
    // ChartClip::NextOnsetAfter) - before that instant, the
    // section isn't current yet, so IsLaneJudgeable rejects the press
    // outright regardless of tolerance, silently discarding the early half
    // of that one note's window that every other note in the chart gets to
    // use. This is the fix: if PreviewClip() already knows the very next
    // section (the one about to begin) is Learn and this press falls within
    // that section's own first onset's start tolerance for this lane, it's
    // remembered (see m_bufferedPress) and judged the instant BeginSection
    // actually establishes that onset - exactly as if IsLaneJudgeable had
    // already been true - rather than starting the actual section (and its
    // clip's audio) early just to accommodate it. A release landing before
    // that happens is remembered too (see OnRelease), so a fast tap-and-
    // release just ahead of the transition resolves the same way a normal
    // one would instead of leaving a hold stranded with no press behind it.
    // Returns true if this press was recognized and buffered this way (the
    // caller should suppress its own miss feedback); false if there's
    // nothing legitimate to buffer against (too early even for that, or no
    // upcoming Learn section at all) - the caller's usual "no note here"
    // feedback still applies then.
    bool TryBufferEarlyPress(int lane);

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

    // Returns the permanent running total for the song currently loaded/
    // playing: the sum of every section's own bank payout (bank amount at
    // the instant it finished, times whatever the streak multiplier was
    // then - see Update()'s own banking comment). Does NOT include
    // CurrentBank()'s own not-yet-paid-out amount - the two are shown as
    // separate HUD values (see ConsumeHudChangeEvents), not summed. Reset to
    // 0 by every Start(); persists across a Pause()/Resume() and past
    // GamePhase::Complete (the caller reads it once the session reaches
    // Complete, before the next Start() zeroes it again).
    int CurrentScore() const;

    // Returns the not-yet-paid-out bank alone (m_bank): every precise/
    // imprecise hit's points accumulate here (unlike the old per-section
    // pending pile this replaces, an isolated miss does NOT wipe this) until
    // either a section finishes (paid into CurrentScore(), multiplied by
    // CurrentMultiplier() - see Update()'s own banking comment) or the
    // shared streak trips (wiped to 0 outright - see RegisterMiss). 0
    // immediately after either of those.
    int CurrentBank() const;

    // Returns the multiplier CurrentBank() will be paid out at if a section
    // finished right now - see MultiplierForStreak. Purely a function of
    // ScoringStreak(), exposed separately since the HUD shows it as its own
    // value (e.g. "x2").
    int CurrentMultiplier() const;

    // Returns the scoring streak (m_streakTracker.Streak()) that
    // CurrentMultiplier() is computed from - NOT the same thing as
    // CurrentStreak() (the current section's own hitsRequired lock-in
    // progress, which resets every section and freezes once passing). This
    // one survives section boundaries that don't pay out anything (a
    // Reset/Background section, or a Break with an empty bank), but resets
    // to 0 both on the shared 3-miss trip AND every time a section's bank
    // actually pays out (see Update()'s own banking comment) - see
    // StreakTracker's own comment for exactly when this does and doesn't
    // reset.
    int ScoringStreak() const;

    // Returns the beat of the next note this lane is awaiting a press for.
    double NextExpectedBeatForLane(int lane) const;

    // Returns the current arrangement's shared origin, in beats (see
    // m_arrangementOriginSeconds) - the note lane needs this to tile
    // CurrentClip()'s pattern into absolute beat-space (NotesInRange)
    // exactly the way it's actually judged, since a clip's cycle boundaries
    // are measured from here, not absolute beat 0. 0 if there's no current
    // clip (harmless default, never read in that case).
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
        // one, and scored for less (see GameSession::RegisterHit). The note
        // lane's ripple renders yellow instead of
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

    // Which of the three HUD-visible values (CurrentScore()/CurrentBank()/
    // CurrentMultiplier()) just changed - pushed once per actual change (see
    // RegisterHit/RegisterMiss/Update()'s own banking comment), for the HUD
    // to grow that value's text 2x for about a second.
    enum class HudField
    {
        Total,
        Bank,
        Multiplier,
    };

    // field just changed to newValue (CurrentScore()/CurrentBank()/
    // CurrentMultiplier(), matching field) - newValue lets a renderer tell a
    // Bank field's ordinary per-hit increase apart from the moment it drops
    // to exactly 0 (a streak trip wiping it - see RegisterMiss), without
    // needing to track the previous value itself.
    struct HudChangeEvent
    {
        HudField field = HudField::Total;
        int newValue = 0;
    };

    // Returns and clears every HudChangeEvent recorded since the last call -
    // same drain-and-clear contract as ConsumeJudgementEvents, for the same
    // reason (Update()'s own banking can fire independently of any OnPress/
    // OnRelease call, so nothing here should be allowed to drop one between
    // calls). Intended reader: the note lane's HUD grow-pulse animation.
    std::vector<HudChangeEvent> ConsumeHudChangeEvents();

    // A one-shot sound cue to play - MultiplierUp when CurrentMultiplier()
    // just increased (RegisterHit), StreakBroken when the shared streak just
    // tripped and wiped a non-empty bank to 0 (RegisterMiss). Never pushed
    // for a multiplier decrease or an empty-bank trip - see RegisterMiss's
    // own comment.
    enum class SfxCue
    {
        MultiplierUp,
        StreakBroken,
    };

    // Returns and clears every SfxCue recorded since the last call - same
    // drain-and-clear contract as ConsumeHudChangeEvents. Intended reader:
    // MainWindow's one-shot SFX playback (GameSession itself knows nothing
    // about AudioEngine's SFX API - see AudioEngine::PlaySfx).
    std::vector<SfxCue> ConsumeSfxEvents();

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

    // Returns the arrangement origin PreviewClip() will actually join under
    // (see m_arrangementOriginSeconds), in beats - the current one if it's
    // still valid by the time PreviewClip() goes live, or PreviewClip()'s
    // own transition beat itself if a Reset intervenes first (a fresh
    // arrangement trivially originates at its own first clip). Lets the note
    // lane tile PreviewClip()'s pattern into the same absolute-beat-space its
    // onsets are computed in (NotesInRange). Returns a negative value if
    // there's nothing to preview right now.
    double PreviewClipOriginBeat() const;

private:
    // One clip's playback voice: is it currently playing, and where (in
    // wall-clock seconds) its current loop started - the latter only for
    // loop_count's own floor math (ComputeLearnAdvanceSeconds/
    // ComputeBreakAdvance), not phase (see m_arrangementOriginSeconds for
    // that). Looked up by the clip's own address in m_song.clips (see
    // m_clipInstances below), not by index - shared by every section that
    // ever references that clip, since two sections reusing the same clip
    // must never disagree about whether it's playing.
    struct ClipInstance
    {
        bool isPlaying = false;
        double loopStartSeconds = 0.0;
    };

    // A press (and, if it happened, its matching release) that arrived
    // before the section owning its note had begun - see
    // TryBufferEarlyPress. One per lane; a later buffered press for the
    // same lane simply overwrites an earlier, unconsumed one.
    struct BufferedPress
    {
        bool active = false;
        double pressSeconds = 0.0;
        bool released = false;
        double releaseSeconds = 0.0;
    };

    // Judges a press already confirmed to be within a lane's current
    // expected note's start tolerance - shared by OnPress's own live press
    // and by ConsumeBufferedPresses replaying an early one at the instant
    // its section actually begins. pressSeconds is the instant the press
    // itself happened (the live clock for OnPress; the buffered timestamp
    // for a replayed one) - used only to grade precision/scoring, since the
    // tolerance check itself already happened before this is called.
    void ApplyInTolerancePress(int lane, const ChartSection& section, const ChartClip& clip, double startBeat,
                                double pressSeconds);

    // Replays every lane's buffered early press (see TryBufferEarlyPress)
    // against clip's just-established onsets, now that section has actually
    // begun - re-validates each against the note's start tolerance rather
    // than trusting it was still valid whenever it was buffered (a DontFail
    // section dropping back to failing, or a not-yet-passing Learn section
    // repeating another loop, can both push the real onset much later than
    // it looked at buffering time - see PendingAdvanceAtSeconds' own
    // comment). Clears every lane's buffered press unconditionally
    // afterward, used or not, so none of it can leak into a later section.
    // Called from BeginSection's own Learn case, after
    // SchedulePendingAdvance (a buffered press completing easy mode's
    // hits_required needs one already scheduled - see RegisterHit's own
    // extension logic).
    void ConsumeBufferedPresses(const ChartSection& section, const ChartClip& clip);

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

    // Records a hit: pays c_PrecisePoints or c_ImprecisePoints (per wasPrecise)
    // into m_bank, registers with m_streakTracker (see StreakTracker's own
    // comment - this is what the section's own hitsRequired lock-in
    // progress no longer solely drives), and starts the current section's
    // clip loop (phase-aligned, at init_volume) if it isn't already playing.
    // This section's own hitsRequired progress is left alone once already
    // passing, in Pass mode (frozen at its passing value) - it no longer
    // drives anything at that point (but see IsLaneJudgeable/Update() for
    // how the section keeps scoring anyway). In DontFail mode a hit while
    // already passing is equally a no-op for that progress (there's nothing
    // further to earn until a miss drops it back to failing) - m_bank still
    // gets paid either way, every judged note is worth something. lane is
    // only for the JudgementEvent this pushes (see ConsumeJudgementEvents) -
    // every caller already knows it, since a hit is always judged against a
    // specific lane's own note. wasPrecise is the press's own accuracy (see
    // SectionInstance::LaneHoldWasPrecise) - scores fewer points when false,
    // and is forwarded into the JudgementEvent so the note lane's ripple can
    // render yellow instead of green for it. Pushes HudField::Bank always,
    // and HudField::Multiplier/SfxCue::MultiplierUp when this hit's streak
    // increment just crossed into a higher multiplier tier.
    void RegisterHit(int lane, bool wasPrecise);

    // Starts clipIndex's stem looping now (phase-aligned to the current
    // arrangement's shared origin - see m_arrangementOriginSeconds - at the
    // given volume) if it isn't already playing, and records the start time
    // in that clip's own ClipInstance::loopStartSeconds for loop_count to
    // measure from. Idempotent per clip - safe to call on a clip that's
    // already playing (e.g. already running as a background layer).
    // finiteLoopCount == 0 (the default) loops forever, for a clip whose
    // eventual stop time isn't known yet (learn/background). A positive
    // value is for a clip whose total loop_count is already known right now
    // (a break section) - it's handed straight to AudioEngine so the voice
    // stops itself naturally and sample-accurately, rather than relying on a
    // later polled StopClipLoop() call to catch the exact instant (which can
    // let a fraction of a second of the loop's beginning bleed through
    // first).
    void StartClipLoop(int clipIndex, double volume, int finiteLoopCount = 0);

    // Establishes the arrangement's shared origin (see
    // m_arrangementOriginSeconds) at nowSeconds if nothing is currently
    // playing, or returns the existing one unchanged if something already
    // is - either way, returns the value to use. Called explicitly, ahead of
    // any origin-dependent computation, by BeginSection's Learn and Break
    // cases; StartClipLoop also calls it as a safety net, covering
    // Background and any other path that starts a clip without going
    // through Learn/Break.
    double EnsureArrangementOrigin(double nowSeconds);

    // Stops clipIndex's stem if it's playing.
    void StopClipLoop(int clipIndex);

    // Records a miss: registers with m_streakTracker (see its own comment -
    // an isolated miss no longer wipes anything by itself) and stops the
    // current section's clip loop once it trips (3 in a row, unchanged in
    // both modes). In Pass mode, a full no-op once already passing - once
    // locked in, IsLaneJudgeable already keeps real presses from ever
    // reaching here again (see its own comment - Update()'s auto-accrual is
    // the section's sole scorer from that instant on), and (unlike every
    // other case) this call doesn't even push a JudgementEvent. In DontFail
    // mode, a miss while passing instead drops the section back to failing
    // (see SectionInstance::RegisterMiss) and reverts the clip's volume to
    // init_volume. Before passing, a miss's only effect on the section's own
    // advance timing is indirect: resetting this section's own hitsRequired
    // progress makes passing (and therefore advancing) take longer to reach,
    // possibly costing the clip another full loop's repeat - see Update()'s
    // own comment. lane is only for the JudgementEvent this pushes (see
    // ConsumeJudgementEvents) - every caller already knows it, since a miss
    // is always judged against a specific lane's own note. When
    // m_streakTracker trips, m_bank is wiped to 0 (HudField::Bank pushed,
    // plus SfxCue::StreakBroken if it wasn't already empty) - see
    // StreakTracker's own comment for why an isolated miss doesn't do this.
    void RegisterMiss(int lane);

    // Moves this lane's next-expected-note pointer forward to the next note after it.
    void AdvanceExpectedNote(int lane);

    // Returns the start-tolerance window (seconds) to judge a press
    // against clip with, given clip's current playback state. In
    // normal mode this is just the chart-declared value; in easy mode it's
    // widened by c_EasyModeToleranceMultiplier unconditionally, and by a
    // further c_EasyModeStoppedToleranceMultiplier on top of that while the
    // clip isn't currently playing - either because it hasn't started yet
    // or because RegisterMiss stopped it after too many misses - to help
    // the player get back on track.
    double EffectiveStartToleranceSeconds(const ChartClip& clip) const;

    // Returns how long the count-in lasts, in seconds: one full bar at the
    // song's own tempo/time signature, so it's always musically aligned
    // instead of an arbitrary wall-clock duration.
    double CountInSeconds() const;

    // NextOnsetAfter lives on ChartClip itself (shared with the editor's
    // analytical block scheduler) - see its own doc comment there.

    // Returns the lane note whose phase-within-span matches
    // absoluteStartBeat's phase (measured relative to originBeat - the
    // arrangement origin, see m_arrangementOriginSeconds - not absolute beat
    // 0), or nullptr if none does (shouldn't happen for a beat that came
    // from NextOnsetAfter against the same clip/lane/origin) - used to look
    // up a note's duration once its press has been judged correct.
    const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat) const;

    // Returns the wall-clock seconds at which PreviewClip() will actually
    // go live, or a negative value if there's nothing to preview.
    double PreviewTransitionSeconds() const;

    // Returns the section index PreviewClip()/PreviewFirstOnsetBeatForLane()
    // are currently previewing, or -1 if nothing is being previewed right
    // now - the shared logic both of those (and PreviewClipOriginBeat())
    // build on top of, factored out so it's only walked once.
    int PreviewSectionIndex() const;

    // True if a [reset] section falls anywhere between the current section
    // and sectionIndex (exclusive of both) - i.e. whatever's playing right
    // now will have been silenced by the time sectionIndex's own clip
    // actually joins, so PreviewClipOriginBeat() can't predict its origin as
    // "the current arrangement's," even if that arrangement is valid right
    // now. Used only by PreviewClipOriginBeat().
    bool ArrangementResetsBeforeSection(int sectionIndex) const;

    // Returns the index of the first section at or after startIndex whose
    // kind is neither Background nor Reset (neither ever persists as
    // "current" - BeginSection always recurses straight through both
    // within the same call - so neither is ever a real waiting/preview
    // point), or -1 if none remain. Used by PreviewClip() to find the next
    // real stopping point; whether that section is itself Learn is checked
    // separately by the caller, since an intervening Break section is a
    // real stopping point too, just not a previewable one.
    int NextPersistentSectionAtOrAfter(int startIndex) const;

    // ExpandLaneNotesToFillClip lives on ChartClip itself (shared with the
    // editor's analytical block scheduler).

    // When easy mode is on, simplifies clip's MIDI-derived pattern before
    // it's tiled/judged, in three stages, run in order (see
    // CircularGreedyThinIndices and its callers in the .cpp for the actual
    // algorithm):
    //   1. Per-lane density thinning: within each lane independently,
    //      greedily drop notes so no two *kept* onsets in that lane start
    //      closer than c_EasyModePerLaneMinGapMs apart (converted to beats
    //      via bpm) - circular, respecting the loop boundary at spanBeats.
    //      A lane that's already sparse enough is left completely
    //      untouched; a note's startBeat is never moved, only whether it
    //      survives.
    //   2. Cross-lane thinning: the stage-1 survivors, merged across all 4
    //      lanes into one time-ordered stream, are thinned again by the
    //      same rule using a tighter c_EasyModeGlobalMinGapMs, so the player
    //      is never asked to react to two different lanes closer together
    //      than that (catches a fast lane-alternating pattern, e.g. a
    //      cycling arpeggio, that looks sparse lane-by-lane but isn't as a
    //      combined stream).
    //   3. Chord collapse: any notes still within c_EasyModeChordEpsilonBeats
    //      of each other after (1)-(2) collapse to the lowest lane index
    //      (lanes are pitch-ordered ascending, so this keeps a chord's
    //      root/bass note) - no simultaneous notes survive.
    // Every surviving note then keeps its own authored durationBeats,
    // clamped to a floor (c_EasyModeNoteDurationFloorMs) and a ceiling of
    // "don't run into the next surviving note in the same lane" (computed
    // circularly, wrapping to that lane's first note one spanBeats later) -
    // a genuinely sustained note still reads and plays as a hold instead of
    // being flattened to a uniform blip. No grid-snapping anywhere: a
    // surviving note's startBeat is never altered, only which notes survive
    // and how long they last. No-op unless clip.hasMidi. Must run before
    // ExpandLaneNotesToFillClip, while spanBeats still means "one
    // repetition's length" - tiling afterward just repeats whatever this
    // produces, so it never needs to know easy mode exists.
    static void ApplyEasyModeTransform(ChartClip& clip, double bpm);

    // ComputeLoopFloorSeconds lives on ChartClip itself (shared with the
    // editor's analytical block scheduler).

    // Records a judgement for a specific lane note, for OnsetJudgement() to
    // look up later - forwards to m_currentInstance, adding
    // RHYTHM_DEBUG_JUDGEMENTS tracing. precise is only meaningful when
    // result == JudgementResult::Hit - see SectionInstance::OnsetPrecise.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise = true);

    // The multiplier a bank payout earns for a given whole-song scoring
    // streak - x1 for 0-9, x2 for 10-19, x3 for 20-29, x4 for 30+. Shared by
    // CurrentMultiplier() and every scoring path in the .cpp, so there's one
    // place that knows the tier boundaries.
    static int MultiplierForStreak(int streak);

    // Appends to m_hudChangeEvents/m_sfxEvents respectively - tiny helpers so
    // RegisterHit/RegisterMiss/Update()'s banking read as what they're doing
    // rather than repeating push_back at every call site.
    void PushHudChanged(HudField field, int newValue);
    void PushSfx(SfxCue cue);

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
    // LoadChart) - not by index. Entries are created lazily (via
    // operator[]), the first time a clip is ever started, and never erased
    // except by a fresh LoadChart/Start(). See ClipInstance's own comment
    // for why this lives here (per clip), not on SectionInstance (per
    // section).
    std::unordered_map<const ChartClip*, ClipInstance> m_clipInstances;

    // The current arrangement's shared phase reference - the wall-clock
    // second the first clip of this unbroken run of continuously-sounding
    // clips began at (see EnsureArrangementOrigin). Every ChartClip timing
    // method (NextOnsetAfter, ComputeClipPhaseSeconds, ComputeLearnAdvanceSeconds,
    // ComputeBreakAdvance) uses this, never absolute beat/second 0 - see
    // ChartClip's own class comment for why every clip that ever
    // sounds together with another lands on one of its own bar boundaries
    // from here, chart-authoring assumptions ChartSong::Load/GameSession::
    // LoadChart validate up front (ChartClip::ValidateArrangementAlignment)
    // so this can never need to special-case an unaligned join at runtime.
    // m_arrangementOriginValid is false whenever nothing is currently
    // playing - Start()/Stop()/a Reset section/a Break section that
    // silences a clip that wasn't already playing all clear it, so the next
    // clip to start becomes a fresh arrangement's own origin instead of
    // reusing a stale one from before everything went silent.
    bool m_arrangementOriginValid = false;
    double m_arrangementOriginSeconds = 0.0;

    // See TryBufferEarlyPress/ConsumeBufferedPresses/BufferedPress.
    BufferedPress m_bufferedPress[c_LaneCount];

    QueuedBackground m_queuedBackground;
    JudgementResult m_lastJudgement = JudgementResult::None;

    // See CurrentScore()/CurrentBank()/ScoringStreak(). m_streakTracker is
    // the single shared streak/consecutive-miss tracker (see its own class
    // comment) - owned here, not on m_currentInstance, and deliberately
    // never reset by BeginSection itself; it survives a Reset/Background
    // section, or a Break/Learn section whose own bank is empty when it
    // finishes. It IS reset by Start(), by the shared 3-miss trip, and by
    // Update()'s own banking code every time a section's bank actually pays
    // out (see its own comment) - in practice that covers essentially every
    // Learn section's own advance, since reaching one at all requires having
    // just scored something. m_bank is every judged hit's points since the
    // last payout or wipe (see RegisterHit/RegisterMiss); m_score is the
    // permanent sum of every section's own payout (bank amount times the
    // multiplier in effect at that instant), immune to anything after it
    // happened. CurrentScore()/CurrentBank() return these two separately,
    // not summed - the HUD shows them as distinct values.
    int m_score = 0;
    int m_bank = 0;
    StreakTracker m_streakTracker;

    // Per-lane cursor for Update()'s Pass-mode post-lock-in auto-accrual -
    // the last beat already auto-scored for that lane, so each Update() call
    // only walks the notes newly crossed since the previous one. Seeded to
    // the lock-in instant by RegisterHit the moment a section starts
    // passing (Pass mode only - see IsLaneJudgeable/Update()'s own comment).
    double m_autoScoreCursorBeat[c_LaneCount] = {};

    // See ConsumeJudgementEvents - populated by RegisterHit/RegisterMiss,
    // drained (and cleared) there.
    std::vector<JudgementEvent> m_judgementEvents;
    // See ConsumeHudChangeEvents/ConsumeSfxEvents - populated by
    // RegisterHit/RegisterMiss/Update() (banking), drained (and cleared)
    // there.
    std::vector<HudChangeEvent> m_hudChangeEvents;
    std::vector<SfxCue> m_sfxEvents;
};
