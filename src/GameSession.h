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
    Learning, // A section is active - covers learn/break/reset/background alike.
    Complete,
};

// Drives the chart's ordered list of sections, one at a time.
//   Learn      - judges presses against the clip's pattern; starts playing immediately and
//                advances once passing and its loop_count floor is met.
//   Break      - stops every other clip, plays this one for loop_count loops, no judging.
//   Reset      - stops every clip and advances immediately; no clip of its own.
//   Background - queues a clip to start (without stopping anything) when the next section begins.
//
// Live per-section judging state lives in SectionInstance, replaced wholesale each time a
// section begins. A clip's own playback voice is tracked separately, per clip, in ClipInstance.
//
// UI-agnostic - knows nothing about HWNDs or input devices.
class GameSession
{
public:
    explicit GameSession(AudioEngine& audioEngine);

    // Parses a chart and loads its clips' audio into the audio engine.
    //   chartFilePath - path to the .chart file to load.
    //   easyMode      - simplifies every clip's pattern and eases judging tolerances when true.
    //   outError      - filled with one message per problem found if loading fails.
    // Returns false if the chart is invalid or a stem fails to load.
    bool LoadChart(const std::wstring& chartFilePath, bool easyMode, std::wstring& outError);

    // Starts gameplay from the beginning of the loaded chart.
    void Start();

    // Stops all playback and returns to Idle.
    void Stop();

    // Freezes the judging clock and all playing audio until Resume().
    // No-op if already paused. Does not change Phase() or any judging state.
    void Pause();

    // Resumes playback and judging after Pause(), as if no time had passed.
    // No-op if not currently paused.
    void Resume();

    bool IsPaused() const;

    // Registers a key-down for lane and judges it against that lane's next expected note.
    //   lane - which lane was pressed.
    // No-op while paused, or outside a Learn section.
    void OnPress(int lane);

    // True if a press on lane right now would actually be judged by OnPress.
    //   lane - which lane to check.
    // Returns false outside live Learn judging, or if the lane has no notes.
    // Ignores pause state on purpose - callers should gate on IsPaused() themselves.
    // Call CatchUpCountIn() first if a real press just happened.
    bool IsLaneJudgeable(int lane) const;

    // Remembers a press that arrived just before its section's own start, to be judged once that section begins.
    //   lane - which lane was pressed.
    // Returns true if the press was buffered (caller should suppress its own "no note" feedback); false otherwise.
    // Only buffers a press within the upcoming Learn section's first-onset tolerance for this lane.
    bool TryBufferEarlyPress(int lane);

    // Begins the first section immediately if the count-in has already elapsed, without waiting for the next Update().
    // Safe to call unconditionally; no-op once the count-in has already ended.
    void CatchUpCountIn();

    // Registers a key-up for lane and judges it against the note that lane was holding, if any.
    //   lane - which lane was released.
    // Not gated by phase, play mode, or pause.
    void OnRelease(int lane);

    // Advances count-in, miss-detection, hold-timeout, and section-advance timing. Call once per frame.
    // No-op while paused.
    void Update();

    GamePhase Phase() const;
    const ChartSong& Song() const;
    int CurrentSectionIndex() const;

    // Returns the current section's clip.
    // Returns nullptr if there's no current section, or it's a [reset] section.
    const ChartClip* CurrentClip() const;

    // Returns the current section's kind, or SectionKind::Learn as a harmless default if there is none.
    SectionKind CurrentSectionKind() const;

    int CurrentStreak() const;

    // Returns the permanent score total: the sum of every finished section's own bank payout.
    // Does not include CurrentBank()'s not-yet-paid-out amount. Reset by Start().
    int CurrentScore() const;

    // Returns the not-yet-paid-out point total accumulated since the last payout or streak trip.
    // Paid into CurrentScore() when a section finishes, or wiped to 0 if the streak trips.
    int CurrentBank() const;

    // Returns the multiplier CurrentBank() would be paid out at if a section finished right now.
    int CurrentMultiplier() const;

    // Returns the whole-song scoring streak that CurrentMultiplier() is computed from.
    // Distinct from CurrentStreak(), which is the current section's own lock-in progress.
    int ScoringStreak() const;

