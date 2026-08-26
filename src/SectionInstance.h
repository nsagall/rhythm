#pragma once

#include <vector>

#include "ChartClip.h"
#include "LaneConfig.h"
#include "StreakTracker.h"

// Result of judging a specific press/release against a note. Cleared once
// read via GameSession::ConsumeLastJudgement().
enum class JudgementResult
{
    None,
    Hit,
    Miss,
};

// The live, mutable state of one run through the current [learn]/[break]
// section - everything that's true only "while this particular section is
// current," as distinct from ChartSection/ChartClip (immutable - exactly
// what the .chart file says) and from a clip's own playback voice (see
// ChartClip's own ClipInstance - shared across every section that ever
// references that clip, since two sections reusing the same clip must
// never disagree about whether it's playing or where its groove is).
// GameSession replaces its current instance
// wholesale (m_currentInstance = SectionInstance(index)) every time a new
// section begins, so there's never any stale field left over to remember
// to reset by hand.
//
// A Break section gets one of these too, same as Learn - it uses the
// pending-advance half of this state (every section schedules its own
// advance the instant it begins) but never touches the judging half
// (streak/isPassing/lane holds/judged notes all just sit at their
// default, unused values), since IsLaneJudgeable() only ever returns true
// for a Learn section. Reset/Background sections never become "current" at
// all (GameSession::BeginSection recurses straight through both), so
// neither ever gets an instance of its own.
class SectionInstance
{
public:
    // sectionIndex == -1 represents "no current section" (Idle, or before
    // Start()) - every other field defaults to its own "nothing has
    // happened yet" value. mode only matters for a Learn section (see
    // ChartClip::learnMode) - it decides m_passing's starting value: false
    // (Pass mode, matching today) or true (DontFail mode - a track starts
    // out already passing, only a miss ever drops it to failing).
    explicit SectionInstance(int sectionIndex = -1, LearnMode mode = LearnMode::Pass);

    int SectionIndex() const
    {
        return m_sectionIndex;
    }

    int Streak() const
    {
        return m_streak;
    }

    // Pass mode: true is a one-way latch - once reached, never reverts for
    // the rest of this section's run. DontFail mode: reversible - a miss
    // can drop this back to false (see RegisterMiss), and re-earning
    // hitsRequired in a row brings it back to true.
    bool IsPassing() const
    {
        return m_passing;
    }

    bool HasPendingAdvance() const
    {
        return m_hasPendingAdvance;
    }

    // Raw value, regardless of HasPendingAdvance() - a caller that already
    // knows (or doesn't care) whether one is actually scheduled can read
    // this directly; GameSession::PendingAdvanceAtSeconds() is what adds
    // the "-1 if nothing's pending" contract for external callers.
    double PendingAdvanceAtSeconds() const
    {
        return m_pendingAdvanceAtSeconds;
    }

    // Schedules (or replaces) this section's candidate advance instant.
    void SchedulePendingAdvance(double atSeconds);

    // Pushes the candidate advance back by exactly one more loop, for a
    // Learn section that reached it without locking in - see
    // GameSession::Update's own comment on why this repeats instead of
    // ever abandoning the section.
    void ExtendPendingAdvance(double byStemDurationSeconds);

    // The section has actually advanced - nothing left pending.
    void ClearPendingAdvance();

    // Returns 0.0 for an out-of-range lane, matching every other per-lane
    // accessor below.
    double NextExpectedBeatForLane(int lane) const;
    void SetNextExpectedBeat(int lane, double beat);

    // Moves lane's next-expected-note pointer forward to the next note
    // after it, on the clip's own persistent origin (originBeat) - see
    // ChartClip::NextOnsetAfter.
    void AdvanceExpectedNote(int lane, double originBeat, const ChartClip& clip);

    bool IsLaneHeld(int lane) const;
    double LaneHoldStartBeat(int lane) const;
    double LaneHoldExpectedEndBeat(int lane) const;

    // True if this lane's press (already judged correct - see IsLaneHeld)
    // landed within half of the effective start tolerance of the note's own
    // onset - false if it was still within the full tolerance (so still a
    // correct press) but closer to half than to dead-on. Set once by
    // StartLaneHold from the press itself; read back by GameSession::
    // RegisterHit (via OnRelease, or directly in easy mode) to decide how
    // many points the eventual Hit is worth - see GameSession::RegisterHit.
    // Only meaningful while IsLaneHeld(lane) is true.
    bool LaneHoldWasPrecise(int lane) const;

    void StartLaneHold(int lane, double startBeat, double expectedEndBeat, bool wasPrecise);
    void ClearLaneHold(int lane);

