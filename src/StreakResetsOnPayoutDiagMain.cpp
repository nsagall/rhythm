#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies that
// GameSession::m_streakTracker resets to 0 - and CurrentMultiplier() drops
// back to x1 - the instant a section's bank actually pays out (see
// Update()'s own banking comment and StreakTracker::Reset()), not just on
// the shared 3-consecutive-miss trip (already covered by
// ScoringDiagMain.cpp). This is what makes the streak carry across a
// Reset/Background section, or into a Break with nothing of its own to pay
// out, but NOT across a Learn section's own advance - reaching an advance at
// all requires having just scored something, so its own payout resets the
// streak every time in practice.
//
// Drives test_charts/section_modes_test.chart end to end, playing every
// judgeable note perfectly: [background: bass_drum] -> [learn:
// fast_test_hat, hits_required=4] -> [reset] -> [break: bass_drum] ->
// [learn: fast_test_hat again]. The first learn section's streak/multiplier
// are captured the instant its payout happens; the second learn section's
// streak/multiplier are captured the instant it begins - both must read
// back to 0 / x1, not whatever the first section had built up.

namespace
{

// Presses+releases every currently-due note across all lanes of clip,
// exactly on time - shared by both learn sections in this chart, since both
// use the same clip. Returns once nothing more is due to press this call.
void PlayDueNotesPerfectly(GameSession& session, const ChartClip& clip, double lastPressedBeat[c_LaneCount],
                            bool heldByUs[c_LaneCount], double releaseAtSeconds[c_LaneCount])
{
    double secondsPerBeat = 60.0 / session.Song().Bpm();

    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
        {
            session.OnRelease(lane);
            session.ConsumeJudgementEvents();
            heldByUs[lane] = false;
        }
    }

    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        if (clip.LaneNotes(lane).empty() || heldByUs[lane] || !session.IsLaneJudgeable(lane))
        {
            continue;
        }
        double nextBeat = session.NextExpectedBeatForLane(lane);
        if (std::abs(nextBeat - lastPressedBeat[lane]) <= 1e-6)
        {
            continue;
        }
        double onsetSeconds = nextBeat * secondsPerBeat;
        if (session.Clock().ElapsedSeconds() < onsetSeconds)
        {
            continue;
        }

        lastPressedBeat[lane] = nextBeat;
        session.OnPress(lane);
        session.ConsumeJudgementEvents();

        double durationBeats =
            DiagTestHelpers::DurationForLaneNote(clip, lane, session.CurrentClipOriginBeat(), nextBeat);
        releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
        heldByUs[lane] = true;
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring chartPath = L"test_charts/section_modes_test.chart";
    if (argc > 1)
    {
        std::string arg = argv[1];
        chartPath.assign(arg.begin(), arg.end());
    }

    AudioEngine engine;
    if (!engine.Initialize())
    {
        printf("AudioEngine::Initialize failed\n");
        return 1;
    }

    GameSession session(engine);
    std::wstring loadError;
    if (!session.LoadChart(chartPath, /*easyMode=*/false, loadError))
    {
        wprintf(L"LoadChart failed: %ls\n", loadError.c_str());
        return 1;
    }

    session.Start();

    bool heldByUs[c_LaneCount] = {};
    double releaseAtSeconds[c_LaneCount] = {};
    double lastPressedBeat[c_LaneCount];
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    int firstLearnSectionIndex = -1;
    int secondLearnSectionIndex = -1;
    bool sawFirstLearnPayout = false;
    int scoreBeforePayout = -1;
    int streakBeforePayout = -1;
    int multiplierBeforePayout = -1;
    int streakAfterPayout = -1;
    int multiplierAfterPayout = -1;
    bool sawMultiplierDropEvent = false;
    bool sawSecondLearnBegin = false;
    int streakAtSecondLearnEntry = -1;
    int multiplierAtSecondLearnEntry = -1;
    int lastSectionIndex = -2;
    int prevSectionIndex = -1; // captured before each Update() - see its own use below

    DWORD startTick = GetTickCount();

    while (session.Phase() != GamePhase::Complete && !sawSecondLearnBegin)
    {
        int sectionIndexBeforeThisUpdate = prevSectionIndex;
        int scoreBeforeThisUpdate = session.CurrentScore();
        int streakBeforeThisUpdate = session.ScoringStreak();
        int multiplierBeforeThisUpdate = session.CurrentMultiplier();

        session.Update();
        prevSectionIndex = session.CurrentSectionIndex();

        // We only ever play perfect, on-time presses in this diagnostic - no
        // deliberate misses - so the only possible source of a
        // multiplier-drop-to-x1 event is the payout itself (never the
        // 3-miss trip), which is why this doesn't need to be gated on
        // sawFirstLearnPayout already being true (it isn't yet, this early
        // in the very iteration the payout happens).
        for (const auto& event : session.ConsumeHudChangeEvents())
        {
            if (event.field == GameSession::HudField::Multiplier && event.newValue == 1)
            {
                sawMultiplierDropEvent = true;
            }
        }
        session.ConsumeJudgementEvents();
        session.ConsumeSfxEvents();

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex == -1)
        {
            // Still in the count-in.
            Sleep(5);
            continue;
        }
        SectionKind kind = session.CurrentSectionKind();