    // Returns the beat of the next note this lane is awaiting a press for.
    //   lane - which lane to check.
    double NextExpectedBeatForLane(int lane) const;

    // Returns the beat CurrentClip()'s own pattern cycles are measured from.
    // Returns 0 if there's no current clip.
    double CurrentClipOriginBeat() const;

    const SongClock& Clock() const;

    // Returns the audio engine stem handle for a clip, for debugging.
    //   clipIndex - index of the clip in Song().clips.
    StemHandle DebugStemHandle(int clipIndex) const;

    // Returns and clears the most recent judgement (Hit/Miss/None).
    JudgementResult ConsumeLastJudgement();

    // One judgement (Hit or Miss), snapshotted at the instant it happened.
    struct JudgementEvent
    {
        JudgementResult result = JudgementResult::None;
        int lane = -1;
        bool passing = false;

        // Meaningful only when result == Hit: false if the press landed outside the precise
        // half of the tolerance window (still a correct hit, but worth fewer points).
        bool precise = true;
    };

    // Returns and clears every judgement recorded since the last call, in order.
    // Unlike ConsumeLastJudgement, never drops one - Update() can judge several lanes per call.
    std::vector<JudgementEvent> ConsumeJudgementEvents();

    // Which HUD-visible value (CurrentScore()/CurrentBank()/CurrentMultiplier()) just changed.
    enum class HudField
    {
        Total,
        Bank,
        Multiplier,
    };

    // Records that field just changed to newValue.
    struct HudChangeEvent
    {
        HudField field = HudField::Total;
        int newValue = 0;
    };

    // Returns and clears every HudChangeEvent recorded since the last call, in order.
    std::vector<HudChangeEvent> ConsumeHudChangeEvents();

    // A one-shot sound cue to play.
    enum class SfxCue
    {
        MultiplierUp,  // CurrentMultiplier() just increased.
        StreakBroken,  // The scoring streak just tripped and wiped a non-empty bank.
    };

    // Returns and clears every SfxCue recorded since the last call, in order.
    std::vector<SfxCue> ConsumeSfxEvents();

    // Returns how a specific lane note was judged, for the note lane to color it once it's passed the line.
    //   startBeat - the note's own start beat, identifying it.
    //   lane      - which lane the note is in.
    // Returns JudgementResult::None if the note hasn't been judged yet, or is too old to still be tracked.
    JudgementResult OnsetJudgement(double startBeat, int lane) const;

    // Whether the same lane note's OnsetJudgement was a precise hit.
    //   startBeat - the note's own start beat, identifying it.
    //   lane      - which lane the note is in.
    // Returns true (a meaningless default) when OnsetJudgement isn't Hit for this note.
    bool OnsetPrecise(double startBeat, int lane) const;

    // True while lane's press was judged correct and its release hasn't been judged yet.
    //   lane - which lane to check.
    bool IsLaneHeld(int lane) const;

    // Returns the start beat of the note lane is currently holding.
    //   lane - which lane to check.
    // Meaningful only when IsLaneHeld(lane) is true.
    double LaneHoldStartBeat(int lane) const;

    // True while the current section has a scheduled advance pending.
    bool IsAwaitingAdvance() const;

    // True once the current Learn section's streak has met its clip's hits_required.
    // Always false for a Break section. Reversible in DontFail mode; permanent in Pass mode.
    bool IsPassing() const;

    // Returns the wall-clock second the current section is scheduled to advance at.
    // Returns a negative value if nothing is pending.
    // For a not-yet-passing Learn section, this is provisional - it pushes back a full loop each time it's reached.
    double PendingAdvanceAtSeconds() const;

    // Returns the upcoming clip to show as an early note preview.
    // Returns nullptr if there's nothing to preview right now - including while the current
    // section is a not-yet-passing Learn section, since what's next isn't known yet.
    const ChartClip* PreviewClip() const;

    // Returns the beat of the first note on lane that PreviewClip() will require once it goes live.
    //   lane - which lane to check.
    // Returns a negative value if there's nothing to preview right now.
    double PreviewFirstOnsetBeatForLane(int lane) const;

