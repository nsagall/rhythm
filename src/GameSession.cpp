#include "GameSession.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace
{

constexpr double kCountInSeconds = 2.0;
constexpr int kMaxConsecutiveMisses = 3;

// Easy mode's start-tolerance widening - see EffectiveStartToleranceSeconds.
// The chart's own declared tolerance is widened by this much unconditionally...
constexpr double kEasyModeToleranceMultiplier = 1.5;
// ...and, on top of that, by this much more while the clip isn't currently
// playing (never started, or stopped after too many misses), to help the
// player get back on track.
constexpr double kEasyModeStoppedToleranceMultiplier = 2.0;

// How long an easy-mode note lasts, in beats - half the quarter-note grid
// it's quantized to, so consecutive notes always leave a visible gap
// instead of running into each other.
constexpr double kEasyModeNoteDurationBeats = 0.5;

// How much a MIDI pattern's declared length is allowed to exceed its
// stem's measured audio length before LoadChart rejects it as not fitting
// in one loop. A real stem's duration (sample count / sample rate) will
// essentially never land exactly on a beat-derived value, so this has to
// be generous enough to absorb ordinary export/rounding slop rather than
// rejecting legitimately-fitting content over a couple of milliseconds.
constexpr double kClipLengthToleranceSeconds = 0.1;

// How far the judging clock (a free-running QueryPerformanceCounter timer)
// is allowed to drift from the actual audio hardware's playback position
// before Update() pulls it back in line - see the resync block there. Small
// enough to catch real drift promptly, but larger than the normal
// quantization noise from GetPositionSeconds() only advancing once per
// XAudio2 processing pass (~a few ms), so a perfectly healthy clock doesn't
// get re-anchored (and its otherwise-smooth motion subtly stair-stepped)
// every single frame over nothing.
constexpr double kClockResyncThresholdSeconds = 0.008;

} // namespace

// Binds this session to the audio engine it will drive.
GameSession::GameSession(AudioEngine& audioEngine) : m_audioEngine(audioEngine)
{
}

// Parses and validates a chart and loads all its clips' stems into the
// audio engine. Returns false if the chart fails validation or any of its
// stems can't be loaded, with outError describing every problem found.
bool GameSession::LoadChart(const std::wstring& chartFilePath, bool easyMode, std::wstring& outError)
{
    ChartSong song;
    std::vector<std::wstring> errors;
    if (!ChartFile::Load(chartFilePath, song, errors))
    {
        outError.clear();
        for (const std::wstring& error : errors)
        {
            if (!outError.empty())
            {
                outError += L"\r\n";
            }
            outError += error;
        }
        return false;
    }

    m_audioEngine.StopAll();

    std::vector<StemHandle> stemHandles;
    for (ChartClip& clip : song.clips)
    {
        StemHandle handle = m_audioEngine.LoadStem(clip.wavFilePath);
        if (!handle.IsValid())
        {
            outError = L"clip '" + clip.name + L"': file '" + clip.wavFilePath +
                       L"' could not be loaded as audio (unsupported or corrupt WAV format)";
            return false;
        }
        stemHandles.push_back(handle);

        // A clip with no .mid file (hasMidi == false) has no pattern to
        // validate against the stem's length or tile to fill it - it's
        // only ever played back whole (solo/background), never judged.
        if (clip.hasMidi)
        {
            if (easyMode)
            {
                ApplyEasyModeTransform(clip);
            }

            double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
            double secondsPerBeat = 60.0 / song.bpm;
            double clipBeats = stemDuration / secondsPerBeat;
            double toleranceBeats = kClipLengthToleranceSeconds / secondsPerBeat;
            if (clipBeats < clip.spanBeats - toleranceBeats)
            {
                // The reverse of the "MIDI shorter than the audio" case (which
                // ExpandLaneNotesToFillClip below handles by tiling): here the
                // pattern doesn't even fit in a single loop of the stem, so the
                // audio would already have wrapped back to its start before the
                // pattern's own last notes are reached - notes get judged
                // against a moment the audio isn't actually at anymore. Not a
                // crash, just silently wrong, so it's rejected at load time
                // instead of shipped.
                outError = L"clip '" + clip.name + L"': its MIDI pattern (" + std::to_wstring(clip.spanBeats) +
                           L" beats) is longer than one loop of its audio ('" + clip.wavFilePath + L"', " +
                           std::to_wstring(clipBeats) + L" beats) - trim the MIDI pattern or use a longer audio stem";
                return false;
            }
            ExpandLaneNotesToFillClip(clip, stemDuration, song.bpm);
        }
    }

    m_song = std::move(song);
    m_stemHandles = std::move(stemHandles);
    m_phase = GamePhase::Idle;
    m_currentSectionIndex = -1;
    m_streak = 0;
    m_consecutiveMisses = 0;
    m_easyMode = easyMode;
    m_easyGraceAvailable = false;
    m_clipIsPlaying.assign(m_song.clips.size(), false);
    m_clipLoopStartSeconds.assign(m_song.clips.size(), 0.0);
    m_queuedBackground = QueuedBackground{};
    m_hasPendingAdvance = false;
    m_isInIntro = false;
    m_songHasStarted = false;
    m_lastJudgement = JudgementResult::None;
    m_judgedNotes.clear();
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        m_laneHolds[lane] = LaneHold{};
        m_nextExpectedBeat[lane] = 0.0;
    }
    return true;
}

