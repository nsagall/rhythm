#include <windows.h>

#include <cmath>
#include <cstdio>

#include "AudioEngine.h"
#include "DiagTestHelpers.h"
#include "GameSession.h"

// Standalone diagnostic (not part of the normal build): verifies the new
// bank/streak/multiplier scoring model (see GameSession::RegisterHit/
// RegisterMiss/Update()'s own banking comment, StreakTracker) end to end
// against section 0 of a real chart, in six phases:
//
//   1. Play perfectly for a couple of hits - CurrentBank()/ScoringStreak()/
//      CurrentMultiplier() must track the flat 10/5 precise/imprecise payout
//      and MultiplierForStreak exactly after every real Hit.
//   2. Deliberately let exactly ONE note time out (an isolated miss) - the
//      core formula change from the old any-miss-wipes-the-pot model: bank
//      and streak must both be completely UNCHANGED afterward, since only a
//      3-in-a-row trip is allowed to wipe anything now.
//   3. Resume playing perfectly for a couple more hits, then deliberately
//      let exactly THREE notes time out in a row - the shared streak
//      tracker must trip: bank drops to 0, streak drops to 0, multiplier
//      drops to x1, and a StreakBroken SfxEvent is queued.
//   4. Resume playing perfectly until the section locks in (IsPassing()).
//   5. From the instant it locks in, press NOTHING for the rest of the
//      section - Update()'s Pass-mode post-lock-in auto-accrual must be the
//      section's sole scorer from here on: real Hit JudgementEvents keep
//      arriving with no input, bank/streak keep climbing, and
//      IsLaneJudgeable stays false throughout (a real press would no longer
//      have anywhere to go).
//   6. Once the section actually finishes (advances or the song completes),
//      the payout must equal the bank at that instant times the multiplier
//      at that instant, folded into CurrentScore(), with CurrentBank() back
//      at 0 immediately after.
//
// Mirrors GameSession.cpp's own kPrecisePoints/kImprecisePoints/
// kImprecisionToleranceFraction/kMultiplierTierStreaks constants (private to
// that file) so this can compute the same closed-form expectation
// independently, rather than importing GameSession's own internals.

namespace
{

constexpr int kPrecisePoints = 10;
constexpr int kImprecisePoints = 5;
constexpr double kImprecisionToleranceFraction = 0.5;
constexpr int kMultiplierTierStreaks[] = {10, 20, 30};

int MultiplierForStreak(int streak)
{
    int multiplier = 1;
    for (int tierStreak : kMultiplierTierStreaks)
    {
        if (streak >= tierStreak)
        {
            ++multiplier;
        }
    }
    return multiplier;
}

enum class TestPhase
{
    PlayPerfectlyPhase1,
    InduceIsolatedMiss,
    PlayPerfectlyPhase2,
    InduceTripleMiss,
    PlayPerfectlyUntilLockIn,
    NoInputAfterLockIn,
    Done,
};

} // namespace

