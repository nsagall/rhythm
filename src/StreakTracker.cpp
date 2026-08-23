#include "StreakTracker.h"

void StreakTracker::RegisterHit()
{
    ++m_streak;
    m_consecutiveMisses = 0;
}

bool StreakTracker::RegisterMiss()
{
    ++m_consecutiveMisses;
    if (m_consecutiveMisses >= c_MaxConsecutiveMisses)
    {
        m_streak = 0;
        m_consecutiveMisses = 0;
        return true;
    }
    return false;
}

void StreakTracker::Reset()
{
    m_streak = 0;
    m_consecutiveMisses = 0;
}