// Starts gameplay from the beginning of the loaded chart.
void GameSession::Start()
{
    if (m_song.sections.empty())
    {
        return;
    }

    // A previous run may have left clips locked in and still looping
    // (that's by design while a song is in progress/just completed - each
    // locked-in loop keeps playing to build up the full arrangement), so a
    // fresh start has to stop them explicitly or they'd keep playing
    // underneath the new run.
    m_audioEngine.StopAll();
    m_currentSectionIndex = -1;
    m_streak = 0;
    m_consecutiveMisses = 0;
    m_easyGraceAvailable = false;
    std::fill(m_clipIsPlaying.begin(), m_clipIsPlaying.end(), false);
    m_queuedBackground = QueuedBackground{};
    m_hasPendingAdvance = false;
    m_isInIntro = false;
    m_songHasStarted = false;
    m_lastJudgement = JudgementResult::None;
    m_judgedNotes.clear();
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        m_laneHolds[lane] = LaneHold{};
        m_nextExpectedBeat[lane] = 0.0;
    }

    m_clock.Start(m_song.bpm);
    m_phase = GamePhase::CountIn;
}

// Stops all playback and returns to Idle.
void GameSession::Stop()
{
    m_audioEngine.StopAll();
    m_phase = GamePhase::Idle;
    m_currentSectionIndex = -1;
    m_streak = 0;
    m_consecutiveMisses = 0;
    std::fill(m_clipIsPlaying.begin(), m_clipIsPlaying.end(), false);
    m_queuedBackground = QueuedBackground{};
    m_hasPendingAdvance = false;
    m_isInIntro = false;
    m_lastJudgement = JudgementResult::None;
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        m_laneHolds[lane] = LaneHold{};
    }
}

// True exactly when OnPress(lane) would actually judge a press right now -
// mirrors every one of OnPress's own early-return guards, just without any
// of its judging side effects.
bool GameSession::IsLaneJudgeable(int lane) const
{
    if (m_phase != GamePhase::Learning || m_isInIntro)
    {
        return false;
    }
    if (lane < 0 || lane >= kLaneCount || m_currentSectionIndex < 0)
    {
        return false;
    }
    const ChartSection& section = m_song.sections[m_currentSectionIndex];
    if (section.playMode != PlayMode::Learn)
    {
        return false; // solo/background: no judging, ever
    }
    const ChartClip& clip = m_song.clips[section.clipIndex];
    return !clip.laneNotes[lane].empty();
}

// Registers a key-down for lane; judges it against that lane's next expected note if the current section is learning.
void GameSession::OnPress(int lane)
{
    if (!IsLaneJudgeable(lane))
    {
        return;
    }

    const ChartSection& section = m_song.sections[m_currentSectionIndex];
    const ChartClip& clip = m_song.clips[section.clipIndex];

    double secondsPerBeat = 60.0 / m_song.bpm;
    double startBeat = m_nextExpectedBeat[lane];
    double startSeconds = startBeat * secondsPerBeat;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip, section.clipIndex);
    double nowSeconds = m_clock.ElapsedSeconds();

    if (std::abs(nowSeconds - startSeconds) <= toleranceSeconds)
    {
        const LaneNote* note = FindLaneNote(clip, lane, startBeat);
        double durationBeats = note ? note->durationBeats : 0.0;

        StartClipLoop(section.clipIndex, clip.initVolume);
        m_laneHolds[lane] = LaneHold{true, startBeat, startBeat + durationBeats};
        AdvanceExpectedNote(lane);

        if (m_easyMode)
        {
            // Release timing is ignored entirely in easy mode, so the press
            // itself is the final judgement - mirrors OnRelease's
            // in-tolerance branch below, the only other place a Hit gets
            // registered.
            RegisterHit();
            m_lastJudgement = JudgementResult::Hit;
            RecordOnsetJudgement(startBeat, lane, JudgementResult::Hit);
            if (m_streak >= clip.hitsRequired && !m_hasPendingAdvance)
            {
                SchedulePendingAdvance();
            }
        }
        else
        {
            // A correct press doesn't produce a final judgement yet - that
            // only happens at release - so any stale Hit/Miss left over
            // from an earlier press/release must be cleared here, or the
            // caller's very next ConsumeLastJudgement() would misattribute
            // it to this press.
            m_lastJudgement = JudgementResult::None;
        }
    }
    else
    {
        // Mistimed: fails immediately, but doesn't advance - this lane keeps
        // awaiting the same note until it's hit correctly or times out,
        // exactly like a mistimed tap did in the single-lane model.
        RegisterMiss();
        m_lastJudgement = JudgementResult::Miss;
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
    }
}

// Registers a key-up for lane; judges it against the note that lane was holding, if any.
void GameSession::OnRelease(int lane)
{
    if (lane < 0 || lane >= kLaneCount || !m_laneHolds[lane].active || m_currentSectionIndex < 0)
    {
        return;
    }

    const ChartSection& section = m_song.sections[m_currentSectionIndex];
    if (section.playMode != PlayMode::Learn)
    {
        return; // structurally shouldn't happen (holds only populate in Learn), kept as defense-in-depth
    }

    if (m_easyMode)
    {
        // Already judged Hit at press time - release timing is ignored
        // entirely, so releasing (whenever it happens) just lets go of the
        // hold and produces no judgement of its own.
        m_laneHolds[lane].active = false;
        m_lastJudgement = JudgementResult::None;
        return;
    }

    const ChartClip& clip = m_song.clips[section.clipIndex];
    double secondsPerBeat = 60.0 / m_song.bpm;
    double endSeconds = m_laneHolds[lane].expectedEndBeat * secondsPerBeat;
    double toleranceSeconds = clip.releaseToleranceMs / 1000.0;
    double nowSeconds = m_clock.ElapsedSeconds();
    double startBeat = m_laneHolds[lane].startBeat;

    if (std::abs(nowSeconds - endSeconds) <= toleranceSeconds)
    {
        RegisterHit();
        m_lastJudgement = JudgementResult::Hit;
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Hit);
        if (m_streak >= clip.hitsRequired && !m_hasPendingAdvance)
        {
            SchedulePendingAdvance();
        }
    }
    else
    {
        RegisterMiss();
        m_lastJudgement = JudgementResult::Miss;
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
    }

    m_laneHolds[lane].active = false;
}

