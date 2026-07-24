#include "GameSession.h"

#include <cmath>

namespace
{

constexpr double kCountInSeconds = 2.0;
constexpr int kMaxConsecutiveMisses = 3;

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

    std::vector<int> stemHandles;
    for (const ChartInstrument& instrument : song.instruments)
    {
        int handle = m_audioEngine.LoadStem(instrument.wavFilePath);
        if (handle < 0)
        {
            return false;
        }
        stemHandles.push_back(handle);
    }

    m_song = std::move(song);
    m_stemHandles = std::move(stemHandles);
    m_phase = GamePhase::Idle;
    m_currentInstrumentIndex = -1;
    m_streak = 0;
    m_consecutiveMisses = 0;
    m_loopIsPlaying = false;
    m_hasPendingAdvance = false;
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
    m_consecutiveMisses = 0;
    m_loopIsPlaying = false;
    m_hasPendingAdvance = false;
    m_lastJudgement = JudgementResult::None;
}

// Registers a tap at the current moment; judges it if an instrument is still being learned.
void GameSession::OnTap()
{
    if (m_phase != GamePhase::Learning || m_hasPendingAdvance)
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
        RegisterHit();
        m_lastJudgement = JudgementResult::Hit;
        AdvanceExpectedOnset();

        if (m_streak >= instrument.hitsRequired)
        {
            SchedulePendingAdvance();
        }
    }
    else
    {
        RegisterMiss();
        m_lastJudgement = JudgementResult::Miss;
    }
}

// Advances count-in/miss-detection timing; call once per frame.
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
        if (m_hasPendingAdvance)
        {
            if (m_clock.ElapsedSeconds() >= m_pendingAdvanceAtSeconds)
            {
                m_hasPendingAdvance = false;

                const ChartInstrument& finishedInstrument = m_song.instruments[m_currentInstrumentIndex];
                m_audioEngine.SetVolume(m_stemHandles[m_currentInstrumentIndex], static_cast<float>(finishedInstrument.volume));

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

        const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
        double secondsPerBeat = 60.0 / m_song.bpm;
        double onsetSeconds = m_nextExpectedOnsetBeat * secondsPerBeat;
        double toleranceSeconds = instrument.toleranceMs / 1000.0;

        if (m_clock.ElapsedSeconds() > onsetSeconds + toleranceSeconds)
        {
            RegisterMiss();
            m_lastJudgement = JudgementResult::Miss;
            AdvanceExpectedOnset();
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

// Returns the instrument currently being learned, or nullptr if none.
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
    m_consecutiveMisses = 0;
    m_loopIsPlaying = false;
    m_hasPendingAdvance = false;
    m_phase = GamePhase::Learning;

    const ChartInstrument& instrument = m_song.instruments[instrumentIndex];
    m_nextExpectedOnsetBeat = NextOnsetAfter(m_clock.BeatPosition() - 1e-6, instrument);
}

// Called once the current instrument's streak requirement is met: schedules the advance to the
// next instrument (or Complete) for the next time the current instrument's stem wraps back to
// the start of a playthrough, using the stem's own measured duration as the loop length.
void GameSession::SchedulePendingAdvance()
{
    double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[m_currentInstrumentIndex]);
    double nowSeconds = m_clock.ElapsedSeconds();

    if (stemDuration <= 0.0)
    {
        m_pendingAdvanceAtSeconds = nowSeconds;
    }
    else
    {
        m_pendingAdvanceAtSeconds = std::ceil(nowSeconds / stemDuration) * stemDuration;
    }
    m_hasPendingAdvance = true;
}

// Records a hit: advances the streak, resets the miss counter, and starts this instrument's loop (phase-aligned) if it isn't already playing.
void GameSession::RegisterHit()
{
    m_streak++;
    m_consecutiveMisses = 0;

    if (!m_loopIsPlaying)
    {
        const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
        int handle = m_stemHandles[m_currentInstrumentIndex];
        double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
        double phaseSeconds = stemDuration > 0.0 ? std::fmod(m_clock.ElapsedSeconds(), stemDuration) : 0.0;
        m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(instrument.initVolume));
        m_loopIsPlaying = true;
    }
}

// Records a miss: resets the streak, and stops this instrument's loop after 3 in a row.
void GameSession::RegisterMiss()
{
    m_streak = 0;
    m_consecutiveMisses++;

    if (m_consecutiveMisses >= kMaxConsecutiveMisses && m_loopIsPlaying)
    {
        m_audioEngine.Stop(m_stemHandles[m_currentInstrumentIndex]);
        m_loopIsPlaying = false;
    }
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
