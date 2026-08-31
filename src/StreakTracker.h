#pragma once

// Tracks one running "how many in a row" streak against one shared trip rule: c_MaxConsecutiveMisses
// misses in a row resets everything to 0. This single instance backs two decisions that must never
// disagree - SectionInstance::RegisterMiss's "should the clip stop" and GameSession's scoring
// streak/multiplier. A single miss (or two, if a hit lands before the third) never resets the
// streak; only the trip does, resetting both counters together. GameSession::Update() also calls
// Reset() directly when a section's bank pays out.
class StreakTracker
{
public:
    static constexpr int c_MaxConsecutiveMisses = 3;

    int Streak() const
    {
        return m_streak;
    }

    // Extends the streak by one and forgives any misses building toward a trip.
    void RegisterHit();

    // Counts one more miss toward the trip. Returns true when this call reaches
    // c_MaxConsecutiveMisses, at which point both counters have already been reset to 0.
    bool RegisterMiss();

    // Unconditionally zeroes both the streak and the in-progress miss count.
    void Reset();

private:
    int m_streak = 0;
    int m_consecutiveMisses = 0;
};