    // Returns the beat PreviewClip()'s own pattern cycles will be measured from once it goes live.
    // Returns a negative value if there's nothing to preview right now.
    double PreviewClipOriginBeat() const;

private:
    // A press (and, if it happened, its release) buffered by TryBufferEarlyPress.
    struct BufferedPress
    {
        bool active = false;
        double pressSeconds = 0.0;
        bool released = false;
        double releaseSeconds = 0.0;
    };

    // Judges a press already confirmed within its note's start tolerance.
    //   lane         - which lane was pressed.
    //   section      - the current section.
    //   clip         - section's own clip.
    //   startBeat    - the note's own start beat.
    //   pressSeconds - when the press actually happened, for grading precision.
    void ApplyInTolerancePress(int lane, const ChartSection& section, const ChartClip& clip, double startBeat,
                                double pressSeconds);

    // Replays every lane's buffered early press against section/clip's just-established onsets.
    //   section - the section that just began.
    //   clip    - section's own clip.
    // Re-validates each press against its note's tolerance rather than trusting it was still valid when buffered.
    // Clears every lane's buffered press afterward, used or not.
    void ConsumeBufferedPresses(const ChartSection& section, const ChartClip& clip);

    // Begins the section at sectionIndex, dispatching on its kind.
    //   sectionIndex  - index of the section to begin.
    //   scheduledBeat - the ideal beat this transition was scheduled for, used instead of the live clock for determinism.
    // Also starts any background clip queued by the previous section.
    void BeginSection(int sectionIndex, double scheduledBeat);

    // Records a hit: pays points into m_bank, registers with the streak tracker, and starts the clip if needed.
    //   lane       - the lane the hit was judged on.
    //   wasPrecise - whether the press was precise; scores fewer points when false.
    void RegisterHit(int lane, bool wasPrecise);

    // Starts clipIndex's stem looping, phase-aligned to the arrangement origin, if it isn't already playing.
    //   clipIndex       - index of the clip to start.
    //   volume          - playback volume.
    //   finiteLoopCount - 0 loops forever; a positive value stops the voice automatically after that many loops.
    // Idempotent - a no-op if the clip is already playing.
    void StartClipLoop(int clipIndex, double volume, int finiteLoopCount = 0);

    // m_song.OriginBeat()/OriginSeconds() (see ChartSong's own comment), converted into the beat
    // numbering scheduledBeat and every note's own expected beat already use throughout this class -
    // beats since SongClock::Start() (i.e. since the count-in began), not since ChartSong's own beat
    // 0 (the count-in's end). ChartSong knows nothing about SongClock, so this one small conversion
    // stays here rather than on ChartSong.
    double ArrangementOriginBeat() const;

    // Stops clipIndex's stem if it's playing.
    //   clipIndex - index of the clip to stop.
    void StopClipLoop(int clipIndex);

    // Records a miss: registers with the streak tracker and stops the clip loop if it trips 3 in a row.
    //   lane - the lane the miss was judged on.
    // No-op in Pass mode once already passing. In DontFail mode, drops a passing section back to failing.
    void RegisterMiss(int lane);

    // Moves lane's next-expected-note pointer forward to the next note after it.
    //   lane - which lane to advance.
    void AdvanceExpectedNote(int lane);

    // Returns the start-tolerance window, in seconds, to judge a press against clip with.
    //   clip - the clip to compute tolerance for.
    // Widened in easy mode, and further widened while clip isn't currently playing.
    double EffectiveStartToleranceSeconds(const ChartClip& clip) const;

    // Returns how long the count-in lasts, in seconds: one full bar at the song's own tempo/time signature.
    double CountInSeconds() const;

    // NextOnsetAfter lives on ChartClip itself (shared with the editor's
    // analytical block scheduler) - see its own doc comment there.

    // Returns the lane note whose phase-within-span matches absoluteStartBeat's phase.
    //   clip              - clip to search.
    //   lane              - which lane to search.
    //   originBeat        - arrangement origin, in beats.
    //   absoluteStartBeat - beat to match against.
    // Returns nullptr if no note matches.
    const LaneNote* FindLaneNote(const ChartClip& clip, int lane, double originBeat, double absoluteStartBeat) const;

