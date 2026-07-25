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

    std::vector<StemHandle> stemHandles;
    for (ChartInstrument& instrument : song.instruments)
    {
        StemHandle handle = m_audioEngine.LoadStem(instrument.wavFilePath);
        if (!handle.IsValid())
        {
            return false;
        }
        stemHandles.push_back(handle);

        double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
        ExpandPatternToFillClip(instrument, stemDuration, song.bpm);
    }

    m_song = std::move(song);
    m_stemHandles = std::move(stemHandles);
    m_phase = GamePhase::Idle;
    m_currentInstrumentIndex = -1;
    m_streak = 0;
    m_consecutiveMisses = 0;
    m_loopIsPlaying = false;
    m_hasPendingAdvance = false;
    m_isInIntro = false;
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
    m_isInIntro = false;
    m_lastJudgement = JudgementResult::None;
}

// Registers a tap at the current moment; judges it if an instrument is still being learned.
void GameSession::OnTap()
{
    if (m_phase != GamePhase::Learning || m_hasPendingAdvance || m_isInIntro)
    {
        return;
    }

    const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
    double secondsPerBeat = 60.0 / m_song.bpm;
    double onsetSeconds = m_nextExpectedOnsetBeat * secondsPerBeat;
    double toleranceSeconds = instrument.toleranceMs / 1000.0;
    double nowSeconds = m_clock.ElapsedSeconds();

    double judgedOnsetBeat = m_nextExpectedOnsetBeat;

    if (std::abs(nowSeconds - onsetSeconds) <= toleranceSeconds)
    {
        RegisterHit();
        m_lastJudgement = JudgementResult::Hit;
        RecordOnsetJudgement(judgedOnsetBeat, JudgementResult::Hit);
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
        RecordOnsetJudgement(judgedOnsetBeat, JudgementResult::Miss);
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
        if (m_isInIntro)
        {
            if (m_clock.ElapsedSeconds() >= m_introEndSeconds)
            {
                m_isInIntro = false;
                const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
                m_nextExpectedOnsetBeat = NextOnsetAfter(m_clock.BeatPosition() - 1e-6, instrument);
            }
            return;
        }

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
            RecordOnsetJudgement(m_nextExpectedOnsetBeat, JudgementResult::Miss);
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

// Returns the audio engine stem handle for an instrument, for debugging.
StemHandle GameSession::DebugStemHandle(int instrumentIndex) const
{
    if (instrumentIndex < 0 || instrumentIndex >= static_cast<int>(m_stemHandles.size()))
    {
        return StemHandle{};
    }
    return m_stemHandles[instrumentIndex];
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

bool GameSession::IsAwaitingAdvance() const
{
    return m_hasPendingAdvance;
}

bool GameSession::IsInIntro() const
{
    return m_isInIntro;
}

// Returns and clears the most recent judgement (Hit/Miss/None).
JudgementResult GameSession::ConsumeLastJudgement()
{
    JudgementResult result = m_lastJudgement;
    m_lastJudgement = JudgementResult::None;
    return result;
}

// Returns how a specific pattern onset was judged, or None if untracked.
JudgementResult GameSession::OnsetJudgement(double onsetBeat) const
{
    for (const JudgedOnset& judged : m_judgedOnsets)
    {
        if (std::abs(judged.beat - onsetBeat) < 1e-6)
        {
            return judged.result;
        }
    }
    return JudgementResult::None;
}

// Records a judgement for a specific onset, for OnsetJudgement() to look up later. Trims old entries so this can't grow unbounded.
void GameSession::RecordOnsetJudgement(double onsetBeat, JudgementResult result)
{
    m_judgedOnsets.push_back({onsetBeat, result});

    constexpr size_t kMaxTracked = 8;
    if (m_judgedOnsets.size() > kMaxTracked)
    {
        m_judgedOnsets.erase(m_judgedOnsets.begin());
    }
}

// Begins (or resumes) learning the instrument at the given index.
void GameSession::BeginLearning(int instrumentIndex)
{
    m_currentInstrumentIndex = instrumentIndex;
    m_streak = 0;
    m_consecutiveMisses = 0;
    m_loopIsPlaying = false;
    m_hasPendingAdvance = false;
    m_isInIntro = false;
    m_phase = GamePhase::Learning;
    m_judgedOnsets.clear();

    const ChartInstrument& instrument = m_song.instruments[instrumentIndex];

    if (instrument.introBars > 0)
    {
        StartCurrentInstrumentLoop();
        m_isInIntro = true;
        double secondsPerBeat = 60.0 / m_song.bpm;
        m_introEndSeconds = m_clock.ElapsedSeconds() + instrument.introBars * m_song.beatsPerBar * secondsPerBeat;
        return;
    }

    m_nextExpectedOnsetBeat = NextOnsetAfter(m_clock.BeatPosition() - 1e-6, instrument);
}

// Called once the current instrument's streak requirement is met: schedules the advance to the
// next instrument (or Complete) for the next time the current instrument's stem wraps back to
// the start of a playthrough, using the stem's own measured duration as the loop length.
void GameSession::SchedulePendingAdvance()
{
    const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
    double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[m_currentInstrumentIndex]);
    double nowSeconds = m_clock.ElapsedSeconds();

    if (stemDuration <= 0.0)
    {
        m_pendingAdvanceAtSeconds = nowSeconds;
    }
    else
    {
        int extraLoops = std::max(instrument.outroLoops, 0);
        m_pendingAdvanceAtSeconds = std::ceil(nowSeconds / stemDuration) * stemDuration + extraLoops * stemDuration;
    }
    m_hasPendingAdvance = true;
}

// Records a hit: advances the streak, resets the miss counter, and starts this instrument's loop (phase-aligned) if it isn't already playing.
void GameSession::RegisterHit()
{
    m_streak++;
    m_consecutiveMisses = 0;
    StartCurrentInstrumentLoop();
}

// Starts the current instrument's loop now (phase-aligned to the beat grid, at init_volume) if it isn't already playing.
void GameSession::StartCurrentInstrumentLoop()
{
    if (m_loopIsPlaying)
    {
        return;
    }

    const ChartInstrument& instrument = m_song.instruments[m_currentInstrumentIndex];
    StemHandle handle = m_stemHandles[m_currentInstrumentIndex];
    double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
    double phaseSeconds = stemDuration > 0.0 ? std::fmod(m_clock.ElapsedSeconds(), stemDuration) : 0.0;
    m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(instrument.initVolume));
    m_loopIsPlaying = true;
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

// If the instrument's declared span is shorter than its stem's actual
// duration, tiles the pattern (repeating it every original span) to fill
// the whole clip, and widens spanBeats to match - so a short authored
// phrase repeats to cover a longer clip instead of leaving the back half
// of every loop silent/ungraded, and the judged pattern's cycle stays in
// sync with what the audio actually repeats.
void GameSession::ExpandPatternToFillClip(ChartInstrument& instrument, double stemDurationSeconds, double bpm)
{
    if (instrument.patternBeats.empty() || instrument.spanBeats <= 0.0 || stemDurationSeconds <= 0.0)
    {
        return;
    }

    double secondsPerBeat = 60.0 / bpm;
    double clipBeats = stemDurationSeconds / secondsPerBeat;
    if (clipBeats <= instrument.spanBeats + 1e-6)
    {
        return;
    }

    std::vector<double> originalPattern = instrument.patternBeats;
    double originalSpan = instrument.spanBeats;

    std::vector<double> expanded;
    for (double repeatStart = 0.0; repeatStart < clipBeats - 1e-9; repeatStart += originalSpan)
    {
        for (double onset : originalPattern)
        {
            double absolute = repeatStart + onset;
            if (absolute < clipBeats - 1e-9)
            {
                expanded.push_back(absolute);
            }
        }
    }

    instrument.patternBeats = std::move(expanded);
    instrument.spanBeats = clipBeats;
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
