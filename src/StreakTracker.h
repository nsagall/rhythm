#pragma once

// Tracks one running "how many in a row" streak against one shared trip
// rule - c_MaxConsecutiveMisses misses in a row resets everything back to 0
// and reports it. This is the single piece of logic behind two otherwise-
// independent decisions that must never disagree: SectionInstance::
// RegisterMiss's "should the current section's clip stop playing" (a
// per-section question, but fed by the same shared instance - see
// GameSession::m_streakTracker's own comment for why it's owned there, not
// per-section) and GameSession's own scoring streak/multiplier (see
// GameSession::MultiplierForStreak). A single miss - or even two, so long
// as a hit lands before the third - never resets the streak on its own;
// only reaching c_MaxConsecutiveMisses does, at which point both the streak
// and the miss count reset together. GameSession::Update() also calls
// Reset() directly, on the unrelated occasion a section's bank actually
// pays out - see its own banking comment for why that's a second, distinct
// reset trigger from the trip above.
class StreakTracker
{
public:
    static constexpr int c_MaxConsecutiveMisses = 3;

    int Streak() const
    {
        return m_streak;
    }

    // Extends the streak by one and forgives however many misses had been
    // building toward a trip.
    void RegisterHit();

    // Counts one more miss toward the trip. Returns true exactly when this
    // call reached c_MaxConsecutiveMisses - at which point the streak and the
    // miss count have both already been reset to 0 by this same call, ready
    // to start over from nothing rather than left sitting at the trip
    // threshold.
    bool RegisterMiss();

    // Unconditionally zeroes both the streak and the in-progress miss count
    // - see the class comment for the one caller that isn't the trip inside
    // RegisterMiss itself.
    void Reset();

private:
    int m_streak = 0;
    int m_consecutiveMisses = 0;
};