    // Records a hit: always registers with streakTracker first (see its own
    // comment - the shared streak keeps growing even once this section is
    // already passing, since GameSession's post-lock-in
    // auto-accrual depends on it), then advances this section's own
    // hitsRequired progress (unless already passing, where it's frozen at
    // its passing value and no longer moves - true whether that's Pass
    // mode's permanent lock-in or DontFail mode's current passing streak),
    // and starts passing once that progress reaches hitsRequired. Returns
    // true the instant passing is newly (re-)reached this call (so the
    // caller can react - e.g. switching the clip's own volume), false every
    // other time. No logic difference between modes needed here - see
    // m_passing's own comment.
    bool RegisterHit(int hitsRequired, StreakTracker& streakTracker);

    // What RegisterMiss found happened - see its own comment.
    struct MissResult
    {
        // streakTracker just tripped (StreakTracker::c_MaxConsecutiveMisses
        // reached) - same meaning, same threshold, in both modes; the
        // caller should stop the clip's audio.
        bool shouldStopClip = false;
        // DontFail mode only: this section was passing, and this miss just
        // dropped it to failing - the caller should react (e.g. revert the
        // clip's volume, and the note lane should swap its upcoming-notes
        // preview). Never true in Pass mode, where a miss while passing is
        // already a no-op below.
        bool justEnteredFailState = false;
        // True only for RegisterMiss's own early-return case: already
        // passing in Pass mode, where a miss is a complete no-op (streak/
        // consecutive-miss counters untouched too, unlike every other
        // case). Lets a caller that needs to know this (e.g. to skip
        // emitting a judgement event or wiping score for a no-op miss)
        // read it back here instead of recomputing the same condition by
        // hand just before calling RegisterMiss.
        bool wasNoOpAlreadyPassing = false;
    };

    // Records a miss: a no-op once passing, in Pass mode (frozen forever,
    // exactly like RegisterHit) - streakTracker isn't touched either, same
    // as every other consequence below. In easy mode, each of this
    // section's first c_EasyGraceMisses misses is instead fully forgiven
    // regardless of mode or current passing state (this section's own
    // hitsRequired progress and streakTracker both left untouched, and no
    // mode-specific consequence fires either) - see the m_easyGraceRemaining
    // field comment. Otherwise resets this section's own hitsRequired
    // progress and registers with streakTracker (see its own comment for
    // what that does); in DontFail mode, a miss while passing additionally
    // drops back to failing.
    MissResult RegisterMiss(bool easyMode, StreakTracker& streakTracker);

    JudgementResult OnsetJudgement(double startBeat, int lane) const;

    // Whether the same lane note OnsetJudgement(startBeat, lane) resolves to
    // a Hit was a precise one (see GameSession::JudgementEvent::precise) -
    // true (the harmless default) for a Miss, or a note never judged/too old
    // to still be tracked, since neither has a "how precise" to report.
    bool OnsetPrecise(double startBeat, int lane) const;

    // Records a judgement for a specific lane note, for OnsetJudgement()
    // to look up later. Trims old entries so this can't grow unbounded.
    // precise is only meaningful when result == JudgementResult::Hit - see
    // OnsetPrecise.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise = true);

private:
    // One lane currently mid-hold: its press was judged correct and its
    // release hasn't been judged yet (by a real key-up or by
    // GameSession::Update's held-past-tolerance timeout).
    struct LaneHold
    {
        bool active = false;
        double startBeat = 0.0;
        double expectedEndBeat = 0.0;
        bool wasPrecise = true;
    };

    // How one specific lane note (identified by its start beat) was judged
    // - recorded so OnsetJudgement() can look it up later and NoteLane can
    // color a note that's already scrolled past the judge line.
    struct JudgedLaneNote
    {
        double beat = 0.0;
        int lane = 0;
        JudgementResult result = JudgementResult::None;
        bool precise = true;
    };

    int m_sectionIndex = -1;
    LearnMode m_mode = LearnMode::Pass;
    int m_streak = 0;

    // Miss grace period (easy mode only): counts down each time RegisterMiss
    // fully forgives a miss, starting at c_EasyGraceMisses - so a beginner's
    // first couple of stumbles in a section don't cost anything. Never
    // consulted when easyMode is false.
    static constexpr int c_EasyGraceMisses = 2;
    int m_easyGraceRemaining = c_EasyGraceMisses;

    bool m_passing = false;
    bool m_hasPendingAdvance = false;
    double m_pendingAdvanceAtSeconds = 0.0;

    double m_nextExpectedBeat[c_LaneCount] = {};
    LaneHold m_laneHolds[c_LaneCount];
    std::vector<JudgedLaneNote> m_judgedNotes;
};