// Advances count-in/miss-detection/hold-timeout timing; call once per frame.
void GameSession::Update()
{
    // Keep the judging clock locked to actual audio playback. m_clock is a
    // free-running CPU timer, while the music is actually driven by
    // XAudio2's own hardware clock - the two can slowly drift apart over a
    // long-looping clip (different oscillators, buffer scheduling, etc.),
    // which would otherwise show up as hit judging (and the falling notes)
    // slipping out of sync with what's actually audible. Every currently-
    // playing clip is started phase-aligned to the same shared beat grid
    // (see StartClipLoop), so the current section's own clip - if it has
    // one and it's already playing, true for Learn and Solo alike - is an
    // equally valid drift reference regardless of which section is active.
    // Also requires AudioEngine::IsPlaying(): a solo section's clip plays a
    // *finite* loop_count, and once that last pass finishes XAudio2 stops
    // the voice on its own - m_clipIsPlaying stays true until the section's
    // own scheduled advance explicitly stops it (below), but
    // GetPositionSeconds() has already frozen at that point instead of
    // still advancing. Without this check, resyncing against that frozen
    // position every tick pins the clock to that one instant forever, so
    // "now" can never reach the scheduled advance time - the whole game
    // hangs the moment a solo section's clip finishes playing.
    if (m_currentSectionIndex >= 0)
    {
        int clipIndex = m_song.sections[m_currentSectionIndex].clipIndex;
        if (clipIndex >= 0 && m_clipIsPlaying[clipIndex] && m_audioEngine.IsPlaying(m_stemHandles[clipIndex]))
        {
            double audioElapsed =
                m_clipLoopStartSeconds[clipIndex] + m_audioEngine.GetPositionSeconds(m_stemHandles[clipIndex]);
            if (std::abs(audioElapsed - m_clock.ElapsedSeconds()) > kClockResyncThresholdSeconds)
            {
                m_clock.Resync(audioElapsed);
            }
        }
    }

    double secondsPerBeat = 60.0 / m_song.bpm;
    double now = m_clock.ElapsedSeconds();

    // Held-past-late-release timeout: deliberately independent of
    // phase/pending-advance, so a hold already in flight keeps resolving
    // even if the section has since locked in, instead of being abandoned
    // mid-air. Lane holds structurally only ever populate during a Learn
    // section, but the play-mode check is kept anyway as defense-in-depth.
    // Skipped entirely in easy mode: a hold there is already judged Hit at
    // press time (see OnPress), so there's nothing left to time out - it
    // just sits active until the real key-up, which OnRelease resolves as
    // a no-op.
    if (m_currentSectionIndex >= 0 && !m_easyMode)
    {
        const ChartSection& heldSection = m_song.sections[m_currentSectionIndex];
        if (heldSection.playMode == PlayMode::Learn)
        {
            const ChartClip& heldClip = m_song.clips[heldSection.clipIndex];
            double toleranceSeconds = heldClip.releaseToleranceMs / 1000.0;
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (!m_laneHolds[lane].active)
                {
                    continue;
                }
                double endSeconds = m_laneHolds[lane].expectedEndBeat * secondsPerBeat;
                if (now > endSeconds + toleranceSeconds)
                {
                    RegisterMiss();
                    m_lastJudgement = JudgementResult::Miss;
                    RecordOnsetJudgement(m_laneHolds[lane].startBeat, lane, JudgementResult::Miss);
                    m_laneHolds[lane].active = false;
                }
            }
        }
    }

    if (m_phase == GamePhase::CountIn)
    {
        if (now >= kCountInSeconds)
        {
            BeginSection(0, kCountInSeconds / secondsPerBeat);
        }
        return;
    }

    if (m_phase == GamePhase::Learning)
    {
        if (m_isInIntro)
        {
            if (now >= m_introEndSeconds)
            {
                m_isInIntro = false;
                const ChartClip& clip = m_song.clips[m_song.sections[m_currentSectionIndex].clipIndex];
                double introEndBeat = m_introEndSeconds / secondsPerBeat - 1e-6;
                if (m_songHasStarted)
                {
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        m_nextExpectedBeat[lane] = NextOnsetAfter(introEndBeat, clip, lane);
                    }
                }
                else
                {
                    FirstReachableOnsetForAllLanes(introEndBeat, clip, m_nextExpectedBeat);
                }
                m_songHasStarted = true;
            }
            return;
        }

        if (m_hasPendingAdvance && now >= m_pendingAdvanceAtSeconds)
        {
            m_hasPendingAdvance = false;

            const ChartSection& finishedSection = m_song.sections[m_currentSectionIndex];
            if (finishedSection.playMode == PlayMode::Learn)
            {
                // Only a learn section's clip switches init_volume ->
                // volume; a solo clip already plays at `volume`
                // throughout (there's no lock-in event to switch on).
                m_audioEngine.SetVolume(m_stemHandles[finishedSection.clipIndex],
                                         static_cast<float>(m_song.clips[finishedSection.clipIndex].volume));
            }
            else if (finishedSection.playMode == PlayMode::Solo)
            {
                // Unlike a locked-in learn clip (which keeps playing by
                // design, to build up the arrangement), a solo section
                // is a one-off scripted interlude - stop it once its own
                // loop_count wait completes so it doesn't drone on
                // underneath every subsequent section until the next
                // solo's StopAll() happens to kill it. Safe no-op for an
                // empty-clip solo (clipIndex == -1, guarded by
                // StopClipLoop itself).
                StopClipLoop(finishedSection.clipIndex);
            }

            int nextIndex = m_currentSectionIndex + 1;
            if (nextIndex < static_cast<int>(m_song.sections.size()))
            {
                BeginSection(nextIndex, m_pendingAdvanceAtSeconds / secondsPerBeat);
            }
            else
            {
                m_phase = GamePhase::Complete;
            }
            return;
        }

        // Press-phase timeout: any lane still awaiting a press whose window
        // has closed. Learn-only. Runs even while m_hasPendingAdvance is
        // true (the post-lock-in extension, before the section actually
        // advances above) - keeps notes timing out/getting judged instead of
        // freezing the instant the streak requirement is met.
        const ChartSection& section = m_song.sections[m_currentSectionIndex];
        if (section.playMode == PlayMode::Learn)
        {
            const ChartClip& clip = m_song.clips[section.clipIndex];
            double toleranceSeconds = EffectiveStartToleranceSeconds(clip, section.clipIndex);
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (clip.laneNotes[lane].empty())
                {
                    continue;
                }
                double onsetSeconds = m_nextExpectedBeat[lane] * secondsPerBeat;
                if (now > onsetSeconds + toleranceSeconds)
                {
                    RegisterMiss();
                    m_lastJudgement = JudgementResult::Miss;
                    RecordOnsetJudgement(m_nextExpectedBeat[lane], lane, JudgementResult::Miss);
                    AdvanceExpectedNote(lane);
                }
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

int GameSession::CurrentSectionIndex() const
{
    return m_currentSectionIndex;
}

// Returns the audio engine stem handle for a clip, for debugging.
StemHandle GameSession::DebugStemHandle(int clipIndex) const
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_stemHandles.size()))
    {
        return StemHandle{};
    }
    return m_stemHandles[clipIndex];
}

