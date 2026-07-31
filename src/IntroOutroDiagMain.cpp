#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "AudioEngine.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): drives GameSession
// headlessly against a chart, auto-pressing/releasing every lane's notes at
// the right moment for learn sections, and prints phase/section/play-mode
// transitions so intro_bars/outro_loops/loop_count/per-lane hold judging
// and the learn/solo/background section flow can be verified without UI
// automation - including (1) that a solo section's clip actually stops
// once its own wait completes rather than droning into subsequent
// sections, and (2) that PreviewClip() doesn't skip over an intervening
// solo section's own preview window, nor skip past one to a further
// section.

namespace
{

const wchar_t* PhaseName(GamePhase phase)
{
    switch (phase)
    {
        case GamePhase::Idle: return L"Idle";
        case GamePhase::CountIn: return L"CountIn";
        case GamePhase::Learning: return L"Learning";
        case GamePhase::Complete: return L"Complete";
    }
    return L"?";
}

const wchar_t* JudgementName(JudgementResult result)
{
    switch (result)
    {
        case JudgementResult::Hit: return L"Hit";
        case JudgementResult::Miss: return L"Miss";
        case JudgementResult::None: return L"None";
    }
    return L"?";
}

const wchar_t* PlayModeName(PlayMode mode)
{
    switch (mode)
    {
        case PlayMode::Learn: return L"learn";
        case PlayMode::Solo: return L"solo";
        case PlayMode::Background: return L"background";
    }
    return L"?";
}

// Mirrors GameSession's private FindLaneNote: looks up the note whose
// phase-within-span matches absoluteStartBeat, so this diagnostic can plan
// a release without access to GameSession's internals.
double DurationForLaneNote(const ChartClip& clip, int lane, double absoluteStartBeat)
{
    double span = clip.spanBeats;
    double phase = std::fmod(absoluteStartBeat, span);
    if (phase < 0.0)
    {
        phase += span;
    }
    for (const LaneNote& note : clip.laneNotes[lane])
    {
        if (std::abs(note.startBeat - phase) < 1e-6)
        {
            return note.durationBeats;
        }
    }
    return 0.0;
}

// Mirrors NoteLane.cpp's anonymous-namespace NotesInRange (not exported, so
// reimplemented here): true if any note on this lane, tiled across its
// clip's repeating pattern, overlaps [fromBeat, toBeat] at all.
bool AnyNoteVisible(const ChartClip& clip, int lane, double fromBeat, double toBeat)
{
    double spanBeats = clip.spanBeats;
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty() || spanBeats <= 0.0)
    {
        return false;
    }
    long long firstBar = static_cast<long long>(std::floor(fromBeat / spanBeats)) - 1;
    long long lastBar = static_cast<long long>(std::floor(toBeat / spanBeats)) + 1;
    for (long long bar = firstBar; bar <= lastBar; ++bar)
    {
        for (const LaneNote& note : notes)
        {
            double absoluteStart = bar * spanBeats + note.startBeat;
            double absoluteEnd = absoluteStart + note.durationBeats;
            if (absoluteEnd >= fromBeat && absoluteStart <= toBeat)
            {
                return true;
            }
        }
    }
    return false;
}

// NoteLane.cpp's private kBeatsBehind (how far a note stays visible past the
// judge line before scrolling off) - mirrored here since it's not exported.
constexpr double kDiagBeatsBehind = 1.0;

// Same tiling as AnyNoteVisible, but returns every overlapping note instead
// of just whether one exists - lets DIAG_NOTELANE_TRACE print NoteLane's
// exact visible set (including duration/Y-position) instead of a yes/no.
struct DiagVisibleNote
{
    double absoluteStart = 0.0;
    double durationBeats = 0.0;
};

