#include "SectionInstance.h"

#include <cmath>

#include "StreakTracker.h"

SectionInstance::SectionInstance(int sectionIndex, LearnMode mode)
    : m_sectionIndex(sectionIndex), m_mode(mode), m_passing(mode == LearnMode::DontFail)
{
}

void SectionInstance::SchedulePendingAdvance(double atSeconds)
{
    m_pendingAdvanceAtSeconds = atSeconds;
    m_hasPendingAdvance = true;
}

void SectionInstance::ExtendPendingAdvance(double byStemDurationSeconds)
{
    m_pendingAdvanceAtSeconds += byStemDurationSeconds;
}

void SectionInstance::ClearPendingAdvance()
{
    m_hasPendingAdvance = false;
}

double SectionInstance::NextExpectedBeatForLane(int lane) const
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return 0.0;
    }
    return m_nextExpectedBeat[lane];
}

void SectionInstance::SetNextExpectedBeat(int lane, double beat)
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return;
    }
    m_nextExpectedBeat[lane] = beat;
}

void SectionInstance::AdvanceExpectedNote(int lane, double originBeat, const ChartClip& clip)
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return;
    }
    m_nextExpectedBeat[lane] = clip.NextOnsetAfter(originBeat, m_nextExpectedBeat[lane], lane);
}

bool SectionInstance::IsLaneHeld(int lane) const
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return false;
    }
    return m_laneHolds[lane].active;
}

double SectionInstance::LaneHoldStartBeat(int lane) const
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return -1.0;
    }
    return m_laneHolds[lane].startBeat;
}

double SectionInstance::LaneHoldExpectedEndBeat(int lane) const
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return -1.0;
    }
    return m_laneHolds[lane].expectedEndBeat;
}

bool SectionInstance::LaneHoldWasPrecise(int lane) const
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return true;
    }
    return m_laneHolds[lane].wasPrecise;
}

void SectionInstance::StartLaneHold(int lane, double startBeat, double expectedEndBeat, bool wasPrecise)
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return;
    }
    m_laneHolds[lane] = LaneHold{true, startBeat, expectedEndBeat, wasPrecise};
}

void SectionInstance::ClearLaneHold(int lane)
{
    if (lane < 0 || lane >= c_LaneCount)
    {
        return;
    }
    m_laneHolds[lane].active = false;
}

bool SectionInstance::RegisterHit(int hitsRequired, StreakTracker& streakTracker)
{
    streakTracker.RegisterHit();

    if (m_passing)
    {
        return false;
    }
    m_streak++;
    if (m_streak >= hitsRequired)
    {
        m_passing = true;
        return true;
    }
    return false;
}

SectionInstance::MissResult SectionInstance::RegisterMiss(bool easyMode, StreakTracker& streakTracker)
{
    MissResult result;

    if (m_passing && m_mode == LearnMode::Pass)
    {
        result.wasNoOpAlreadyPassing = true;
        return result;
    }

    if (easyMode && m_easyGraceRemaining > 0)
    {
        --m_easyGraceRemaining;
        return result;
    }

    bool wasPassing = m_passing;
    m_streak = 0;
    result.shouldStopClip = streakTracker.RegisterMiss();

    if (wasPassing)
    {
        // Only reachable in DontFail mode - Pass mode already returned above.
        m_passing = false;
        result.justEnteredFailState = true;
    }

    return result;
}

JudgementResult SectionInstance::OnsetJudgement(double startBeat, int lane) const
{
    for (const JudgedLaneNote& judged : m_judgedNotes)
    {
        if (judged.lane == lane && std::abs(judged.beat - startBeat) < 1e-6)
        {
            return judged.result;
        }
    }
    return JudgementResult::None;
}

bool SectionInstance::OnsetPrecise(double startBeat, int lane) const
{
    for (const JudgedLaneNote& judged : m_judgedNotes)
    {
        if (judged.lane == lane && std::abs(judged.beat - startBeat) < 1e-6)
        {
            return judged.precise;
        }
    }
    return true;
}

void SectionInstance::RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise)
{
    m_judgedNotes.push_back({startBeat, lane, result, precise});

    constexpr size_t c_MaxTracked = 32;
    if (m_judgedNotes.size() > c_MaxTracked)
    {
        m_judgedNotes.erase(m_judgedNotes.begin());
    }
}