    // Returns the wall-clock second PreviewClip() will actually go live.
    // Returns a negative value if there's nothing to preview.
    double PreviewTransitionSeconds() const;

    // Returns the section index currently being previewed, or -1 if nothing is being previewed right now.
    int PreviewSectionIndex() const;

    // True if a [reset] section falls between the current section and sectionIndex.
    //   sectionIndex - section to check up to, exclusive.
    bool ArrangementResetsBeforeSection(int sectionIndex) const;

    // Returns the index of the first section at or after startIndex that isn't Background or Reset.
    //   startIndex - index to start searching from.
    // Returns -1 if none remain.
    int NextPersistentSectionAtOrAfter(int startIndex) const;

    // ExpandLaneNotesToFillClip lives on ChartClip itself (shared with the
    // editor's analytical block scheduler).

    // ApplyEasyModeTransform lives on ChartClip itself.

    // ComputeLoopFloorSeconds lives on ChartClip itself (shared with the
    // editor's analytical block scheduler).

    // Records a judgement for a specific lane note, for OnsetJudgement() to look up later.
    //   startBeat - the note's own start beat.
    //   lane      - which lane the note is in.
    //   result    - the judgement to record.
    //   precise   - whether the hit was precise; meaningful only when result == Hit.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise = true);

    // Returns the multiplier a bank payout earns for a given whole-song scoring streak.
    //   streak - whole-song scoring streak.
    // x1 for 0-9, x2 for 10-19, x3 for 20-29, x4 for 30+.
    static int MultiplierForStreak(int streak);

    // Appends a HudChangeEvent for field changing to newValue.
    void PushHudChanged(HudField field, int newValue);
    // Appends an SfxCue to play.
    void PushSfx(SfxCue cue);

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<StemHandle> m_stemHandles; // one full-loop stem per clip, indexed by clip index

    // A background clip queued to start at the next BeginSection() call. clipIndex -1 means nothing queued.
    struct QueuedBackground
    {
        int clipIndex = -1;
        int loopCount = 0;
    };

    SongClock m_clock;
    GamePhase m_phase = GamePhase::Idle;

    // See Pause()/Resume()/IsPaused().
    bool m_paused = false;

    // Set once by LoadChart and left alone for the rest of this chart's lifetime.
    bool m_easyMode = false;

    // The live judging state of whichever section is current right now.
    SectionInstance m_currentInstance;

    // Per-clip playback voices, keyed by each clip's own address in m_song.clips.
    std::unordered_map<const ChartClip*, ClipInstance> m_clipInstances;

    // See TryBufferEarlyPress/ConsumeBufferedPresses/BufferedPress.
    BufferedPress m_bufferedPress[c_LaneCount];

    QueuedBackground m_queuedBackground;

    // Set when a Break's own advance fires (see Update()'s finishedSection
    // handling) and consumed at the very top of the next BeginSection call -
    // a Break implicitly ends with a Reset, re-anchoring the bar-alignment
    // origin to that instant (see ChartSong::OriginBeat()'s own comment for
    // why). Deferred to BeginSection, rather than done directly in Update(),
    // specifically so the re-anchor uses the exact same scheduledBeat-derived
    // nowSeconds the section it lands on then reads right back - computing
    // it independently in both places (Update()'s own PendingAdvanceAtSeconds()
    // vs. BeginSection's scheduledBeat parameter) can differ by a
    // rounding hair, which is enough to fail the exact-boundary check.
    bool m_arrangementResetPending = false;

    JudgementResult m_lastJudgement = JudgementResult::None;

    // See CurrentScore()/CurrentBank()/ScoringStreak().
    int m_score = 0;
    int m_bank = 0;
    StreakTracker m_streakTracker;

    // Per-lane cursor for Update()'s post-lock-in auto-scoring: the last beat already scored for that lane.
    double m_autoScoreCursorBeat[c_LaneCount] = {};

    // See ConsumeJudgementEvents.
    std::vector<JudgementEvent> m_judgementEvents;
    // See ConsumeHudChangeEvents/ConsumeSfxEvents.
    std::vector<HudChangeEvent> m_hudChangeEvents;
    std::vector<SfxCue> m_sfxEvents;
};