// Returns the clip the current section refers to, or nullptr if there's no current section or it's an empty solo.
const ChartClip* GameSession::CurrentClip() const
{
    if (m_currentSectionIndex < 0 || m_currentSectionIndex >= static_cast<int>(m_song.sections.size()))
    {
        return nullptr;
    }
    int clipIndex = m_song.sections[m_currentSectionIndex].clipIndex;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.clips.size()))
    {
        return nullptr;
    }
    return &m_song.clips[clipIndex];
}

// Returns the current section's play mode, or Learn as a harmless default if there's no current section.
PlayMode GameSession::CurrentPlayMode() const
{
    if (m_currentSectionIndex < 0 || m_currentSectionIndex >= static_cast<int>(m_song.sections.size()))
    {
        return PlayMode::Learn;
    }
    return m_song.sections[m_currentSectionIndex].playMode;
}

int GameSession::CurrentStreak() const
{
    return m_streak;
}

// Returns the beat of the next note this lane is awaiting a press for.
double GameSession::NextExpectedBeatForLane(int lane) const
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return 0.0;
    }
    return m_nextExpectedBeat[lane];
}

const SongClock& GameSession::Clock() const
{
    return m_clock;
}

bool GameSession::IsAwaitingAdvance() const
{
    return m_hasPendingAdvance;
}

double GameSession::PendingAdvanceAtSeconds() const
{
    return m_hasPendingAdvance ? m_pendingAdvanceAtSeconds : -1.0;
}

bool GameSession::IsInIntro() const
{
    return m_isInIntro;
}

// Returns the index of the first section at or after startIndex whose play_mode isn't Background, or -1 if none remain.
int GameSession::NextNonBackgroundSectionAtOrAfter(int startIndex) const
{
    for (int i = std::max(startIndex, 0); i < static_cast<int>(m_song.sections.size()); ++i)
    {
        if (m_song.sections[i].playMode != PlayMode::Background)
        {
            return i;
        }
    }
    return -1;
}

const ChartClip* GameSession::PreviewClip() const
{
    // A clip with its own intro_bars hides dots for a while after it goes
    // live too, so there's nothing useful to preview until its own intro
    // tail arrives (handled by the m_isInIntro branch below).
    if (m_phase == GamePhase::CountIn)
    {
        int idx = NextNonBackgroundSectionAtOrAfter(0);
        if (idx < 0 || m_song.sections[idx].playMode != PlayMode::Learn)
        {
            return nullptr;
        }
        const ChartClip& clip = m_song.clips[m_song.sections[idx].clipIndex];
        return clip.introBars > 0 ? nullptr : &clip;
    }
    if (m_phase != GamePhase::Learning || m_currentSectionIndex < 0)
    {
        return nullptr;
    }

    // Applies uniformly whether the current section is Learn-awaiting-
    // advance or Solo-awaiting-advance - both set m_hasPendingAdvance the
    // same way, so a solo section's own hold is the natural place to
    // preview the *next* learn section's dots, exactly like a learn
    // section's own hold already was.
    if (m_isInIntro)
    {
        // Structurally only ever true while current is Learn (set only in
        // BeginSection's Learn case), so CurrentClip() here is unaffected.
        return CurrentClip();
    }
    if (m_hasPendingAdvance)
    {
        int nextIdx = NextNonBackgroundSectionAtOrAfter(m_currentSectionIndex + 1);
        if (nextIdx < 0 || m_song.sections[nextIdx].playMode != PlayMode::Learn)
        {
            return nullptr;
        }
        const ChartClip& next = m_song.clips[m_song.sections[nextIdx].clipIndex];
        return next.introBars > 0 ? nullptr : &next;
    }
    return nullptr;
}