int main(int argc, char** argv)
{
    std::wstring chartPath = L"Content/Cool Boy/Cool Boy.chart";
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

    bool heldByUs[kLaneCount] = {};
    double releaseAtSeconds[kLaneCount] = {};
    double lastPressedBeat[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        lastPressedBeat[lane] = -1.0;
    }

    TestPhase phase = TestPhase::PlayPerfectlyPhase1;
    int hitsThisPhase = 0;
    int missesThisPhase = 0;
    int expectedBank = 0;
    int expectedStreak = 0;

    bool isolatedMissOk = false;
    bool tripleMissOk = false;
    bool sawStreakBrokenSfx = false;
    bool sawMultiplierUpSfx = false;
    bool lockInObserved = false;
    bool autoAccrualObserved = false;
    bool autoAccrualStayedUnjudgeable = true;
    bool formulaMismatch = false;
    int bankBeforeFinish = -1;
    int multiplierBeforeFinish = -1;
    int scoreBeforeFinish = -1;
    bool payoutOk = false;
    bool payoutObserved = false;

    DWORD startTick = GetTickCount();
    // Tracks the section current as of the END of the previous iteration -
    // captured before THIS iteration's Update() call runs, since Update()
    // itself is what performs a section transition synchronously (so by the
    // time CurrentSectionIndex() is read again afterward, the transition has
    // already happened - reading "before" only after Update() would always
    // see the post-transition value on both sides of a comparison).
    int sectionIndexBeforeUpdate = -1;

    while (session.Phase() != GamePhase::Complete && phase != TestPhase::Done)
    {
        int sectionIndexBeforeThisUpdate = sectionIndexBeforeUpdate;
        // Snapshotted before Update() runs, for the same reason
        // sectionIndexBeforeThisUpdate is - if this same Update() call both
        // adds one last auto-accrual hit AND performs the section-finish
        // payout, reading these afterward would already show the paid-out
        // (bank==0, score-already-increased) state on both sides of the
        // "what should the payout have been" comparison below.
        int bankBeforeThisUpdate = (phase == TestPhase::NoInputAfterLockIn) ? session.CurrentBank() : -1;
        int multiplierBeforeThisUpdate = (phase == TestPhase::NoInputAfterLockIn) ? session.CurrentMultiplier() : -1;
        int scoreBeforeThisUpdate = (phase == TestPhase::NoInputAfterLockIn) ? session.CurrentScore() : -1;

        session.Update();
        sectionIndexBeforeUpdate = session.CurrentSectionIndex();

        // Two lanes' holds can mature in the same Update() batch (e.g. a
        // chord), producing more than one JudgementEvent per
        // ConsumeJudgementEvents() call - by the time this loop sees the
        // FIRST event, RegisterHit for every event in the batch has already
        // run against the live session. So expectedBank/expectedStreak are
        // accumulated across the whole batch first, and only checked against
        // session.CurrentBank()/ScoringStreak() once, after the loop -
        // checking mid-batch would spuriously "mismatch" on every event but
        // the last one in a multi-event batch, without anything actually
        // being wrong.
        bool sawHitThisBatch = false;
        for (const GameSession::JudgementEvent& event : session.ConsumeJudgementEvents())
        {
            if (event.result == JudgementResult::Hit)
            {
                expectedBank += event.precise ? kPrecisePoints : kImprecisePoints;
                ++expectedStreak;
                sawHitThisBatch = true;
                printf("[t=%.3fs] Hit (precise=%s)\n", session.Clock().ElapsedSeconds(),
                       event.precise ? "true" : "false");

                if (phase == TestPhase::NoInputAfterLockIn)
                {
                    autoAccrualObserved = true;
                }
            }
            else if (event.result == JudgementResult::Miss)
            {
                ++missesThisPhase;
                printf("[t=%.3fs] Miss #%d this phase (bank=%d streak=%d)\n", session.Clock().ElapsedSeconds(),
                       missesThisPhase, session.CurrentBank(), session.ScoringStreak());
            }
        }
        if (sawHitThisBatch)
        {
            bool bankOk = session.CurrentBank() == expectedBank;
            bool streakOk = session.ScoringStreak() == expectedStreak;
            bool multiplierOk = session.CurrentMultiplier() == MultiplierForStreak(expectedStreak);
            formulaMismatch |= !bankOk || !streakOk || !multiplierOk;
            printf("  -> bank=%d (expected %d) streak=%d (expected %d) multiplier=x%d (expected x%d)%s\n",
                   session.CurrentBank(), expectedBank, session.ScoringStreak(), expectedStreak,
                   session.CurrentMultiplier(), MultiplierForStreak(expectedStreak),
                   (bankOk && streakOk && multiplierOk) ? "" : " ** MISMATCH **");
        }

        for (GameSession::SfxCue cue : session.ConsumeSfxEvents())
        {
            if (cue == GameSession::SfxCue::StreakBroken)
            {
                sawStreakBrokenSfx = true;
            }
            else if (cue == GameSession::SfxCue::MultiplierUp)
            {
                sawMultiplierUpSfx = true;
            }
        }

        // Section-finish payout check: fires the instant CurrentSectionIndex()
        // changes (or the song completes) right after we deliberately stopped
        // pressing - using the snapshot taken just before THIS Update() call
        // (see bankBeforeThisUpdate's own comment), not whatever's live now.
        if (phase == TestPhase::NoInputAfterLockIn && !payoutObserved)
        {
            int nowSectionIndex = session.CurrentSectionIndex();
            if (nowSectionIndex != sectionIndexBeforeThisUpdate || session.Phase() == GamePhase::Complete)
            {
                payoutObserved = true;
                bankBeforeFinish = bankBeforeThisUpdate;
                multiplierBeforeFinish = multiplierBeforeThisUpdate;
                scoreBeforeFinish = scoreBeforeThisUpdate;
                int expectedScoreAfter = scoreBeforeFinish + bankBeforeFinish * multiplierBeforeFinish;
                bool scoreOk = session.CurrentScore() == expectedScoreAfter;
                bool bankResetOk = session.CurrentBank() == 0;
                payoutOk = scoreOk && bankResetOk;
                printf("[t=%.3fs] section finished: bank %d * multiplier x%d -> score %d (expected %d), bank now "
                       "%d%s\n",
                       session.Clock().ElapsedSeconds(), bankBeforeFinish, multiplierBeforeFinish,
                       session.CurrentScore(), expectedScoreAfter, session.CurrentBank(),
                       payoutOk ? "" : " ** MISMATCH **");
                phase = TestPhase::Done;
                break;
            }
        }

        if (phase == TestPhase::Done)
        {
            break;
        }

        int sectionIndex = session.CurrentSectionIndex();
        if (sectionIndex == -1)
        {
            // Still in the count-in - nothing to press yet.
            Sleep(5);
            continue;
        }
        if (sectionIndex != 0)
        {
            // Never expected before lock-in-driven completion above - section
            // 0 is the only one this diagnostic drives.
            printf("Unexpectedly left section 0 (index=%d) before the test finished\n", sectionIndex);
            break;
        }

        const ChartClip& clip = session.Song().clips[session.Song().sections[0].clipIndex];
        double secondsPerBeat = 60.0 / session.Song().bpm;

        // Resolve holds (press+release both matter, non-easy-mode) - a
        // held-by-us note's release is timed independently of which phase
        // we're in, since it was already started before any phase change.
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (heldByUs[lane] && session.Clock().ElapsedSeconds() >= releaseAtSeconds[lane])
            {
                session.OnRelease(lane);
                heldByUs[lane] = false;
            }
        }

        switch (phase)
        {
            case TestPhase::PlayPerfectlyPhase1:
            case TestPhase::PlayPerfectlyPhase2:
            case TestPhase::PlayPerfectlyUntilLockIn:
            {
                if (phase == TestPhase::PlayPerfectlyUntilLockIn && session.IsPassing() && !lockInObserved)
                {
                    lockInObserved = true;
                    bankBeforeFinish = session.CurrentBank();
                    multiplierBeforeFinish = session.CurrentMultiplier();
                    scoreBeforeFinish = session.CurrentScore();
                    printf("[t=%.3fs] locked in - switching to no-input auto-accrual phase\n",
                           session.Clock().ElapsedSeconds());
                    phase = TestPhase::NoInputAfterLockIn;
                    break;
                }

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
                    if (session.Clock().ElapsedSeconds() < onsetSeconds)
                    {
                        continue;
                    }

                    lastPressedBeat[lane] = nextBeat;
                    session.OnPress(lane);
                    session.ConsumeJudgementEvents(); // a correct press produces no event yet - see above

                    double durationBeats =
                        DiagTestHelpers::DurationForLaneNote(clip, lane, session.CurrentClipOriginBeat(), nextBeat);
                    releaseAtSeconds[lane] = (nextBeat + durationBeats) * secondsPerBeat;
                    heldByUs[lane] = true;

                    if (phase == TestPhase::PlayPerfectlyPhase1 || phase == TestPhase::PlayPerfectlyPhase2)
                    {
                        ++hitsThisPhase;
                    }
                }

                if (phase == TestPhase::PlayPerfectlyPhase1 && hitsThisPhase >= 2)
                {
                    printf("[t=%.3fs] observed %d perfect hits - now inducing one isolated miss\n",
                           session.Clock().ElapsedSeconds(), hitsThisPhase);
                    phase = TestPhase::InduceIsolatedMiss;
                    missesThisPhase = 0;
                }
                else if (phase == TestPhase::PlayPerfectlyPhase2 && hitsThisPhase >= 2)
                {
                    printf("[t=%.3fs] observed %d more perfect hits - now inducing a triple miss\n",
                           session.Clock().ElapsedSeconds(), hitsThisPhase);
                    phase = TestPhase::InduceTripleMiss;
                    missesThisPhase = 0;
                }
                break;
            }

            case TestPhase::InduceIsolatedMiss:
                // Deliberately press nothing (see the held-note release loop
                // above for anything already in flight) until Update()'s own
                // timeout path registers exactly one Miss.
                if (missesThisPhase >= 1)
                {
                    isolatedMissOk = (session.CurrentBank() == expectedBank) && (session.ScoringStreak() == expectedStreak);
                    printf("[t=%.3fs] isolated miss resolved - bank=%d (expected %d) streak=%d (expected %d)%s\n",
                           session.Clock().ElapsedSeconds(), session.CurrentBank(), expectedBank,
                           session.ScoringStreak(), expectedStreak, isolatedMissOk ? "" : " ** MISMATCH **");
                    phase = TestPhase::PlayPerfectlyPhase2;
                    hitsThisPhase = 0;
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        lastPressedBeat[lane] = -1.0; // let the next due note re-qualify for a press
                    }
                }
                break;

            case TestPhase::InduceTripleMiss:
                if (missesThisPhase >= 3)
                {
                    tripleMissOk = (session.CurrentBank() == 0) && (session.ScoringStreak() == 0) &&
                                    (session.CurrentMultiplier() == 1);
                    printf("[t=%.3fs] triple miss resolved - bank=%d streak=%d multiplier=x%d%s\n",
                           session.Clock().ElapsedSeconds(), session.CurrentBank(), session.ScoringStreak(),
                           session.CurrentMultiplier(), tripleMissOk ? "" : " ** MISMATCH **");
                    expectedBank = 0;
                    expectedStreak = 0;
                    phase = TestPhase::PlayPerfectlyUntilLockIn;
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        lastPressedBeat[lane] = -1.0;
                    }
                }
                break;

            case TestPhase::NoInputAfterLockIn:
                // Deliberately no OnPress calls at all - see the file's own
                // header comment. Just confirm every lane really is
                // unjudgeable (a real press would be silently wasted).
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    if (!clip.laneNotes[lane].empty() && session.IsLaneJudgeable(lane))
                    {
                        autoAccrualStayedUnjudgeable = false;
                    }
                }
                break;

            case TestPhase::Done:
                break;
        }

        if (GetTickCount() - startTick > 60000)
        {
            printf("TIMEOUT after 60s, aborting\n");
            break;
        }

        Sleep(5);
    }

    printf("\n=== RESULTS ===\n");
    printf("Bank/streak/multiplier matched the flat precise/imprecise formula on every hit: %s\n",
           formulaMismatch ? "false ** MISMATCH **" : "true");
    printf("An isolated (non-tripping) miss left bank/streak completely unchanged: %s%s\n",
           isolatedMissOk ? "true" : "false", isolatedMissOk ? "" : " ** MISMATCH **");
    printf("A 3-in-a-row miss tripped the shared streak tracker (bank/streak/multiplier all reset): %s%s\n",
           tripleMissOk ? "true" : "false", tripleMissOk ? "" : " ** MISMATCH **");
    printf("StreakBroken SFX cue observed on the trip: %s%s\n", sawStreakBrokenSfx ? "true" : "false",
           sawStreakBrokenSfx ? "" : " ** MISMATCH **");
    printf("MultiplierUp SFX cue observed at some point: %s%s\n", sawMultiplierUpSfx ? "true" : "false",
           sawMultiplierUpSfx ? "" : " (never reached a higher tier - not necessarily a bug, depends on the chart)");
    printf("Section locked in: %s%s\n", lockInObserved ? "true" : "false", lockInObserved ? "" : " ** MISMATCH **");
    printf("Post-lock-in auto-accrual produced real Hit events with zero input: %s%s\n",
           autoAccrualObserved ? "true" : "false", autoAccrualObserved ? "" : " ** MISMATCH **");
    printf("Every lane stayed unjudgeable throughout the no-input phase: %s%s\n",
           autoAccrualStayedUnjudgeable ? "true" : "false", autoAccrualStayedUnjudgeable ? "" : " ** MISMATCH **");
    printf("Section-finish payout (bank * multiplier folded into score, bank reset to 0): %s%s\n",
           payoutOk ? "true" : "false", payoutObserved ? "" : " ** MISMATCH - never observed **");
    printf("Final score: %d, final streak: %d, final multiplier: x%d\n", session.CurrentScore(),
           session.ScoringStreak(), session.CurrentMultiplier());

    session.Stop();
    engine.Shutdown();
    return 0;
}
