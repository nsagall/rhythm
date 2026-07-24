#include "GameSession.h"

#include <cmath>

namespace
{

constexpr double kCountInSeconds = 2.0;

} // namespace

// Binds this session to the audio engine it will drive.
GameSession::GameSession(AudioEngine& audioEngine) : m_audioEngine(audioEngine)
{
}

// Parses a chart and loads all its stems into the audio engine. Returns false if the chart or any of its stems can't be loaded.
bool GameSession::LoadChart(const std::wstring& chartFilePath)
{
    ChartSong song;
    if (!ChartFile::Load(chartFilePath, song))
    {
        return false;
    }

    m_audioEngine.StopAll();

    std::vector<int> hitHandles;
    std::vector<int> loopHandles;
    for (const ChartInstrument& instrument : song.instruments)
    {
        int hitHandle = m_audioEngine.LoadStem(instrument.wavFilePath);
        if (hitHandle < 0)
        {
            return false;
        }
        hitHandles.push_back(hitHandle);

        int loopHandle = m_audioEngine.BuildPatternLoop(hitHandle, instrument.patternBeats, instrument.spanBeats, song.bpm);
        if (loopHandle < 0)
        {
            return false;
        }
        loopHandles.push_back(loopHandle);
    }

    m_song = std::move(song);
    m_hitStemHandles = std::move(hitHandles);
    m_loopStemHandles = std::move(loopHandles);
    m_phase = GamePhase::Idle;
    m_currentInstrumentIndex = -1;
    m_streak = 0;
    m_lastJudgement = JudgementResult::None;
    return true;
}

// Starts gameplay from the beginning of the loaded chart.
void GameSession::Start()
{
    if (m_song.instruments.empty())
    {
        return;
    }
    m_clock.Start(m_song.bpm);
    m_phase = GamePhase::CountIn;
}

// Stops all playback and returns to Idle.
void GameSession::Stop()
{
    m_audioEngine.StopAll();
    m_phase = GamePhase::Idle;
    m_currentInstrumentIndex = -1;
    m_streak = 0;
    m_lastJudgement = JudgementResult::None;
}

// Registers a tap at the current moment; judges it if an instrument is being learned.
void GameSession::OnTap()
{
    if (m_phase != GamePhase::Learning)
    {
        return;
    }

    const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
    double secondsPerBeat = 60.0 / m_song.bpm;
    double onsetSeconds = m_nextExpectedOnsetBeat * secondsPerBeat;
    double toleranceSeconds = instrument.toleranceMs / 1000.0;
    double nowSeconds = m_clock.ElapsedSeconds();

    if (std::abs(nowSeconds - onsetSeconds) <= toleranceSeconds)
    {
        m_streak++;
        m_lastJudgement = JudgementResult::Hit;
        m_audioEngine.PlayOneShot(m_hitStemHandles[m_currentInstrumentIndex]);
        AdvanceExpectedOnset();

        if (m_streak >= instrument.hitsRequired)
        {
            BeginLocking();
        }
    }
    else
    {
        m_streak = 0;
        m_lastJudgement = JudgementResult::Miss;
    }
}

// Advances count-in/miss-detection/lock-in timing; call once per frame.
void GameSession::Update()
{
    if (m_phase == GamePhase::CountIn)
    {
        if (m_clock.ElapsedSeconds() >= kCountInSeconds)
        {
            BeginLearning(0);
        }
        return;
    }

    if (m_phase == GamePhase::Learning)
    {
        const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
        double secondsPerBeat = 60.0 / m_song.bpm;
        double onsetSeconds = m_nextExpectedOnsetBeat * secondsPerBeat;
        double toleranceSeconds = instrument.toleranceMs / 1000.0;

        if (m_clock.ElapsedSeconds() > onsetSeconds + toleranceSeconds)
        {
            m_streak = 0;
            m_lastJudgement = JudgementResult::Miss;
            AdvanceExpectedOnset();
        }
        return;
    }

    if (m_phase == GamePhase::Locking)
    {
        if (m_clock.ElapsedSeconds() >= m_lockTransitionEndSeconds)
        {
            m_audioEngine.StartLooping(m_loopStemHandles[m_currentInstrumentIndex]);

            int nextIndex = m_currentInstrumentIndex + 1;
            if (nextIndex < static_cast<int>(m_song.instruments.size()))
            {
                BeginLearning(nextIndex);
            }
            else
            {
                m_phase = GamePhase::Complete;
            }
        }
        return;
    }
}

GamePhase GameSession::Phase() const
{
    return m_phase;
}

const ChartSong& GameSession::Song() const
{
    return m_song;
}

int GameSession::CurrentInstrumentIndex() const
{
    return m_currentInstrumentIndex;
}

// Returns the instrument currently being learned/locked, or nullptr if none.
const ChartInstrument* GameSession::CurrentInstrument() const
{
    if (m_currentInstrumentIndex < 0 || m_currentInstrumentIndex >= static_cast<int>(m_song.instruments.size()))
    {
        return nullptr;
    }
    return &m_song.instruments[m_currentInstrumentIndex];
}

int GameSession::CurrentStreak() const
{
    return m_streak;
}

double GameSession::NextExpectedOnsetBeat() const
{
    return m_nextExpectedOnsetBeat;
}

const SongClock& GameSession::Clock() const
{
    return m_clock;
}

// Returns and clears the most recent judgement (Hit/Miss/None).
JudgementResult GameSession::ConsumeLastJudgement()
{
    JudgementResult result = m_lastJudgement;
    m_lastJudgement = JudgementResult::None;
    return result;
}

// Begins (or resumes) learning the instrument at the given index.
void GameSession::BeginLearning(int instrumentIndex)
{
    m_currentInstrumentIndex = instrumentIndex;
    m_streak = 0;
    m_phase = GamePhase::Learning;

    const ChartInstrument& instrument = m_song.instruments[instrumentIndex];
    m_nextExpectedOnsetBeat = NextOnsetAfter(m_clock.BeatPosition() - 1e-6, instrument);
}

// Begins the brief transition into the current instrument looping, quantized to the next bar boundary.
void GameSession::BeginLocking()
{
    m_phase = GamePhase::Locking;
    m_lockTransitionEndSeconds = m_clock.ElapsedSeconds() + m_clock.SecondsToNextBar(m_song.beatsPerBar);
}

// Moves m_nextExpectedOnsetBeat forward to the next onset after it.
void GameSession::AdvanceExpectedOnset()
{
    const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
    m_nextExpectedOnsetBeat = NextOnsetAfter(m_nextExpectedOnsetBeat, instrument);
}

// Returns the smallest pattern onset (in absolute beats) strictly after afterBeat.
double GameSession::NextOnsetAfter(double afterBeat, const ChartInstrument& instrument) const
{
    if (instrument.patternBeats.empty())
    {
        return afterBeat + instrument.spanBeats;
    }

    double span = instrument.spanBeats;
    long long barIndex = static_cast<long long>(std::floor(afterBeat / span));

    for (double onset : instrument.patternBeats)
    {
        double candidate = barIndex * span + onset;
        if (candidate > afterBeat + 1e-9)
        {
            return candidate;
        }
    }
    return (barIndex + 1) * span + instrument.patternBeats.front();
}