double GameSession::PreviewTransitionSeconds() const
{
    if (m_phase == GamePhase::CountIn)
    {
        return kCountInSeconds;
    }
    if (m_phase == GamePhase::Learning && m_isInIntro)
    {
        return m_introEndSeconds;
    }
    if (m_phase == GamePhase::Learning && m_hasPendingAdvance)
    {
        return m_pendingAdvanceAtSeconds;
    }
    return -1.0;
}

double GameSession::PreviewFirstOnsetBeatForLane(int lane) const
{
    const ChartClip* preview = PreviewClip();
    if (!preview || lane < 0 || lane >= kLaneCount)
    {
        return -1.0;
    }

    double transitionSeconds = PreviewTransitionSeconds();
    double secondsPerBeat = 60.0 / m_song.bpm;
    double transitionBeat = transitionSeconds / secondsPerBeat;

    // During the count-in, m_songHasStarted is always still false - the
    // preview must show the same notes that BeginSection is about to
    // anchor judging to (see FirstReachableOnsetForAllLanes), not wherever
    // NextOnsetAfter's usual per-lane cutoff logic lands relative to the
    // count-in's fixed duration (see m_songHasStarted's own comment).
    if (m_phase == GamePhase::CountIn)
    {
        double allLanes[kLaneCount];
        FirstReachableOnsetForAllLanes(transitionBeat - 1e-6, *preview, allLanes);
        return allLanes[lane];
    }
    return NextOnsetAfter(transitionBeat - 1e-6, *preview, lane);
}

// Returns and clears the most recent judgement (Hit/Miss/None).
JudgementResult GameSession::ConsumeLastJudgement()
{
    JudgementResult result = m_lastJudgement;
    m_lastJudgement = JudgementResult::None;
    return result;
}

// Returns how a specific lane note was judged, or None if untracked.
JudgementResult GameSession::OnsetJudgement(double startBeat, int lane) const
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

bool GameSession::IsLaneHeld(int lane) const
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return false;
    }
    return m_laneHolds[lane].active;
}

double GameSession::LaneHoldStartBeat(int lane) const
{
    if (lane < 0 || lane >= kLaneCount)
    {
        return -1.0;
    }
    return m_laneHolds[lane].startBeat;
}

// Records a judgement for a specific lane note, for OnsetJudgement() to look up later. Trims old entries so this can't grow unbounded.
void GameSession::RecordOnsetJudgement(double startBeat, int lane, JudgementResult result)
{
    m_judgedNotes.push_back({startBeat, lane, result});

    constexpr size_t kMaxTracked = 32;
    if (m_judgedNotes.size() > kMaxTracked)
    {
        m_judgedNotes.erase(m_judgedNotes.begin());
    }
}

// Begins the section at the given index, kicking off any background clip
// queued by the previous section first, then dispatching on this section's
// own play_mode.
void GameSession::BeginSection(int sectionIndex, double scheduledBeat)
{
    m_currentSectionIndex = sectionIndex;
    m_streak = 0;
    m_consecutiveMisses = 0;
    m_easyGraceAvailable = true;
    m_hasPendingAdvance = false;
    m_isInIntro = false;
    m_phase = GamePhase::Learning;
    m_judgedNotes.clear();
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        m_laneHolds[lane] = LaneHold{};
    }

    // "The next section begins" is exactly this call - kick off whatever
    // the previous section (if it was `background`) queued, before this
    // section's own logic runs, so the two play out in parallel from here.
    // Once started, a background clip loops indefinitely - exactly like a
    // locked-in learn clip - until a later `solo` section's StopAll() (or
    // Stop()/Start()) silences it; loop_count has no effect on background
    // sections.
    if (m_queuedBackground.clipIndex >= 0)
    {
        int bgClipIndex = m_queuedBackground.clipIndex;
        m_queuedBackground = QueuedBackground{};
        StartClipLoop(bgClipIndex, m_song.clips[bgClipIndex].volume);
    }

    const ChartSection& section = m_song.sections[sectionIndex];

    switch (section.playMode)
    {
        case PlayMode::Background:
        {
            // Never blocks, never itself occupies "current" for judging
            // purposes - queue its clip for the *next* BeginSection and
            // fall straight through to that next section immediately.
            m_queuedBackground = QueuedBackground{section.clipIndex, section.loopCount};
            int nextIndex = sectionIndex + 1;
            if (nextIndex < static_cast<int>(m_song.sections.size()))
            {
                BeginSection(nextIndex, scheduledBeat);
            }
            else
            {
                // Last section in the chart was `background` - its queued
                // clip simply never starts; that's acceptable, not an error.
                m_phase = GamePhase::Complete;
            }
            return;
        }

        case PlayMode::Solo:
        {
            m_audioEngine.StopAll();
            std::fill(m_clipIsPlaying.begin(), m_clipIsPlaying.end(), false);
            if (section.clipIndex >= 0)
            {
                const ChartClip& clip = m_song.clips[section.clipIndex];
                // A solo's loop_count is already known right now (unlike a
                // learn clip, whose eventual stop time depends on future
                // player input), so it's handed straight to StartClipLoop:
                // the voice stops itself naturally and sample-accurately
                // once its loops are done, instead of relying solely on
                // the polled StopClipLoop() call below to catch the exact
                // instant - which could otherwise let a fraction of a
                // second of the loop's beginning bleed through first,
                // especially audible at the very end of a chart where
                // nothing else is left playing to mask it.
                StartClipLoop(section.clipIndex, clip.volume, std::max(section.loopCount, 1));
                double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
                // Measured from m_clipLoopStartSeconds (exactly what
                // StartClipLoop just recorded), not a fresh clock read here -
                // matches how SchedulePendingAdvance already does this for a
                // learn section's lock-in floor, rather than re-querying the
                // clock a few instructions after the loop's real start time.
                m_pendingAdvanceAtSeconds =
                    ComputeLoopFloorSeconds(m_clipLoopStartSeconds[section.clipIndex], stemDuration, section.loopCount);
            }
            else
            {
                // Empty-clip solo: silence gate - stop everything, advance
                // as soon as the kNoteFallBeats floor below allows.
                m_pendingAdvanceAtSeconds = m_clock.ElapsedSeconds();
            }

            // Same kNoteFallBeats guarantee SchedulePendingAdvance gives a
            // locked-in learn section: without it, a short/empty-clip solo
            // can hand off to the next learn section with its first note's
            // scheduled beat only an instant away, so the note lane's
            // preview lands it already at (or past) the judge line instead
            // of spawning at the top edge with its full travel time.
            double secondsPerBeat = 60.0 / m_song.bpm;
            double tFallSeconds = kNoteFallBeats * secondsPerBeat;
            m_pendingAdvanceAtSeconds =
                std::max(m_pendingAdvanceAtSeconds, m_clock.ElapsedSeconds() + tFallSeconds);

            m_hasPendingAdvance = true;
            return;
        }

        case PlayMode::Learn:
        {
            const ChartClip& clip = m_song.clips[section.clipIndex];
            if (clip.introBars > 0)
            {
                StartClipLoop(section.clipIndex, clip.initVolume);
                m_isInIntro = true;
                double secondsPerBeat = 60.0 / m_song.bpm;
                m_introEndSeconds = m_clock.ElapsedSeconds() + clip.introBars * m_song.beatsPerBar * secondsPerBeat;
                return;
            }
            if (m_songHasStarted)
            {
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    m_nextExpectedBeat[lane] = NextOnsetAfter(scheduledBeat - 1e-6, clip, lane);
                }
            }
            else
            {
                FirstReachableOnsetForAllLanes(scheduledBeat - 1e-6, clip, m_nextExpectedBeat);
            }
            m_songHasStarted = true;
            return;
        }
    }
}

