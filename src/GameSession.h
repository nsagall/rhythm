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
    Locking,
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
// judges taps against the current instrument's pattern (via SongClock),
// and once a streak of accurate taps is reached, locks that instrument
// into a seamless loop (via AudioEngine) and introduces the next one.
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

    // Advances count-in/miss-detection/lock-in timing; call once per frame.
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

    // Begins the brief transition into the current instrument looping.
    void BeginLocking();

    // Moves m_nextExpectedOnsetBeat forward to the next onset after it.
    void AdvanceExpectedOnset();

    // Returns the smallest pattern onset (in absolute beats) strictly after afterBeat.
    double NextOnsetAfter(double afterBeat, const ChartInstrument& instrument) const;

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<int> m_hitStemHandles; // one raw hit sample per instrument, for PlayOneShot tap feedback
    std::vector<int> m_loopStemHandles; // one synthesized whole-pattern loop per instrument, for StartLooping

    SongClock m_clock;
    GamePhase m_phase = GamePhase::Idle;
    int m_currentInstrumentIndex = -1;
    int m_streak = 0;
    double m_nextExpectedOnsetBeat = 0.0;
    double m_lockTransitionEndSeconds = 0.0;
    JudgementResult m_lastJudgement = JudgementResult::None;
};
