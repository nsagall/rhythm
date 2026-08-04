#include "SectionInstance.h"

#include <cmath>

#include "ChartTiming.h"

namespace
{
constexpr int kMaxConsecutiveMisses = 3;
} // namespace

// Binds this instance to sectionIndex and mode; every other field starts at
// its own "nothing has happened yet" default, except m_passing itself -
// DontFail mode starts already passing (see the header's own comment).
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
    if (lane < 0 || lane >= kLaneCount)
    {
        return 0.0;
    }
    return m_nextExpectedBeat[lane];
}

void SectionInstance::SetNextExpectedBeat(int lane, double beat)
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return;
    }
    m_nextExpectedBeat[lane] = beat;
}

void SectionInstance::AdvanceExpectedNote(int lane, double originBeat, const ChartClip& clip)
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return;
    }
    m_nextExpectedBeat[lane] = ChartTiming::NextOnsetAfter(originBeat, m_nextExpectedBeat[lane], clip, lane);
}

bool SectionInstance::IsLaneHeld(int lane) const
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return false;
    }
    return m_laneHolds[lane].active;
}

double SectionInstance::LaneHoldStartBeat(int lane) const
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return -1.0;
    }
    return m_laneHolds[lane].startBeat;
}

double SectionInstance::LaneHoldExpectedEndBeat(int lane) const
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return -1.0;
    }
    return m_laneHolds[lane].expectedEndBeat;
}

void SectionInstance::StartLaneHold(int lane, double startBeat, double expectedEndBeat)
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return;
    }
    m_laneHolds[lane] = LaneHold{true, startBeat, expectedEndBeat};
}

void SectionInstance::ClearLaneHold(int lane)
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return;
    }
    m_laneHolds[lane].active = false;
}

bool SectionInstance::RegisterHit(int hitsRequired)
{
    if (m_passing)
    {
        return false;
    }
    m_streak++;
    m_consecutiveMisses = 0;
    if (m_streak >= hitsRequired)
    {
        m_passing = true;
        return true;
    }
    return false;
}

SectionInstance::MissResult SectionInstance::RegisterMiss(bool easyMode)
{
    MissResult result;

    if (m_passing && m_mode == LearnMode::Pass)
    {
        return result;
    }

    if (easyMode && m_easyGraceAvailable)
    {
        m_easyGraceAvailable = false;
        return result;
    }

    bool wasPassing = m_passing;
    m_streak = 0;
    m_consecutiveMisses++;
    result.shouldStopClip = m_consecutiveMisses >= kMaxConsecutiveMisses;

    if (wasPassing)
    {
        // Only reachable in DontFail mode - Pass mode already returned
        // above when m_passing was true.
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

void SectionInstance::RecordOnsetJudgement(double startBeat, int lane, JudgementResult result)
{
    m_judgedNotes.push_back({startBeat, lane, result});

    constexpr size_t kMaxTracked = 32;
    if (m_judgedNotes.size() > kMaxTracked)
    {
        m_judgedNotes.erase(m_judgedNotes.begin());
    }
}