// Computes the wall-clock second at which loopCount full loops have
// completed, counted from loopStartSeconds, given a stem of stemDuration
// seconds. Shared by a learn section's lock-in floor, a solo section's
// unconditional wait, and a background layer's self-stop time.
double GameSession::ComputeLoopFloorSeconds(double loopStartSeconds, double stemDuration, int loopCount)
{
    if (stemDuration <= 0.0)
    {
        return loopStartSeconds;
    }
    int minLoops = std::max(loopCount, 1);
    return std::ceil(loopStartSeconds / stemDuration) * stemDuration + (minLoops - 1) * stemDuration;
}

// Called once the shared streak meets the current learn section's clip
// requirement: schedules the advance to the next section (or Complete) for
// the next time the clip's stem wraps back to the start of a playthrough,
// using the stem's own measured duration as the loop length - extended by
// whole extra loops as needed to satisfy loop_count and/or the
// kNoteFallBeats preview-time guarantee (see below).
void GameSession::SchedulePendingAdvance()
{
    const ChartSection& section = m_song.sections[m_currentSectionIndex];
    const ChartClip& clip = m_song.clips[section.clipIndex];
    double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
    double nowSeconds = m_clock.ElapsedSeconds();
    double secondsPerBeat = 60.0 / m_song.bpm;

    if (stemDuration <= 0.0)
    {
        m_pendingAdvanceAtSeconds = nowSeconds;
    }
    else
    {
        int extraLoops = std::max(clip.outroLoops, 0);
        double naturalAdvance = std::ceil(nowSeconds / stemDuration) * stemDuration + extraLoops * stemDuration;

        // loop_count sets a floor measured from when this clip's loop
        // actually started, independent of how fast the player locked in.
        // loop_count=1 (the default) always resolves to <= naturalAdvance,
        // since locking in can't happen before the loop starts - so it
        // never changes existing behavior.
        double minimumAdvance =
            ComputeLoopFloorSeconds(m_clipLoopStartSeconds[section.clipIndex], stemDuration, section.loopCount);

        m_pendingAdvanceAtSeconds = std::max(naturalAdvance, minimumAdvance);

        // Guarantee at least a full kNoteFallBeats (Tfall - the time a note
        // takes to scroll from the top of the lane to the judge line) of
        // real time between locking in and the next section actually
        // starting. Without this, locking in right before a loop boundary
        // could hand off to the next learn section with its first notes
        // barely (or not at all) previewed - they'd pop in already partway
        // down the lane instead of getting their full on-screen travel
        // time. Extends by whole extra loops rather than an arbitrary
        // pause, so the current clip's audio never gets cut mid-loop.
        double tFallSeconds = kNoteFallBeats * secondsPerBeat;
        while (m_pendingAdvanceAtSeconds - nowSeconds < tFallSeconds)
        {
            m_pendingAdvanceAtSeconds += stemDuration;
        }
    }
    m_hasPendingAdvance = true;
}

// Records a hit: advances the shared streak, resets the shared miss counter, and starts the current section's clip loop (phase-aligned) if it isn't already playing. Once already awaiting advance, the streak/miss counters are left alone (frozen at their lock-in value) since they no longer drive anything.
void GameSession::RegisterHit()
{
    if (!m_hasPendingAdvance)
    {
        m_streak++;
        m_consecutiveMisses = 0;
    }
    const ChartSection& section = m_song.sections[m_currentSectionIndex];
    const ChartClip& clip = m_song.clips[section.clipIndex];
    StartClipLoop(section.clipIndex, clip.initVolume);
}