std::vector<DiagVisibleNote> ListVisibleNotes(const ChartClip& clip, int lane, double fromBeat, double toBeat)
{
    std::vector<DiagVisibleNote> result;
    double spanBeats = clip.spanBeats;
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty() || spanBeats <= 0.0)
    {
        return result;
    }
    long long firstBar = static_cast<long long>(std::floor(fromBeat / spanBeats)) - 1;
    long long lastBar = static_cast<long long>(std::floor(toBeat / spanBeats)) + 1;
    for (long long bar = firstBar; bar <= lastBar; ++bar)
    {
        for (const LaneNote& note : notes)
        {
            double absoluteStart = bar * spanBeats + note.startBeat;
            double absoluteEnd = absoluteStart + note.durationBeats;
            if (absoluteEnd >= fromBeat && absoluteStart <= toBeat)
            {
                result.push_back({absoluteStart, note.durationBeats});
            }
        }
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring chartPath = L"Content/Cool Boy/Cool Boy.chart";
    if (argc > 1)
    {
        std::string arg = argv[1];
        chartPath.assign(arg.begin(), arg.end());
    }

    // Temporary realistic-timing test knobs: simulate a human's non-zero
    // press/release reaction offset (in milliseconds, via env vars) instead
    // of this auto-player's normal zero-latency behavior, to check whether
    // late-but-still-in-tolerance timing causes spurious misses that a
    // perfectly-on-time auto-player would never expose.
    const char* pressOffsetEnv = std::getenv("DIAG_PRESS_OFFSET_MS");
    const char* releaseOffsetEnv = std::getenv("DIAG_RELEASE_OFFSET_MS");
    double kTestPressOffsetSeconds = pressOffsetEnv ? std::atof(pressOffsetEnv) / 1000.0 : 0.0;
    double kTestReleaseOffsetSeconds = releaseOffsetEnv ? std::atof(releaseOffsetEnv) / 1000.0 : 0.0;

    // DIAG_MISS_AFTER_LOCKIN=1: once a learn section locks in (awaitingAdvance
    // first observed true), stop auto-pressing for the rest of that section's
    // wait, so Update()'s press-phase timeout generates real post-lock-in
    // misses instead of this perfect auto-player only ever producing more
    // hits - the only way to actually exercise RegisterMiss()'s new
    // once-locked-in no-op.
    bool diagMissAfterLockin = std::getenv("DIAG_MISS_AFTER_LOCKIN") != nullptr;

    // DIAG_NOTELANE_TRACE=1: reproduces NoteLane::Draw's exact lane-0 note
    // selection (isLiveJudging, dotsClip, dotsFromBeat, then the same
    // NotesInRange tiling - here ListVisibleNotes) at intervals throughout
    // the run, to check for notes appearing far outside the intended
    // [nowBeat-kBeatsBehind, nowBeat+kNoteFallBeats] window - which would
    // render outside the visible play area without ever being culled by
    // NoteLane's own visibility check (that check only looks at a note's
    // *bottom* edge, never its top, so an anomalous note could silently
    // draw off the top of the lane).
    bool diagNoteLaneTrace = std::getenv("DIAG_NOTELANE_TRACE") != nullptr;
    double lastTraceSeconds = -1.0;

    // DIAG_EASY_MODE=1: load the chart in easy mode (quantized/de-chorded
    // patterns, release timing ignored, one-note grace per section) instead
    // of normal mode.
    bool diagEasyMode = std::getenv("DIAG_EASY_MODE") != nullptr;
    printf("easyMode=%s\n", diagEasyMode ? "true" : "false");

    AudioEngine engine;
    if (!engine.Initialize())
    {
        printf("AudioEngine::Initialize failed\n");
        return 1;
    }

    // Regression check: a MIDI pattern longer than one loop of its stem's
    // audio must be rejected at load time (the notes would otherwise be
    // judged against a moment the audio has already looped past).
    {
        GameSession badSession(engine);
        std::wstring badError;
        bool badOk = badSession.LoadChart(
            L"C:\\Users\\nsaga\\AppData\\Local\\Temp\\claude\\C--Users-nsaga-OneDrive-Projects\\2d6d0060-ad5a-4bf4-86f9-077b215135dd\\scratchpad\\too_long_test.chart",
            diagEasyMode, badError);
        printf("too_long_test.chart LoadChart() returned %s (expected false)%s\n", badOk ? "true" : "false",
               badOk ? " ** MISMATCH **" : "");
        wprintf(L"  %ls\n", badError.c_str());
    }

    GameSession session(engine);
    std::wstring loadError;
    if (!session.LoadChart(chartPath, diagEasyMode, loadError))
    {
        wprintf(L"LoadChart failed: %ls\n", loadError.c_str());
        return 1;
    }

    // Opt-in (DIAG_DUMP_NOTES=1): dump every clip's post-tiling spanBeats
    // and per-lane note list right after load - useful for spot-checking
    // that tiling produced the expected pattern, without cluttering every
    // normal diagnostic run.
    if (std::getenv("DIAG_DUMP_NOTES"))
    {
        for (size_t ci = 0; ci < session.Song().clips.size(); ++ci)
        {
            const ChartClip& c = session.Song().clips[ci];
            wprintf(L"clip %zu (%ls): spanBeats=%.6f hasMidi=%d\n", ci, c.name.c_str(), c.spanBeats,
                    c.hasMidi ? 1 : 0);
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                for (const LaneNote& note : c.laneNotes[lane])
                {
                    wprintf(L"  lane %d: startBeat=%.6f durationBeats=%.6f endBeat=%.6f\n", lane, note.startBeat,
                            note.durationBeats, note.startBeat + note.durationBeats);
                }
            }
        }
    }


    session.Start();

    // DIAG_RACE_TEST=1: reproduce a real player's key-down landing between
    // two Update() ticks, right on the count-in's own boundary - sleeps on
    // real wall-clock time past CountInSeconds() *without* ever calling
    // Update(), so the phase is still nominally CountIn, then presses lane
    // 0 exactly as MainWindow::RegisterPress would. Before CatchUpCountIn(),
    // IsLaneJudgeable would reject this outright (wrong phase) regardless
    // of tolerance; after it, the press should register normally.
    if (std::getenv("DIAG_RACE_TEST"))
    {
        double secondsPerBeat = 60.0 / session.Song().bpm;
        double countInBeat = session.Song().beatsPerBar;
        double countInSeconds = countInBeat * secondsPerBeat;
        DWORD sleepMs = static_cast<DWORD>(countInSeconds * 1000.0) + 5;
        printf("[race test] phase before sleep=%ls, sleeping %lums (count-in ends at %.4fs)\n",
               PhaseName(session.Phase()), static_cast<unsigned long>(sleepMs), countInSeconds);
        Sleep(sleepMs);
        printf("[race test] phase after sleep (no Update() called)=%ls, elapsed=%.4fs\n",
               PhaseName(session.Phase()), session.Clock().ElapsedSeconds());
        session.OnPress(0);
        JudgementResult raceResult = session.ConsumeLastJudgement();
        bool held = session.IsLaneHeld(0);
        // Normal mode doesn't judge Hit until release - a correctly-timed
        // press leaves judgement=None but starts a hold (held=true). The
        // bug this guards against also leaves judgement=None, but with
        // held=false: IsLaneJudgeable rejected the press outright before
        // OnPress ever reached the tolerance check, so nothing happened at
        // all - a silent, phase-gated miss indistinguishable from success
        // by judgement alone, which is exactly why `held` matters here.
        const char* verdict = (raceResult == JudgementResult::Miss)   ? "MISTIMED (unexpected - press was on time)"
                              : held                                  ? "OK - press registered, hold started"
                                                                       : "BUG - press silently ignored (phase-gated)";
        printf("[race test] OnPress(0) -> phase=%ls judgement=%s held=%s : %s\n", PhaseName(session.Phase()),
               raceResult == JudgementResult::Hit ? "Hit" : raceResult == JudgementResult::Miss ? "Miss" : "None",
               held ? "true" : "false", verdict);
        return 0;
    }

    GamePhase lastPhase = GamePhase::Idle;
    int lastSection = -2;
    PlayMode lastPlayMode = PlayMode::Learn;
    bool lastInIntro = false;
    bool lastAwaitingAdvance = false;
    const ChartClip* lastPreview = nullptr;
    DWORD startTick = GetTickCount();

    // Bug-1 regression tracking: once we transition away from a Solo
    // section, remember its clip's stem handle and poll
    // GetPositionSeconds() on every subsequent section transition until
    // it's observed to stop advancing - a solo clip must stop once its own
    // wait completes, not drone on indefinitely into later sections.
    StemHandle watchedSoloStem;
    double watchedSoloLastPosition = -1.0;
    bool watchedSoloConfirmedStopped = true;

    // Dropout-at-transition tracking: while the CURRENT section is a solo
    // with a real clip, poll engine.IsPlaying() every tick from the moment
    // it's first observed playing - a solo clip's voice is finite (unlike
    // a locked-in learn clip), so if loop_count's requested passes finish
    // before the section's own scheduled advance, the voice self-stops and
    // leaves genuine silence for whatever's left of the wait.
    StemHandle currentSoloStem;
    bool currentSoloEverPlayed = false;
    bool currentSoloDropoutFlagged = false;
    int currentSoloSectionIndex = -1;

    // Tolerance-bug tracking: CurrentStreak() is polled every iteration
    // (not just after our own explicit press/release calls), so a drop
    // that ISN'T immediately preceded by a printed "-> Miss" from this
    // auto-player must have come from inside GameSession::Update() itself
    // (the held-past-tolerance timeout or the press-phase timeout) -
    // exactly the kind of "missed a note that looked hittable" a player
    // would notice but a press/release-only trace would hide.
    int lastStreak = 0;

    // Post-lock-in extension tracking: once a learn section's awaitingAdvance
    // first flips true, stop pressing for the rest of it (if
    // DIAG_MISS_AFTER_LOCKIN), confirm PendingAdvanceAtSeconds() never
    // changes value for the rest of the wait (proves progression timing is
    // unaffected by post-lock-in hits/misses), and confirm the locked-in
    // clip's stem position keeps advancing rather than stopping.
    bool stopPressingThisSection = false;
    double lockinPendingAdvanceAtSeconds = -1.0;
    StemHandle watchedLockinStem;
    double watchedLockinLastPosition = -1.0;
    double watchedLockinLastCheckSeconds = -1.0;

    // Mirrors NoteLane's own nextClipShowing computation (see NoteLane.cpp)
    // to confirm the lock-in explosion equivalent always fires at or before
    // the actual section transition - never silently skipped.
    bool lastMirroredNextClipShowing = false;

    // CountIn preview visibility tracking: for each lane, the first tick
    // its PreviewFirstOnsetBeatForLane() note becomes visible (within
    // kNoteFallBeats of "now"), print how much real lead time is left
    // before CountIn actually ends - directly answers whether the very
    // first section's first notes get their full on-screen travel time.
    bool countInLaneVisiblePrinted[kLaneCount] = {};

    // General blank-lane-row tracking (see the check below): negative while
    // something is visible, set to the elapsed-seconds timestamp a blank
    // stretch started.
    double blankStartSeconds = -1.0;

    // Counts real OnRelease() calls - printed at the end when diagEasyMode
    // is true, where it's expected to stay exactly 0: a correct press
    // already returns Hit directly (see OnPress), so the auto-player's
    // "arm a release" branch below never fires in easy mode, proving
    // Hit-registration has zero dependency on release.
    int releaseCallCount = 0;

    // Per-lane auto-player state: whether we're currently simulating a held
    // key, and when (in seconds) to release it.
    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    while (session.Phase() != GamePhase::Complete)
    {
        session.Update();

        GamePhase phase = session.Phase();
        int sectionIndex = session.CurrentSectionIndex();
        PlayMode playMode = session.CurrentPlayMode();
        bool inIntro = session.IsInIntro();
        bool awaitingAdvance = session.IsAwaitingAdvance();
        double secondsPerBeat = 60.0 / session.Song().bpm;

        if (diagNoteLaneTrace && session.Clock().ElapsedSeconds() - lastTraceSeconds >= 0.3)
        {
            lastTraceSeconds = session.Clock().ElapsedSeconds();
            double nowBeat = session.Clock().BeatPosition();

            // Exactly mirrors NoteLane::Draw's learnAwaitingAdvance/nextClipShowing/
            // isLiveJudging/dotsClip/dotsFromBeat chain (see NoteLane.cpp) for lane 0.
            const ChartClip* liveClip = session.CurrentClip();
            bool learnAwaitingAdvance =
                liveClip && phase == GamePhase::Learning && playMode == PlayMode::Learn && awaitingAdvance;
            bool nextClipShowing = false;
            if (learnAwaitingAdvance)
            {
                double advanceAtBeat = session.PendingAdvanceAtSeconds() / secondsPerBeat;
                const ChartClip* preview = session.PreviewClip();
                if (preview == nullptr)
                {
                    nextClipShowing = (nowBeat >= advanceAtBeat);
                }
                else
                {
                    double earliestOnset = -1.0;
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        double onset = session.PreviewFirstOnsetBeatForLane(lane);
                        if (onset >= 0.0 && (earliestOnset < 0.0 || onset < earliestOnset))
                        {
                            earliestOnset = onset;
                        }
                    }
                    double visibleAtBeat =
                        (earliestOnset >= 0.0) ? (earliestOnset - kNoteFallBeats) : (advanceAtBeat - kNoteFallBeats);
                    double triggerBeat = (visibleAtBeat <= advanceAtBeat) ? visibleAtBeat : (advanceAtBeat - kNoteFallBeats);
                    nextClipShowing = (nowBeat >= triggerBeat);
                }
            }
            bool isLiveJudging = liveClip && phase == GamePhase::Learning && playMode == PlayMode::Learn &&
                                  !nextClipShowing && !inIntro;

            const ChartClip* dotsClip = isLiveJudging ? liveClip : session.PreviewClip();
            double dotsFromBeat = 0.0;
            bool haveDotsFromBeat = true;
            if (isLiveJudging)
            {
                dotsFromBeat = nowBeat - kDiagBeatsBehind;
            }
            else if (dotsClip)
            {
                dotsFromBeat = session.PreviewFirstOnsetBeatForLane(0);
                haveDotsFromBeat = dotsFromBeat >= 0.0;
            }
            else
            {
                haveDotsFromBeat = false;
            }

            printf("[t=%.2fs] TRACE nowBeat=%.4f phase=%ls isLiveJudging=%s dotsClip=%ls dotsFromBeat=%s\n",
                   session.Clock().ElapsedSeconds(), nowBeat, PhaseName(phase), isLiveJudging ? "true" : "false",
                   dotsClip ? dotsClip->name.c_str() : L"(none)",
                   haveDotsFromBeat ? std::to_string(dotsFromBeat).c_str() : "n/a");

            if (dotsClip && haveDotsFromBeat)
            {
                double toBeat = nowBeat + kNoteFallBeats;
                for (const DiagVisibleNote& n : ListVisibleNotes(*dotsClip, 0, dotsFromBeat, toBeat))
                {
                    double beatsFromStart = n.absoluteStart - nowBeat;
                    double beatsFromEnd = beatsFromStart + n.durationBeats;
                    bool aboveTop = beatsFromStart > kNoteFallBeats + 1e-9;
                    printf("[t=%.2fs]   lane0 visible note: absoluteStart=%.4f dur=%.4f beatsFromStart=%.4f "
                           "beatsFromEnd=%.4f%s\n",
                           session.Clock().ElapsedSeconds(), n.absoluteStart, n.durationBeats, beatsFromStart,
                           beatsFromEnd, aboveTop ? "  ** ABOVE VISIBLE TOP **" : "");
                }
            }
        }

        if (phase == GamePhase::CountIn)
        {
            double nowBeatCountIn = session.Clock().BeatPosition();
            const ChartClip* previewClip = session.PreviewClip();
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (countInLaneVisiblePrinted[lane])
                {
                    continue;
                }
                double onset = session.PreviewFirstOnsetBeatForLane(lane);
                if (onset < 0.0)
                {
                    continue;
                }
                if (nowBeatCountIn >= onset - kNoteFallBeats)
                {
                    countInLaneVisiblePrinted[lane] = true;
                    double countInEndBeat = session.Song().beatsPerBar; // GameSession::CountInSeconds(), mirrored
                    double leadBeatsBeforeCountInEnds = countInEndBeat - nowBeatCountIn;
                    // Directly mirror NoteLane's own dotsFromBeat/NotesInRange
                    // call for the preview path (dotsFromBeat = onset exactly)
                    // to confirm the note GameSession says is "next" is
                    // actually findable by the same tiling logic NoteLane uses.
                    bool actuallyVisible =
                        previewClip && AnyNoteVisible(*previewClip, lane, onset, nowBeatCountIn + kNoteFallBeats);
                    printf("[t=%.2fs]   CountIn preview: lane %d onset=%.4f becomes visible now - "
                           "%.4f beats of CountIn remain, note is %.4f beats away, AnyNoteVisible=%s\n",
                           session.Clock().ElapsedSeconds(), lane, onset, leadBeatsBeforeCountInEnds,
                           onset - nowBeatCountIn, actuallyVisible ? "true" : "** FALSE - BUG **");
                }
            }
        }

        // A streak drop that coincides with a section change is just
        // BeginSection's normal per-section reset, not a miss - only flag
        // one that happens with no section/phase change this tick, which
        // can only mean a Miss was registered somewhere (RegisterMiss()
        // resets the streak unconditionally, whether called from a real
        // press/release or from one of Update()'s own timeout checks).
        int streak = session.CurrentStreak();
        bool sectionChangedThisTick = (phase != lastPhase || sectionIndex != lastSection);
        if (streak < lastStreak && !sectionChangedThisTick)
        {
            printf("[t=%.2fs]   SILENT streak drop %d -> %d (Update()-internal timeout, not an explicit press/release)\n",
                   session.Clock().ElapsedSeconds(), lastStreak, streak);
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                printf("[t=%.2fs]     lane %d: held=%s holdStartBeat=%.4f nextExpectedBeat=%.4f heldByUs=%s releaseAtSeconds=%.4f\n",
                       session.Clock().ElapsedSeconds(), lane, session.IsLaneHeld(lane) ? "true" : "false",
                       session.LaneHoldStartBeat(lane), session.NextExpectedBeatForLane(lane),
                       heldByUs[lane] ? "true" : "false", releaseAtSeconds[lane]);
            }
        }
        lastStreak = streak;

        if (phase != lastPhase || sectionIndex != lastSection)
        {
            // We're leaving lastSection right now - if it was a Solo
            // section, start watching its clip's stem position.
            if (lastPlayMode == PlayMode::Solo && lastSection >= 0)
            {
                int soloClipIndex = session.Song().sections[lastSection].clipIndex;
                if (soloClipIndex >= 0)
                {
                    watchedSoloStem = session.DebugStemHandle(soloClipIndex);
                    watchedSoloLastPosition = -1.0;
                    watchedSoloConfirmedStopped = false;
                }
            }

            stopPressingThisSection = false;
            lockinPendingAdvanceAtSeconds = -1.0;
            watchedLockinLastPosition = -1.0;
            watchedLockinLastCheckSeconds = -1.0;
            lastMirroredNextClipShowing = false;

            // Entering a NEW solo section (with a real clip): start
            // watching for a dropout for the rest of this section.
            currentSoloEverPlayed = false;
            currentSoloDropoutFlagged = false;
            currentSoloSectionIndex = -1;
            if (phase == GamePhase::Learning && playMode == PlayMode::Solo)
            {
                int soloClipIndex = session.Song().sections[sectionIndex].clipIndex;
                if (soloClipIndex >= 0)
                {
                    currentSoloStem = session.DebugStemHandle(soloClipIndex);
                    currentSoloSectionIndex = sectionIndex;
                }
            }

            const ChartClip* clip = session.CurrentClip();
            printf("[t=%.2fs] phase=%ls section=%d play_mode=%ls clip=(%ls)\n", session.Clock().ElapsedSeconds(),
                   PhaseName(phase), sectionIndex, PlayModeName(playMode), clip ? clip->name.c_str() : L"-");
            lastPhase = phase;
            lastSection = sectionIndex;
            lastPlayMode = playMode;

            if (!watchedSoloConfirmedStopped && watchedSoloStem.IsValid())
            {
                double pos = engine.GetPositionSeconds(watchedSoloStem);
                printf("[t=%.2fs]   watched solo clip stem position -> %.3fs\n", session.Clock().ElapsedSeconds(),
                       pos);
                if (watchedSoloLastPosition >= 0.0 && std::abs(pos - watchedSoloLastPosition) < 1e-6)
                {
                    watchedSoloConfirmedStopped = true;
                    printf("[t=%.2fs]   watched solo clip stem position CONFIRMED STOPPED\n",
                           session.Clock().ElapsedSeconds());
                }
                watchedSoloLastPosition = pos;
            }
        }
        else if (playMode != lastPlayMode)
        {
            printf("[t=%.2fs]   play_mode -> %ls\n", session.Clock().ElapsedSeconds(), PlayModeName(playMode));
            lastPlayMode = playMode;
        }
        if (inIntro != lastInIntro)
        {
            printf("[t=%.2fs]   IsInIntro -> %s\n", session.Clock().ElapsedSeconds(), inIntro ? "true" : "false");
            lastInIntro = inIntro;
        }
        if (awaitingAdvance != lastAwaitingAdvance)
        {
            printf("[t=%.2fs]   IsAwaitingAdvance -> %s\n", session.Clock().ElapsedSeconds(),
                   awaitingAdvance ? "true" : "false");
            lastAwaitingAdvance = awaitingAdvance;

            if (awaitingAdvance && playMode == PlayMode::Learn)
            {
                lockinPendingAdvanceAtSeconds = session.PendingAdvanceAtSeconds();
                printf("[t=%.2fs]   locked in - PendingAdvanceAtSeconds=%.4f\n", session.Clock().ElapsedSeconds(),
                       lockinPendingAdvanceAtSeconds);

                if (diagMissAfterLockin)
                {
                    stopPressingThisSection = true;
                    printf("[t=%.2fs]   DIAG_MISS_AFTER_LOCKIN: no longer pressing this section\n",
                           session.Clock().ElapsedSeconds());
                }

                int clipIndex = session.Song().sections[sectionIndex].clipIndex;
                watchedLockinStem = session.DebugStemHandle(clipIndex);
                watchedLockinLastPosition = engine.GetPositionSeconds(watchedLockinStem);
                watchedLockinLastCheckSeconds = session.Clock().ElapsedSeconds();
            }
        }

        // While locked in on a learn section: confirm the scheduled advance
        // time never moves (progression timing unaffected by post-lock-in
        // hits/misses - the OnRelease/RegisterMiss guards from GameSession
        // are what this is actually testing), and confirm the clip's stem
        // position keeps advancing rather than stopping (RegisterMiss's
        // once-locked-in no-op).
        if (awaitingAdvance && playMode == PlayMode::Learn && lockinPendingAdvanceAtSeconds >= 0.0)
        {
            double current = session.PendingAdvanceAtSeconds();
            if (std::abs(current - lockinPendingAdvanceAtSeconds) > 1e-6)
            {
                printf("[t=%.2fs]   ** MISMATCH ** PendingAdvanceAtSeconds moved %.4f -> %.4f after lock-in\n",
                       session.Clock().ElapsedSeconds(), lockinPendingAdvanceAtSeconds, current);
                lockinPendingAdvanceAtSeconds = current;
            }

            if (watchedLockinStem.IsValid() && session.Clock().ElapsedSeconds() - watchedLockinLastCheckSeconds >= 0.5)
            {
                double pos = engine.GetPositionSeconds(watchedLockinStem);
                if (std::abs(pos - watchedLockinLastPosition) < 1e-6)
                {
                    printf("[t=%.2fs]   ** MISMATCH ** locked-in clip stem position stalled at %.3fs\n",
                           session.Clock().ElapsedSeconds(), pos);
                }
                watchedLockinLastPosition = pos;
                watchedLockinLastCheckSeconds = session.Clock().ElapsedSeconds();
            }

            // Mirror NoteLane's nextClipShowing computation (NoteLane.cpp) to
            // confirm the explosion equivalent fires at or before the actual
            // transition - never silently skipped - and, now, only once
            // something is actually about to become visible (or the
            // transition-beat fallback, when nothing ever would be).
            const ChartClip* mirroredPreview = session.PreviewClip();
            double nowBeat = session.Clock().BeatPosition();
            double advanceAtBeat = session.PendingAdvanceAtSeconds() / secondsPerBeat;
            bool mirroredNextClipShowing;
            if (mirroredPreview == nullptr)
            {
                mirroredNextClipShowing = (nowBeat >= advanceAtBeat);
            }
            else
            {
                double earliestOnset = -1.0;
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    double onset = session.PreviewFirstOnsetBeatForLane(lane);
                    if (onset >= 0.0 && (earliestOnset < 0.0 || onset < earliestOnset))
                    {
                        earliestOnset = onset;
                    }
                }
                double visibleAtBeat = (earliestOnset >= 0.0) ? (earliestOnset - kNoteFallBeats) : (advanceAtBeat - kNoteFallBeats);
                double triggerBeat = (visibleAtBeat <= advanceAtBeat) ? visibleAtBeat : (advanceAtBeat - kNoteFallBeats);
                mirroredNextClipShowing = (nowBeat >= triggerBeat);
            }
            if (mirroredNextClipShowing && !lastMirroredNextClipShowing)
            {
                printf("[t=%.2fs]   mirrored explosion trigger fires (preview=%ls)\n",
                       session.Clock().ElapsedSeconds(), mirroredPreview ? mirroredPreview->name.c_str() : L"(none)");
                if (mirroredPreview)
                {
                    double nowBeatNow = session.Clock().BeatPosition();
                    double minGapBeats = 1e9;
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        double onset = session.PreviewFirstOnsetBeatForLane(lane);
                        if (onset < 0.0)
                        {
                            continue;
                        }
                        double gapBeats = (onset - nowBeatNow) - kNoteFallBeats; // <=0 means already visible
                        if (gapBeats < minGapBeats)
                        {
                            minGapBeats = gapBeats;
                        }
                        printf("[t=%.2fs]     lane %d preview onset beat=%.4f (%.4f beats until visible)\n",
                               session.Clock().ElapsedSeconds(), lane, onset, gapBeats);
                    }
                    if (minGapBeats > 1e-6)
                    {
                        printf("[t=%.2fs]   ** GAP ** no preview lane visible yet - %.4f beats of blank screen\n",
                               session.Clock().ElapsedSeconds(), minGapBeats);
                    }
                }
            }
            lastMirroredNextClipShowing = mirroredNextClipShowing;
        }

        // Dropout-at-transition check: while the current section is a solo
        // with a real clip, its stem must never stop playing before the
        // section's own scheduled advance - see currentSoloStem above.
        if (playMode == PlayMode::Solo && sectionIndex == currentSoloSectionIndex && currentSoloStem.IsValid())
        {
            bool playingNow = engine.IsPlaying(currentSoloStem);
            if (playingNow)
            {
                currentSoloEverPlayed = true;
            }
            else if (currentSoloEverPlayed && !currentSoloDropoutFlagged)
            {
                currentSoloDropoutFlagged = true;
                printf("[t=%.2fs]   ** MISMATCH ** solo clip stem stopped playing before its own scheduled "
                       "advance (dropout) - advanceAt=%.4f\n",
                       session.Clock().ElapsedSeconds(), session.PendingAdvanceAtSeconds());
            }
        }

        // Mirror NoteLane's own gating logic here to verify the preview window.
        const ChartClip* preview = session.PreviewClip();
        if (preview != lastPreview)
        {
            printf("[t=%.2fs]   PreviewClip -> %ls\n", session.Clock().ElapsedSeconds(),
                   preview ? preview->name.c_str() : L"(none)");
            lastPreview = preview;
        }

        // General "is the lane row ever fully blank" check - mirrors
        // NoteLane::Draw's own dotsClip/dotsFromBeat selection exactly
        // (isLiveJudging vs preview) and checks every lane for any visible
        // note, independent of the specific explosion-timing mechanism
        // above. Only flagged while something could plausibly be on screen
        // (CurrentPlayMode()==Learn, or there's a preview to show) - a
        // genuine solo/background stretch with nothing queued is
        // legitimately blank and not interesting here.
        {
            bool couldShowSomething =
                phase == GamePhase::Learning && !inIntro && (playMode == PlayMode::Learn || preview != nullptr);
            bool anyVisible = false;
            if (couldShowSomething)
            {
                double nowBeatCheck = session.Clock().BeatPosition();
                bool learnAwaitingAdvanceCheck = playMode == PlayMode::Learn && awaitingAdvance;
                bool nextClipShowingCheck = false;
                if (learnAwaitingAdvanceCheck)
                {
                    double advanceAtBeat = session.PendingAdvanceAtSeconds() / secondsPerBeat;
                    if (preview == nullptr)
                    {
                        nextClipShowingCheck = (nowBeatCheck >= advanceAtBeat);
                    }
                    else
                    {
                        nextClipShowingCheck = (nowBeatCheck >= advanceAtBeat - kNoteFallBeats);
                    }
                }
                bool isLiveJudgingCheck = playMode == PlayMode::Learn && !nextClipShowingCheck;
                const ChartClip* liveClip = session.CurrentClip();
                for (int lane = 0; lane < kLaneCount && !anyVisible; ++lane)
                {
                    if (isLiveJudgingCheck && liveClip)
                    {
                        if (AnyNoteVisible(*liveClip, lane, nowBeatCheck - kDiagBeatsBehind, nowBeatCheck + kNoteFallBeats))
                        {
                            anyVisible = true;
                        }
                    }
                    else if (preview)
                    {
                        double dotsFromBeat = session.PreviewFirstOnsetBeatForLane(lane);
                        if (dotsFromBeat >= 0.0 &&
                            AnyNoteVisible(*preview, lane, dotsFromBeat, nowBeatCheck + kNoteFallBeats))
                        {
                            anyVisible = true;
                        }
                    }
                }
            }

            if (couldShowSomething && !anyVisible)
            {
                if (blankStartSeconds < 0.0)
                {
                    blankStartSeconds = session.Clock().ElapsedSeconds();
                }
            }
            else if (blankStartSeconds >= 0.0)
            {
                double duration = session.Clock().ElapsedSeconds() - blankStartSeconds;
                if (duration > 0.25)
                {
                    printf("[t=%.2fs]   ** BLANK ** lane row empty for %.2fs (from t=%.2fs) while play_mode=%ls\n",
                           session.Clock().ElapsedSeconds(), duration, blankStartSeconds, PlayModeName(playMode));
                }
                blankStartSeconds = -1.0;
            }
        }

        // Release any lane whose planned hold has reached its end, whether
        // or not judging is currently live - mirrors GameSession's own
        // "a hold resolves on its own merits" behavior.
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                ++releaseCallCount;
                JudgementResult result = session.ConsumeLastJudgement();
                printf("[t=%.2fs]   lane %d release -> %ls (streak=%d)\n", session.Clock().ElapsedSeconds(), lane,
                       JudgementName(result), session.CurrentStreak());
                heldByUs[lane] = false;
            }
        }

        // Auto-press each lane at the right moment whenever real judging is
        // active - only ever true for a `learn` section; `solo`/`background`
        // sections never judge, so OnPress/OnRelease are simply never called
        // for them here, same as NoteLane never draws receptors for them.
        // Deliberately keeps pressing through awaitingAdvance now (the
        // post-lock-in extension), unless DIAG_MISS_AFTER_LOCKIN asked us to
        // stop once locked in for this section.
        if (phase == GamePhase::Learning && playMode == PlayMode::Learn && !inIntro && !stopPressingThisSection)
        {
            const ChartClip& clip = session.Song().clips[session.Song().sections[sectionIndex].clipIndex];
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (clip.laneNotes[lane].empty() || heldByUs[lane])
                {
                    continue;
                }
                double nextBeat = session.NextExpectedBeatForLane(lane);
                if (std::abs(nextBeat - lastPressedBeat[lane]) <= 1e-6)
                {
                    continue;
                }
                double onsetSeconds = nextBeat * secondsPerBeat;
                if (session.Clock().ElapsedSeconds() >= onsetSeconds + kTestPressOffsetSeconds)
                {
                    session.OnPress(lane);
                    JudgementResult result = session.ConsumeLastJudgement();
                    printf("[t=%.2fs]   lane %d press beat=%.2f -> %ls\n", session.Clock().ElapsedSeconds(), lane,
                           nextBeat, JudgementName(result));
                    lastPressedBeat[lane] = nextBeat;

                    if (result == JudgementResult::None)
                    {
                        // Press was judged correct (no immediate Miss) - a
                        // hold started, so schedule the matching release.
                        double durationBeats = DurationForLaneNote(clip, lane, nextBeat);
                        releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat + kTestReleaseOffsetSeconds;
                        heldByUs[lane] = true;
                    }
                }
            }
        }

        if (GetTickCount() - startTick > 180000)
        {
            printf("TIMEOUT after 60s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("Final phase: %ls\n", PhaseName(session.Phase()));
    if (diagEasyMode)
    {
        printf("easy-mode OnRelease call count: %d (expected 0)%s\n", releaseCallCount,
               releaseCallCount != 0 ? " ** MISMATCH **" : "");
    }

    session.Stop();
    engine.Shutdown();
    return 0;
}
