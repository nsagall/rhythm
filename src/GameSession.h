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
// Each instrument's loop starts playing (phase-aligned to the beat grid,
// at the chart's init_volume) on the player's first correct tap, stops
// after 3 consecutive misses, and once a streak of accurate taps reaches
// the chart's threshold, that instrument locks in (its loop just keeps
// playing). The next instrument isn't introduced immediately - it waits
// until the locked-in instrument's stem file completes its current
// playthrough (plus any chart-declared outro_loops extra repeats) and
// wraps back to its start, so new instruments only ever join at the
// beginning of a loop; at that same moment the instrument that just
// locked in switches from init_volume to its chart volume. An instrument
// with a chart-declared intro_bars instead starts playing automatically
// (no tap needed) and holds off judging/dots until that many bars pass.
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

    // Returns the audio engine stem handle for an instrument, for debugging.
    StemHandle DebugStemHandle(int instrumentIndex) const;

    // Returns and clears the most recent judgement (Hit/Miss/None).
    JudgementResult ConsumeLastJudgement();

    // Returns how a specific pattern onset (in absolute beats) was judged,
    // for the note lane to color dots once they've passed the line.
    // Returns None if that onset hasn't been judged yet, or is too old to
    // still be tracked.
    JudgementResult OnsetJudgement(double onsetBeat) const;

    // True once the current instrument has locked in and is just waiting
    // for its stem to reach a loop boundary before the next one is
    // introduced - taps/dots should stop for it during this window.
    bool IsAwaitingAdvance() const;

    // True while the current instrument's chart-declared intro_bars are
    // still playing automatically, before dots/judging begin.
    bool IsInIntro() const;

private:
    // Begins (or resumes) learning the instrument at the given index.
    void BeginLearning(int instrumentIndex);

    // Records a hit: advances the streak, resets the miss counter, and
    // starts this instrument's loop (phase-aligned, at init_volume) if it isn't already playing.
    void RegisterHit();

    // Starts the current instrument's loop now (phase-aligned to the beat
    // grid, at init_volume) if it isn't already playing. Shared by
    // RegisterHit (starts on the first correct tap) and BeginLearning's
    // intro_bars path (starts automatically, no tap needed).
    void StartCurrentInstrumentLoop();

    // Records a miss: resets the streak, and stops this instrument's loop after 3 in a row.
    void RegisterMiss();

    // Moves m_nextExpectedOnsetBeat forward to the next onset after it.
    void AdvanceExpectedOnset();

    // Returns the smallest pattern onset (in absolute beats) strictly after afterBeat.
    double NextOnsetAfter(double afterBeat, const ChartInstrument& instrument) const;

    // If the instrument's declared span is shorter than its stem's actual duration,
    // tiles the pattern to fill the whole clip and widens spanBeats to match.
    static void ExpandPatternToFillClip(ChartInstrument& instrument, double stemDurationSeconds, double bpm);

    // Called once the current instrument's streak requirement is met: schedules
    // the advance to the next instrument (or Complete), and the switch from
    // init_volume to volume, for the next time the current instrument's stem
    // wraps back to the start of a playthrough.
    void SchedulePendingAdvance();

    // Records a judgement for a specific onset, for OnsetJudgement() to look
    // up later. Trims old entries so this can't grow unbounded.
    void RecordOnsetJudgement(double onsetBeat, JudgementResult result);

    AudioEngine& m_audioEngine;
    ChartSong m_song;
    std::vector<StemHandle> m_stemHandles; // one full-loop stem per instrument

    struct JudgedOnset
    {
        double beat = 0.0;
        JudgementResult result = JudgementResult::None;
    };
    std::vector<JudgedOnset> m_judgedOnsets;

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

    // Set while the current instrument's chart-declared intro_bars are
    // still playing automatically; taps/misses aren't judged until
    // m_introEndSeconds is reached.
    bool m_isInIntro = false;
    double m_introEndSeconds = 0.0;
};