// Starts clipIndex's stem looping now (phase-aligned to the beat grid, at
// the given volume) if it isn't already playing, and records the start
// time for loop_count to measure from.
void GameSession::StartClipLoop(int clipIndex, double volume, int finiteLoopCount)
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_clipIsPlaying.size()) || m_clipIsPlaying[clipIndex])
    {
        return;
    }

    // Read the clock exactly once and reuse it for both the phase seek and
    // the recorded loop-start time - StartLooping below does real XAudio2
    // work (stop/flush/submit/start/getstate), so reading the clock again
    // afterward would record a loop-start time measurably later than the
    // instant the audio was actually phase-seeked to, biasing every later
    // GetPositionSeconds()-based calculation (loop_count floors, and
    // Update()'s drift resync) forward by however long that call took.
    StemHandle handle = m_stemHandles[clipIndex];
    double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
    double nowSeconds = m_clock.ElapsedSeconds();
    double phaseSeconds = stemDuration > 0.0 ? std::fmod(nowSeconds, stemDuration) : 0.0;
    m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(volume), finiteLoopCount);
    m_clipIsPlaying[clipIndex] = true;
    m_clipLoopStartSeconds[clipIndex] = nowSeconds;
}

// Stops clipIndex's stem if it's playing.
void GameSession::StopClipLoop(int clipIndex)
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_clipIsPlaying.size()) || !m_clipIsPlaying[clipIndex])
    {
        return;
    }
    m_audioEngine.Stop(m_stemHandles[clipIndex]);
    m_clipIsPlaying[clipIndex] = false;
}

// Records a miss: resets the shared streak, and stops the current section's clip loop after 3 in a row. A no-op once already awaiting advance - the track has already locked in, so further misses shouldn't stop it or unfreeze the streak display. In easy mode, the first miss each section is instead fully forgiven (see m_easyGraceAvailable) - streak and consecutive-miss count both left untouched, as if it never happened.
void GameSession::RegisterMiss()
{
    if (m_hasPendingAdvance)
    {
        return;
    }

    if (m_easyMode && m_easyGraceAvailable)
    {
        m_easyGraceAvailable = false;
        return;
    }

    m_streak = 0;
    m_consecutiveMisses++;

    if (m_consecutiveMisses >= kMaxConsecutiveMisses)
    {
        int clipIndex = m_song.sections[m_currentSectionIndex].clipIndex;
        StopClipLoop(clipIndex);
    }
}

// Moves this lane's next-expected-note pointer forward to the next note after it.
void GameSession::AdvanceExpectedNote(int lane)
{
    const ChartSection& section = m_song.sections[m_currentSectionIndex];
    const ChartClip& clip = m_song.clips[section.clipIndex];
    m_nextExpectedBeat[lane] = NextOnsetAfter(m_nextExpectedBeat[lane], clip, lane);
}

// Returns the start-tolerance window (seconds) to judge a press with -
// see the header comment for the easy-mode widening this applies.
double GameSession::EffectiveStartToleranceSeconds(const ChartClip& clip, int clipIndex) const
{
    double toleranceMs = clip.startToleranceMs;
    if (m_easyMode)
    {
        toleranceMs *= kEasyModeToleranceMultiplier;
        bool clipPlaying =
            clipIndex >= 0 && clipIndex < static_cast<int>(m_clipIsPlaying.size()) && m_clipIsPlaying[clipIndex];
        if (!clipPlaying)
        {
            toleranceMs *= kEasyModeStoppedToleranceMultiplier;
        }
    }
    return toleranceMs / 1000.0;
}

// If the clip's declared span is shorter than its stem's actual duration,
// tiles each lane's notes (independently, repeating every original span)
// to fill the whole clip, and widens spanBeats to match - so a short
// authored phrase repeats to cover a longer clip instead of leaving the
// back half of every loop silent/ungraded, and the judged pattern's cycle
// stays in sync with what the audio actually repeats. A trailing repeat
// that gets cut off mid-loop only keeps the notes that fully fit (press
// through release) before the cutoff - a note whose start fits but whose
// duration would run past it is dropped rather than shipped as a broken,
// unplayable note.
void GameSession::ExpandLaneNotesToFillClip(ChartClip& clip, double stemDurationSeconds, double bpm)
{
    if (clip.spanBeats <= 0.0 || stemDurationSeconds <= 0.0)
    {
        return;
    }

    double secondsPerBeat = 60.0 / bpm;
    double clipBeats = stemDurationSeconds / secondsPerBeat;
    if (clipBeats <= clip.spanBeats + 1e-6)
    {
        return;
    }

    double originalSpan = clip.spanBeats;
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (clip.laneNotes[lane].empty())
        {
            continue;
        }

        std::vector<LaneNote> original = clip.laneNotes[lane];
        std::vector<LaneNote> expanded;
        for (double repeatStart = 0.0; repeatStart < clipBeats - 1e-9; repeatStart += originalSpan)
        {
            for (const LaneNote& note : original)
            {
                double absoluteStart = repeatStart + note.startBeat;
                // Require the note's *entire* press-to-release span to fit
                // before the loop wraps, not just its start. A trailing
                // repeat of the pattern can otherwise have its last note
                // land just barely before clipBeats (its start passes the
                // old start-only check) while its real-world stem duration
                // - which almost never lands on an exact beat boundary -
                // truncates its actual playable window to a sliver, or
                // leaves it landing implausibly close to the next loop's
                // own first note. Either way it's unplayable as authored,
                // so it's dropped rather than shipped as a note the player
                // can never legitimately hit.
                if (absoluteStart + note.durationBeats <= clipBeats + 1e-9)
                {
                    expanded.push_back({absoluteStart, note.durationBeats});
                }
            }
        }
        clip.laneNotes[lane] = std::move(expanded);
    }

    clip.spanBeats = clipBeats;
}