        // The payout instant: sectionIndexBeforeThisUpdate was still the
        // first learn section, and this Update() call just paid it out
        // (score increased) - capture before/after snapshots using the
        // values read prior to this Update() call, same reasoning as
        // ScoringDiagMain's own bankBeforeThisUpdate.
        if (firstLearnSectionIndex != -1 && sectionIndexBeforeThisUpdate == firstLearnSectionIndex &&
            !sawFirstLearnPayout && session.CurrentScore() != scoreBeforeThisUpdate)
        {
            sawFirstLearnPayout = true;
            scoreBeforePayout = scoreBeforeThisUpdate;
            streakBeforePayout = streakBeforeThisUpdate;
            multiplierBeforePayout = multiplierBeforeThisUpdate;
            streakAfterPayout = session.ScoringStreak();
            multiplierAfterPayout = session.CurrentMultiplier();
            printf("[t=%.3fs] first learn section paid out: score %d -> %d, streak %d -> %d, multiplier x%d -> x%d\n",
                   session.Clock().ElapsedSeconds(), scoreBeforePayout, session.CurrentScore(), streakBeforePayout,
                   streakAfterPayout, multiplierBeforePayout, multiplierAfterPayout);
        }

        if (sectionIndex != lastSectionIndex)
        {
            printf("[t=%.3fs] section index -> %d (kind=%d), streak=%d multiplier=x%d\n",
                   session.Clock().ElapsedSeconds(), sectionIndex, static_cast<int>(kind), session.ScoringStreak(),
                   session.CurrentMultiplier());

            if (kind == SectionKind::Learn)
            {
                if (firstLearnSectionIndex == -1)
                {
                    firstLearnSectionIndex = sectionIndex;
                }
                else if (sectionIndex != firstLearnSectionIndex && !sawSecondLearnBegin)
                {
                    secondLearnSectionIndex = sectionIndex;
                    streakAtSecondLearnEntry = session.ScoringStreak();
                    multiplierAtSecondLearnEntry = session.CurrentMultiplier();
                    sawSecondLearnBegin = true;
                    printf("[t=%.3fs] second learn section began - streak=%d multiplier=x%d (both should read back "
                           "to a fresh start)\n",
                           session.Clock().ElapsedSeconds(), streakAtSecondLearnEntry, multiplierAtSecondLearnEntry);
                    break;
                }
            }

            lastSectionIndex = sectionIndex;
        }

        if (kind == SectionKind::Learn)
        {
            const ChartSection& section = session.Song().Sections()[sectionIndex];
            const ChartClip& clip = session.Song().Clips()[section.clipIndex];
            PlayDueNotesPerfectly(session, clip, lastPressedBeat, heldByUs, releaseAtSeconds);
        }

        if (GetTickCount() - startTick > 60000)
        {
            printf("TIMEOUT after 60s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("First learn section reached a payout: %s%s\n", sawFirstLearnPayout ? "true" : "false",
           sawFirstLearnPayout ? "" : " ** MISMATCH **");
    printf("Streak before that payout was > 0 (something to actually reset): %s%s\n",
           streakBeforePayout > 0 ? "true" : "false", streakBeforePayout > 0 ? "" : " ** MISMATCH **");
    bool streakResetOk = sawFirstLearnPayout && streakAfterPayout == 0;
    printf("Streak reset to 0 the instant the payout happened (%d -> %d): %s%s\n", streakBeforePayout,
           streakAfterPayout, streakResetOk ? "true" : "false", streakResetOk ? "" : " ** MISMATCH **");
    bool multiplierResetOk = sawFirstLearnPayout && multiplierAfterPayout == 1;
    printf("Multiplier dropped back to x1 in the same instant (x%d -> x%d): %s%s\n", multiplierBeforePayout,
           multiplierAfterPayout, multiplierResetOk ? "true" : "false", multiplierResetOk ? "" : " ** MISMATCH **");
    printf("A HudField::Multiplier change event fired for that drop (when it dropped from >x1): %s%s\n",
           sawMultiplierDropEvent ? "true" : "false",
           (multiplierBeforePayout > 1) ? (sawMultiplierDropEvent ? "" : " ** MISMATCH **")
                                         : " (multiplier was already x1 at payout - nothing to signal)");
    printf("Second learn section reached: %s%s\n", sawSecondLearnBegin ? "true" : "false",
           sawSecondLearnBegin ? "" : " ** MISMATCH **");
    bool secondSectionFreshStart =
        sawSecondLearnBegin && streakAtSecondLearnEntry == 0 && multiplierAtSecondLearnEntry == 1;
    printf("Second learn section began at streak=0, multiplier=x1 (NOT carried over from the first): %s%s\n",
           secondSectionFreshStart ? "true" : "false", secondSectionFreshStart ? "" : " ** MISMATCH **");

    session.Stop();
    engine.Shutdown();
    return 0;
}
