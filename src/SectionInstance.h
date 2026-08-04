#pragma once

#include <vector>

#include "ChartFile.h"
#include "LaneConfig.h"

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
// what the .chart file says) and from a clip's own playback voice
// (GameSession's private ClipVoice - shared across every section that ever
// references that clip, since two sections reusing the same clip must
// never disagree about whether it's playing or where its groove is - see
// ClipVoice's own comment). GameSession replaces its current instance
// wholesale (m_currentInstance = SectionInstance(index)) every time a new
// section begins, so there's never any stale field left over to remember
// to reset by hand.
//
// A Break section gets one of these too, same as Learn - it uses the
// pending-advance half of this state (every section schedules its own
// advance the instant it begins) but never touches the judging half
// (streak/isLockedIn/lane holds/judged notes all just sit at their
// default, unused values), since IsLaneJudgeable() only ever returns true
// for a Learn section. Reset/Background sections never become "current" at
// all (GameSession::BeginSection recurses straight through both), so
// neither ever gets an instance of its own.
class SectionInstance
{
public:
    // sectionIndex == -1 represents "no current section" (Idle, or before
    // Start()) - every other field defaults to its own "nothing has
    // happened yet" value.
    explicit SectionInstance(int sectionIndex = -1);

    int SectionIndex() const
    {
        return m_sectionIndex;
    }

    int Streak() const
    {
        return m_streak;
    }

    bool IsLockedIn() const
    {
        return m_lockedIn;
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
    // ChartTiming::NextOnsetAfter.
    void AdvanceExpectedNote(int lane, double originBeat, const ChartClip& clip);

    bool IsLaneHeld(int lane) const;
    double LaneHoldStartBeat(int lane) const;
    double LaneHoldExpectedEndBeat(int lane) const;
    void StartLaneHold(int lane, double startBeat, double expectedEndBeat);
    void ClearLaneHold(int lane);

    // Records a hit: advances the streak (unless already locked in, where
    // it's frozen at its lock-in value and no longer moves), and locks the
    // section in once the streak reaches hitsRequired. Returns true the
    // instant lock-in is newly reached this call (so the caller can react
    // - e.g. switching the clip's own volume), false every other time.
    bool RegisterHit(int hitsRequired);

    // Records a miss: a no-op once already locked in. In easy mode, the
    // very first miss each section is instead fully forgiven (streak and
    // consecutive-miss count both left untouched) - see the
    // m_easyGraceAvailable field comment. Otherwise resets the streak and
    // counts toward the consecutive-miss limit; returns true the instant
    // that limit is reached, so the caller knows to stop the clip's audio.
    bool RegisterMiss(bool easyMode);

    JudgementResult OnsetJudgement(double startBeat, int lane) const;

    // Records a judgement for a specific lane note, for OnsetJudgement()
    // to look up later. Trims old entries so this can't grow unbounded.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result);

private:
    // One lane currently mid-hold: its press was judged correct and its
    // release hasn't been judged yet (by a real key-up or by
    // GameSession::Update's held-past-tolerance timeout).
    struct LaneHold
    {
        bool active = false;
        double startBeat = 0.0;
        double expectedEndBeat = 0.0;
    };

    // How one specific lane note (identified by its start beat) was judged
    // - recorded so OnsetJudgement() can look it up later and NoteLane can
    // color a note that's already scrolled past the judge line.
    struct JudgedLaneNote
    {
        double beat = 0.0;
        int lane = 0;
        JudgementResult result = JudgementResult::None;
    };

    int m_sectionIndex = -1;
    int m_streak = 0;
    int m_consecutiveMisses = 0;

    // One-note grace period (easy mode only): true until this section's
    // first miss consumes it in RegisterMiss - that miss is then fully
    // forgiven. Never consulted when easyMode is false.
    bool m_easyGraceAvailable = true;

    bool m_lockedIn = false;
    bool m_hasPendingAdvance = false;
    double m_pendingAdvanceAtSeconds = 0.0;

    double m_nextExpectedBeat[kLaneCount] = {};
    LaneHold m_laneHolds[kLaneCount];
    std::vector<JudgedLaneNote> m_judgedNotes;
};