// Simplifies clip's MIDI-derived pattern for easy mode - see the header's
// doc comment for the full contract. Runs on one repetition's worth of
// notes (before ExpandLaneNotesToFillClip tiles it), so spanBeats here
// still means "one repetition's length."
void GameSession::ApplyEasyModeTransform(ChartClip& clip)
{
    if (!clip.hasMidi || clip.spanBeats <= 0.0)
    {
        return;
    }

    // spanBeats is always an exact whole number of beats - ChartFile's
    // AlignToBarBoundary (called while parsing every clip's MIDI file) only
    // ever produces a whole multiple of the song's integer beatsPerBar - so
    // the integer beat arithmetic below is exact, with no epsilon-
    // comparison hazard.
    long long spanBeatsInt = static_cast<long long>(std::llround(clip.spanBeats));

    // Per lane: round every original note UP to the next quarter-note beat
    // (never down/nearest - a combined note starts at or after where the
    // original notes began, never earlier). A std::set collapses multiple
    // originals landing on the same beat into one automatically. Wrapping a
    // note quantized off the loop's end back to beat 0 is correct, not a
    // bug: once tiled/looped, beat spanBeatsInt IS beat 0 of the next
    // repetition - the same absolute instant.
    std::set<long long> laneBeats[kLaneCount];
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        for (const LaneNote& note : clip.laneNotes[lane])
        {
            long long target = static_cast<long long>(std::ceil(note.startBeat - 1e-6));
            target %= spanBeatsInt;
            laneBeats[lane].insert(target);
        }
    }

    // No simultaneous notes: a beat claimed by more than one lane survives
    // only in the lowest-indexed lane that claims it.
    for (long long beat = 0; beat < spanBeatsInt; ++beat)
    {
        bool claimed = false;
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            if (laneBeats[lane].count(beat))
            {
                if (claimed)
                {
                    laneBeats[lane].erase(beat);
                }
                claimed = true;
            }
        }
    }

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        std::vector<LaneNote> quantized;
        quantized.reserve(laneBeats[lane].size());
        for (long long beat : laneBeats[lane]) // std::set iterates sorted ascending
        {
            quantized.push_back(LaneNote{static_cast<double>(beat), kEasyModeNoteDurationBeats});
        }
        clip.laneNotes[lane] = std::move(quantized);
    }
}

// Returns the smallest note start (in absolute beats) strictly after afterBeat, for this lane.
double GameSession::NextOnsetAfter(double afterBeat, const ChartClip& clip, int lane) const
{
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty())
    {
        return afterBeat + clip.spanBeats;
    }

    double span = clip.spanBeats;
    long long barIndex = static_cast<long long>(std::floor(afterBeat / span));

    for (const LaneNote& note : notes)
    {
        double candidate = barIndex * span + note.startBeat;
        if (candidate > afterBeat + 1e-9)
        {
            return candidate;
        }
    }
    return (barIndex + 1) * span + notes.front().startBeat;
}

// Returns the absolute beat of this lane's note in the first full pattern
// cycle that starts at or after afterBeat, preserving every lane's
// authored relative offset within that shared cycle - see the header
// comment and m_songHasStarted for why this differs from NextOnsetAfter.
double GameSession::FirstReachableOnset(double afterBeat, const ChartClip& clip, int lane) const
{
    const std::vector<LaneNote>& notes = clip.laneNotes[lane];
    if (notes.empty())
    {
        return NextOnsetAfter(afterBeat, clip, lane);
    }

    double span = clip.spanBeats;
    long long barIndex = (span > 0.0) ? static_cast<long long>(std::ceil((afterBeat - 1e-9) / span)) : 0;
    return barIndex * span + notes.front().startBeat;
}

// Computes every lane's first-note anchor at once - see the header
// comment for why this tries NextOnsetAfter's direct, per-lane candidates
// first (safe exactly when they all land in the same pattern cycle) before
// falling back to FirstReachableOnset's slower, always-safe behavior.
void GameSession::FirstReachableOnsetForAllLanes(double afterBeat, const ChartClip& clip,
                                                  double outBeats[kLaneCount]) const
{
    double span = clip.spanBeats;
    double candidates[kLaneCount];
    long long minCycle = 0;
    long long maxCycle = 0;
    bool sawLaneWithNotes = false;

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        candidates[lane] = NextOnsetAfter(afterBeat, clip, lane);
        if (clip.laneNotes[lane].empty())
        {
            continue; // no real note on this lane to constrain the cycle check with
        }

        long long cycle = (span > 0.0) ? static_cast<long long>(std::floor(candidates[lane] / span)) : 0;
        if (!sawLaneWithNotes)
        {
            minCycle = maxCycle = cycle;
            sawLaneWithNotes = true;
        }
        else
        {
            minCycle = std::min(minCycle, cycle);
            maxCycle = std::max(maxCycle, cycle);
        }
    }

    if (sawLaneWithNotes && minCycle == maxCycle)
    {
        // Every lane's own next reachable note falls within the same
        // cycle - no lane is being asked to skip ahead of another, so
        // using them directly can't corrupt any authored relative timing.
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            outBeats[lane] = candidates[lane];
        }
        return;
    }

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        outBeats[lane] = FirstReachableOnset(afterBeat, clip, lane);
    }
}

// Returns the lane note whose phase-within-span matches absoluteStartBeat's phase, or nullptr if none does.
const LaneNote* GameSession::FindLaneNote(const ChartClip& clip, int lane, double absoluteStartBeat) const
{
    double span = clip.spanBeats;
    double phase = std::fmod(absoluteStartBeat, span);
    if (phase < 0.0)
    {
        phase += span;
    }

    for (const LaneNote& note : clip.laneNotes[lane])
    {
        if (std::abs(note.startBeat - phase) < 1e-6)
        {
            return &note;
        }
    }
    return nullptr;
}
