#pragma once

#include <string>
#include <vector>

#include "AudioEngine.h"
#include "ChartFile.h"
#include "SongClock.h"

// The stages a game session moves through, in order, once per song.
enum class GamePhase
{
    Idle,
    CountIn,
    Learning,
    Complete,
};

// Result of the most recently judged tap or missed onset, for the UI to
// flash the note lane. Cleared once read via ConsumeLastJudgement().
enum class JudgementResult
{
    None,
    Hit,
    Miss,
};

// Drives the progressive "learn one instrument at a time" gameplay loop:
// judges taps against the current instrument's pattern (via SongClock).
// Each instrument's loop starts playing (phase-aligned to the beat grid)
// on the player's first correct tap, stops after 3 consecutive misses,
// and once a streak of accurate taps reaches the chart's threshold, that
// instrument locks in (its loop just keeps playing). The next instrument
// isn't introduced immediately - it waits until the locked-in instrument's
// stem file completes its current playthrough and wraps back to its
// start, so new instruments only ever join at the beginning of a loop.
// UI-agnostic - knows nothing about HWNDs or input devices.
class GameSession
{
public:
    explicit GameSession(AudioEngine& audioEngine);

    // Parses a chart and loads all its stems into the audio engine. Returns
    // false if the chart or any of its stems can't be loaded.
    bool LoadChart(const std::wstring& chartFilePath);

    // Starts gameplay from the beginning of the loaded chart.
    void Start();

    // Stops all playback and returns to Idle.
    void Stop();

    // Registers a tap at the current moment; judges it if a phase is being learned.
    void OnTap();

    // Advances count-in/miss-detection timing; call once per frame.
    void Update();

    GamePhase Phase() const;
    const ChartSong& Song() const;
    int CurrentInstrumentIndex() const;
    const ChartInstrument* CurrentInstrument() const;
    int CurrentStreak() const;
    double NextExpectedOnsetBeat() const;
    const SongClock& Clock() const;

    // Returns and clears the most recent judgement (Hit/Miss/None).
    JudgementResult ConsumeLastJudgement();

private:
    // Begins (or resumes) learning the instrument at the given index.
    void BeginLearning(int instrumentIndex);

    // Records a hit: advances the streak, resets the miss counter, and
    // starts this instrument's loop (phase-aligned) if it isn't already playing.
    void RegisterHit();

    // Records a miss: resets the streak, and stops this instrument's loop after 3 in a row.
    void RegisterMiss();

    // Moves m_nextExpectedOnsetBeat forward to the next onset after it.
    void AdvanceExpectedOnset();

    // Returns the smallest pattern onset (in absolute beats) strictly after afterBeat.
    double NextOnsetAfter(double afterBeat, const ChartInstrument& instrument) const;

    // Called once the current instrument's streak requirement is met: schedules
    // the advance to the next instrument (or Complete) for the next time the
    // current instrument's stem wraps back to the start of a playthrough.
    void SchedulePendingAdvance();

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<int> m_stemHandles; // one full-loop stem per instrument

    SongClock m_clock;
    GamePhase m_phase = GamePhase::Idle;
    int m_currentInstrumentIndex = -1;
    int m_streak = 0;
    int m_consecutiveMisses = 0;
    bool m_loopIsPlaying = false;
    double m_nextExpectedOnsetBeat = 0.0;
    JudgementResult m_lastJudgement = JudgementResult::None;

    // Set once the current instrument's hit requirement is met; taps/misses
    // stop being judged for it and it advances at m_pendingAdvanceAtSeconds.
    bool m_hasPendingAdvance = false;
    double m_pendingAdvanceAtSeconds = 0.0;
};
