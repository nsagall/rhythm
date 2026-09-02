#pragma once

#include <vector>

#include "ChartClip.h"    // LearnMode enum used by value (m_mode member + constructor default).
#include "LaneConfig.h"   // c_LaneCount sizes m_nextExpectedBeat / m_laneHolds.

class StreakTracker;

// Result of judging a specific press/release against a note.
enum class JudgementResult
{
    None,
    Hit,
    Miss,
};

// The live, mutable state of one run through the current [learn]/[break] section - everything
// that's true only while this section is current, as distinct from ChartSection/ChartClip
// (immutable chart content) and from a clip's playback voice (ClipInstance, shared across every
// section that references that clip). GameSession replaces its current instance wholesale each time
// a section begins.
//
// A Break section gets one of these too: it uses the pending-advance half of the state but never
// the judging half (streak/passing/holds/judged notes stay at their defaults). Reset/Background
// sections never become current and never get an instance.
class SectionInstance
{
public:
    // Constructs an instance for one section run.
    //   sectionIndex - index of the section, or -1 for "no current section" (Idle, or before Start()).
    //   mode         - only used for a Learn section; decides m_passing's starting value (false in
    //                  Pass mode, true in DontFail mode, where a track starts passing and only a
    //                  miss drops it).
    explicit SectionInstance(int sectionIndex = -1, LearnMode mode = LearnMode::Pass);

    int SectionIndex() const
    {
        return m_sectionIndex;
    }

    int Streak() const
    {
        return m_streak;
    }

    // Pass mode: a one-way latch, never reverts once reached. DontFail mode: reversible - a miss
    // can drop it to false and re-earning hitsRequired in a row brings it back.
    bool IsPassing() const
    {
        return m_passing;
    }

    bool HasPendingAdvance() const
    {
        return m_hasPendingAdvance;
    }

    // The raw value, whether or not one is actually pending (see HasPendingAdvance()).
    double PendingAdvanceAtSeconds() const
    {
        return m_pendingAdvanceAtSeconds;
    }

    // Schedules (or replaces) this section's candidate advance instant.
    //   atSeconds - wall-clock second the section should advance at.
    void SchedulePendingAdvance(double atSeconds);

    // Pushes the candidate advance back by one more loop, for a Learn section that reached it
    // without locking in.
    //   byStemDurationSeconds - length of one loop of the clip's audio, in seconds.
    void ExtendPendingAdvance(double byStemDurationSeconds);

    // The section has actually advanced - nothing left pending.
    void ClearPendingAdvance();

    // Returns lane's next-expected-note beat, or 0.0 for an out-of-range lane.
    double NextExpectedBeatForLane(int lane) const;
    void SetNextExpectedBeat(int lane, double beat);

    // Moves lane's next-expected-note pointer forward to the next note after it.
    //   lane       - lane to advance.
    //   originBeat - the clip's persistent arrangement origin (see ChartClip::NextOnsetAfter).
    //   clip       - the section's clip.
    void AdvanceExpectedNote(int lane, double originBeat, const ChartClip& clip);

    bool IsLaneHeld(int lane) const;
    double LaneHoldStartBeat(int lane) const;
    double LaneHoldExpectedEndBeat(int lane) const;

    // True if this lane's (already-correct) press landed within half the effective start tolerance
    // of the note's onset; false if it was within the full tolerance but no better than half.
    // Meaningful only while IsLaneHeld(lane) is true. Used by GameSession::RegisterHit to grade points.
    bool LaneHoldWasPrecise(int lane) const;

    void StartLaneHold(int lane, double startBeat, double expectedEndBeat, bool wasPrecise);
    void ClearLaneHold(int lane);

    // Records a hit: registers with streakTracker (the shared streak keeps growing even once
    // passing), then advances this section's hitsRequired progress unless already passing, and
    // starts passing once it reaches hitsRequired.
    //   hitsRequired  - the clip's hits_required.
    //   streakTracker - the whole-song scoring streak.
    // Returns true the instant passing is newly (re-)reached this call, false otherwise.
    bool RegisterHit(int hitsRequired, StreakTracker& streakTracker);

    // What RegisterMiss found happened.
    struct MissResult
    {
        // streakTracker just tripped; the caller should stop the clip's audio.
        bool shouldStopClip = false;

        // DontFail mode: this miss dropped a passing section to failing. Never set in Pass mode.
        bool justEnteredFailState = false;

        // Pass mode, already passing: the miss was a complete no-op (counters untouched).
        bool wasNoOpAlreadyPassing = false;
    };

    // Records a miss.
    //   easyMode      - when true, each of this section's first c_EasyGraceMisses misses is fully
    //                   forgiven (no progress reset, no streak hit, no mode-specific consequence).
    //   streakTracker - the whole-song scoring streak.
    // A no-op once passing in Pass mode. Otherwise resets this section's hitsRequired progress and
    // registers with streakTracker; in DontFail mode a miss while passing also drops back to failing.
    MissResult RegisterMiss(bool easyMode, StreakTracker& streakTracker);

    JudgementResult OnsetJudgement(double startBeat, int lane) const;

    // Whether the same lane note's Hit was precise (see GameSession::JudgementEvent::precise).
    // Returns true (a harmless default) for a Miss or a note never judged / too old to track.
    bool OnsetPrecise(double startBeat, int lane) const;

    // Records a judgement for a specific lane note, for OnsetJudgement() to look up later. Trims
    // old entries so this can't grow unbounded.
    //   startBeat - the note's start beat, identifying it.
    //   lane      - the note's lane.
    //   result    - the judgement to record.
    //   precise   - meaningful only when result == Hit.
    void RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise = true);

private:
    // One lane currently mid-hold: its press was judged correct and its release hasn't been judged yet.
    struct LaneHold
    {
        bool active = false;
        double startBeat = 0.0;
        double expectedEndBeat = 0.0;
        bool wasPrecise = true;
    };

    // How one lane note (identified by its start beat) was judged.
    struct JudgedLaneNote
    {
        double beat = 0.0;
        int lane = 0;
        JudgementResult result = JudgementResult::None;
        bool precise = true;
    };

    static constexpr int c_EasyGraceMisses = 2;

    int m_sectionIndex = -1;
    LearnMode m_mode = LearnMode::Pass;
    int m_streak = 0;

    // Miss grace period (easy mode only): counts down each time RegisterMiss forgives a miss.
    int m_easyGraceRemaining = c_EasyGraceMisses;

    bool m_passing = false;
    bool m_hasPendingAdvance = false;
    double m_pendingAdvanceAtSeconds = 0.0;

    double m_nextExpectedBeat[c_LaneCount] = {};
    LaneHold m_laneHolds[c_LaneCount];
    std::vector<JudgedLaneNote> m_judgedNotes;
};
