#include "GameSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// Easy mode's start-tolerance widening - see EffectiveStartToleranceSeconds.
constexpr double c_EasyModeToleranceMultiplier = 1.5;        // Applied unconditionally.
constexpr double c_EasyModeStoppedToleranceMultiplier = 2.0; // Applied on top, only while the clip isn't playing.

// How far the judging clock may drift from the audio hardware's playback position before Update()
// resyncs it, in seconds.
constexpr double c_ClockResyncThresholdSeconds = 0.008;

// Points a single hit pays into the bank - flat, no combo scaling; MultiplierForStreak rewards
// sustained accuracy at payout instead.
constexpr int c_PrecisePoints = 10;
constexpr int c_ImprecisePoints = 5;

// A press beyond this fraction of the effective start tolerance is judged imprecise, not precise.
constexpr double c_ImprecisionToleranceFraction = 0.5;

// Streak-length breakpoints for MultiplierForStreak: 0-9 -> x1, 10-19 -> x2, 20-29 -> x3, 30+ -> x4.
constexpr int c_MultiplierTierStreaks[] = {10, 20, 30};

} // namespace

int GameSession::MultiplierForStreak(int streak)
{
    int multiplier = 1;
    for (int tierStreak : c_MultiplierTierStreaks)
    {
        if (streak >= tierStreak)
        {
            ++multiplier;
        }
    }
    return multiplier;
}

GameSession::GameSession(AudioEngine& audioEngine) : m_audioEngine(audioEngine)
{
}

bool GameSession::LoadChart(const std::wstring& chartFilePath, bool easyMode, std::wstring& outError)
{
    ChartSong song;
    std::vector<std::wstring> errors;
    if (!song.Load(chartFilePath, errors))
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
    // Captured before ExpandLaneNotesToFillClip can widen any clip's spanBeats:
    // ValidateArrangementAlignment needs each clip's AUTHORED bar-aligned length, not the widened one.
    std::unordered_map<const ChartClip*, ChartClip::ClipAlignmentInfo> clipAlignmentInfo;
    for (ChartClip& clip : song.MutableClips())
    {
        StemHandle handle = m_audioEngine.LoadStem(clip.WavFilePath());
        if (!handle.IsValid())
        {
            outError = L"clip '" + clip.Name() + L"': file '" + clip.WavFilePath() +
                       L"' could not be loaded as audio (unsupported or corrupt WAV format)";
            return false;
        }
        stemHandles.push_back(handle);
        clipAlignmentInfo[&clip] = {clip.SpanBeats(), m_audioEngine.GetStemDurationSeconds(handle)};

        // A clip with no .mid file has no pattern to validate or tile - it's only ever played back
        // whole (break/background), never judged.
        if (clip.HasMidi())
        {
            if (easyMode)
            {
                clip.ApplyEasyModeTransform(song.Bpm());
            }

            double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
            // If the pattern doesn't fit in one loop of the stem, the audio wraps before the
            // pattern's last notes are reached and they'd be judged against a moment the audio
            // isn't at. Rejected at load time. Shared with the editor via ChartClip::ClipFitsOneLoop.
            if (!clip.ClipFitsOneLoop(stemDuration, song.Bpm()))
            {
                double secondsPerBeat = song.SecondsPerBeat();
                double clipBeats = stemDuration / secondsPerBeat;
                outError = L"clip '" + clip.Name() + L"': its MIDI pattern (" + std::to_wstring(clip.SpanBeats()) +
                           L" beats) is longer than one loop of its audio ('" + clip.WavFilePath() + L"', " +
                           std::to_wstring(clipBeats) + L" beats) - trim the MIDI pattern or use a longer audio stem";
                return false;
            }
            clip.ExpandLaneNotesToFillClip(stemDuration, song.Bpm());
        }
    }

    // The whole-chart bar-alignment invariant (see ChartClip's class comment), checked once here
    // with real stem durations so a chart that could only misbehave at runtime is refused up front.
    std::vector<std::wstring> alignmentErrors;
    if (!ChartClip::ValidateArrangementAlignment(song, clipAlignmentInfo, alignmentErrors))
    {
        outError.clear();
        for (const std::wstring& error : alignmentErrors)
        {
            if (!outError.empty())
            {
                outError += L"\r\n";
            }
            outError += error;
        }
        return false;
    }

    m_song = std::move(song);
    m_stemHandles = std::move(stemHandles);
    m_phase = GamePhase::Idle;
    m_easyMode = easyMode;
    m_currentInstance = SectionInstance();
    m_clipInstances.clear();
    m_queuedBackground = QueuedBackground{};
    m_arrangementResetPending = false;
    m_lastJudgement = JudgementResult::None;
    for (BufferedPress& buffered : m_bufferedPress)
    {
        buffered = BufferedPress{};
    }
    return true;
}

void GameSession::Start()
{
    if (m_song.Sections().empty())
    {
        return;
    }

    // A previous run may have left clips passing and still looping, so stop them explicitly or
    // they'd play underneath the new run.
    m_audioEngine.StopAll();
    m_currentInstance = SectionInstance();
    // The bar-alignment origin re-anchors itself once BeginSection reaches section 0 again, so
    // there's nothing to reset here.
    m_clipInstances.clear();
    m_queuedBackground = QueuedBackground{};
    m_arrangementResetPending = false;
    m_lastJudgement = JudgementResult::None;
    m_paused = false;
    for (BufferedPress& buffered : m_bufferedPress)
    {
        buffered = BufferedPress{};
    }

    m_score = 0;
    m_bank = 0;
    m_streakTracker = StreakTracker();
    for (double& cursor : m_autoScoreCursorBeat)
    {
        cursor = 0.0;
    }
    m_hudChangeEvents.clear();
    m_sfxEvents.clear();

    m_clock.Start(m_song.Bpm());
    m_phase = GamePhase::CountIn;
}

void GameSession::Stop()
{
    m_audioEngine.StopAll();
    m_phase = GamePhase::Idle;
    m_currentInstance = SectionInstance();
    for (auto& entry : m_clipInstances)
    {
        entry.second.MarkStopped();
    }
    m_queuedBackground = QueuedBackground{};
    m_arrangementResetPending = false;
    m_lastJudgement = JudgementResult::None;
    m_paused = false;
    for (BufferedPress& buffered : m_bufferedPress)
    {
        buffered = BufferedPress{};
    }
}

void GameSession::Pause()
{
    if (m_paused)
    {
        return;
    }
    m_paused = true;
    m_clock.Pause();
    m_audioEngine.PauseAll();
}

void GameSession::Resume()
{
    if (!m_paused)
    {
        return;
    }
    m_paused = false;
    m_clock.Resume();
    m_audioEngine.ResumeAll();
}

bool GameSession::IsPaused() const
{
    return m_paused;
}

void GameSession::CatchUpCountIn()
{
    if (m_phase != GamePhase::CountIn)
    {
        return;
    }
    double secondsPerBeat = m_song.SecondsPerBeat();
    double transitionSeconds = CountInSeconds();
    if (m_clock.ElapsedSeconds() >= transitionSeconds)
    {
        BeginSection(0, transitionSeconds / secondsPerBeat);
    }
}

bool GameSession::IsLaneJudgeable(int lane) const
{
    if (m_phase != GamePhase::Learning)
    {
        return false;
    }
    if (lane < 0 || lane >= c_LaneCount || m_currentInstance.SectionIndex() < 0)
    {
        return false;
    }
    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    if (section.kind != SectionKind::Learn)
    {
        return false; // break/reset/background: no judging, ever
    }
    const ChartClip& clip = m_song.Clips()[section.clipIndex];
    if (clip.LaneNotes(lane).empty())
    {
        return false;
    }
    // Once a Pass-mode section locks in, Update()'s post-lock-in auto-accrual is its sole scorer;
    // a real press reaching RegisterHit/RegisterMiss again would double-count. DontFail passing is
    // reversible, so it still needs real presses.
    if (clip.Mode() == LearnMode::Pass && m_currentInstance.IsPassing())
    {
        return false;
    }
    return true;
}

void GameSession::OnPress(int lane)
{
    if (m_paused)
    {
        return;
    }
    CatchUpCountIn();
    if (!IsLaneJudgeable(lane))
    {
        return;
    }

    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.Clips()[section.clipIndex];

    double secondsPerBeat = m_song.SecondsPerBeat();
    double startBeat = m_currentInstance.NextExpectedBeatForLane(lane);
    double startSeconds = startBeat * secondsPerBeat;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
    double nowSeconds = m_clock.ElapsedSeconds();

    if (std::abs(nowSeconds - startSeconds) <= toleranceSeconds)
    {
        ApplyInTolerancePress(lane, section, clip, startBeat, nowSeconds);
    }
    else if (nowSeconds < startSeconds - toleranceSeconds)
    {
        // Too early to judge against this note - silently ignored so a stray tap doesn't cost a
        // note not yet reached. The lane keeps awaiting it; a later on-time press still resolves it.
    }
    else
    {
        // Too late: fails immediately but doesn't advance - the lane keeps awaiting the same note
        // until hit or timed out.
        RegisterMiss(lane);
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
    }
}

void GameSession::ApplyInTolerancePress(int lane, const ChartSection& section, const ChartClip& clip,
                                         double startBeat, double pressSeconds)
{
    double secondsPerBeat = m_song.SecondsPerBeat();
    double startSeconds = startBeat * secondsPerBeat;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
    double originBeat = ArrangementOriginBeat();
    const LaneNote* note = FindLaneNote(clip, lane, originBeat, startBeat);
    double durationBeats = note ? note->durationBeats : 0.0;

    // A press past c_ImprecisionToleranceFraction of the tolerance window is still correct but not
    // precise (scores fewer points).
    bool wasPrecise = std::abs(pressSeconds - startSeconds) <= toleranceSeconds * c_ImprecisionToleranceFraction;

    StartClipLoop(section.clipIndex, clip.InitVolume());
    m_currentInstance.StartLaneHold(lane, startBeat, startBeat + durationBeats, wasPrecise);
    AdvanceExpectedNote(lane);

    if (m_easyMode)
    {
        // Easy mode ignores release timing, so the press is the final judgement.
        RegisterHit(lane, wasPrecise);
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Hit);
    }
    else
    {
        // A correct press isn't judged until release, so clear any stale Hit/Miss or the next
        // ConsumeLastJudgement() would misattribute it to this press.
        m_lastJudgement = JudgementResult::None;
    }
}

bool GameSession::TryBufferEarlyPress(int lane)
{
    if (m_paused || lane < 0 || lane >= c_LaneCount)
    {
        return false;
    }

    int previewIdx = PreviewSectionIndex();
    if (previewIdx < 0 || m_song.Sections()[previewIdx].kind != SectionKind::Learn)
    {
        return false;
    }
    const ChartClip& clip = m_song.Clips()[m_song.Sections()[previewIdx].clipIndex];
    if (clip.LaneNotes(lane).empty())
    {
        return false;
    }

    double onsetBeat = PreviewFirstOnsetBeatForLane(lane);
    if (onsetBeat < 0.0)
    {
        return false;
    }
    double secondsPerBeat = m_song.SecondsPerBeat();
    double onsetSeconds = onsetBeat * secondsPerBeat;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
    double nowSeconds = m_clock.ElapsedSeconds();
    if (nowSeconds < onsetSeconds - toleranceSeconds)
    {
        return false; // too early even for this - a stray tap, not anticipation
    }

    m_bufferedPress[lane] = BufferedPress{/*active=*/true, nowSeconds, /*released=*/false, 0.0};
    return true;
}

void GameSession::ConsumeBufferedPresses(const ChartSection& section, const ChartClip& clip)
{
    double secondsPerBeat = m_song.SecondsPerBeat();
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);

    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        BufferedPress buffered = m_bufferedPress[lane];
        m_bufferedPress[lane] = BufferedPress{}; // never carries over past this section beginning

        if (!buffered.active || clip.LaneNotes(lane).empty())
        {
            continue;
        }

        double startBeat = m_currentInstance.NextExpectedBeatForLane(lane);
        double startSeconds = startBeat * secondsPerBeat;
        // Re-validated against the note's real, just-established onset, not trusted from when it
        // was buffered.
        if (std::abs(buffered.pressSeconds - startSeconds) > toleranceSeconds)
        {
            continue;
        }

        ApplyInTolerancePress(lane, section, clip, startBeat, buffered.pressSeconds);

        if (buffered.released && m_currentInstance.IsLaneHeld(lane))
        {
            // The player let go before this section began (a very fast anticipatory tap) - resolve
            // that release now against the hold just started, as OnRelease would.
            if (m_easyMode)
            {
                m_currentInstance.ClearLaneHold(lane);
            }
            else
            {
                double endSeconds = m_currentInstance.LaneHoldExpectedEndBeat(lane) * secondsPerBeat;
                double releaseToleranceSeconds = clip.ReleaseToleranceMs() / 1000.0;
                double holdStartBeat = m_currentInstance.LaneHoldStartBeat(lane);
                if (std::abs(buffered.releaseSeconds - endSeconds) <= releaseToleranceSeconds)
                {
                    RegisterHit(lane, m_currentInstance.LaneHoldWasPrecise(lane));
                    RecordOnsetJudgement(holdStartBeat, lane, JudgementResult::Hit);
                }
                else
                {
                    RegisterMiss(lane);
                    RecordOnsetJudgement(holdStartBeat, lane, JudgementResult::Miss);
                }
                m_currentInstance.ClearLaneHold(lane);
            }
        }
    }
}

void GameSession::OnRelease(int lane)
{
    if (lane < 0 || lane >= c_LaneCount || !m_currentInstance.IsLaneHeld(lane) || m_currentInstance.SectionIndex() < 0)
    {
        // Not holding anything - but if a press was buffered for this lane and not yet consumed,
        // remember this release too, so a fast tap-and-release ahead of a section transition
        // resolves as it would have inside the section.
        if (lane >= 0 && lane < c_LaneCount && m_bufferedPress[lane].active && !m_bufferedPress[lane].released)
        {
            m_bufferedPress[lane].released = true;
            m_bufferedPress[lane].releaseSeconds = m_clock.ElapsedSeconds();
        }
        return;
    }

    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    if (section.kind != SectionKind::Learn)
    {
        return; // structurally shouldn't happen (holds only populate in Learn), kept as defense-in-depth
    }

    if (m_easyMode)
    {
        // Already judged Hit at press time; releasing just lets go of the hold.
        m_currentInstance.ClearLaneHold(lane);
        m_lastJudgement = JudgementResult::None;
        return;
    }

    const ChartClip& clip = m_song.Clips()[section.clipIndex];
    double secondsPerBeat = m_song.SecondsPerBeat();
    double endSeconds = m_currentInstance.LaneHoldExpectedEndBeat(lane) * secondsPerBeat;
    double toleranceSeconds = clip.ReleaseToleranceMs() / 1000.0;
    double nowSeconds = m_clock.ElapsedSeconds();
    double startBeat = m_currentInstance.LaneHoldStartBeat(lane);

    if (std::abs(nowSeconds - endSeconds) <= toleranceSeconds)
    {
        bool wasPrecise = m_currentInstance.LaneHoldWasPrecise(lane);
        RegisterHit(lane, wasPrecise);
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Hit, wasPrecise);
    }
    else
    {
        RegisterMiss(lane);
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
    }

    m_currentInstance.ClearLaneHold(lane);
}

void GameSession::Update()
{
    if (m_paused)
    {
        return;
    }

    // Keep the judging clock (a free-running CPU timer) locked to actual audio playback (XAudio2's
    // hardware clock), which drift apart over a long-looping clip. Every playing clip is started
    // phase-aligned to the same beat grid, so the current section's clip is a valid drift reference.
    // The IsPlaying() guard matters: once a break clip's finite loop_count finishes, XAudio2 stops
    // the voice and GetPositionSeconds() freezes - resyncing against a frozen position would pin
    // the clock forever and the game would hang.
    if (m_currentInstance.SectionIndex() >= 0)
    {
        int clipIndex = m_song.Sections()[m_currentInstance.SectionIndex()].clipIndex;
        auto it = clipIndex >= 0 ? m_clipInstances.find(&m_song.Clips()[clipIndex]) : m_clipInstances.end();
        if (it != m_clipInstances.end() && it->second.IsPlaying() && m_audioEngine.IsPlaying(m_stemHandles[clipIndex]))
        {
            double audioElapsed = it->second.LoopStartSeconds() + m_audioEngine.GetPositionSeconds(m_stemHandles[clipIndex]);
            if (std::abs(audioElapsed - m_clock.ElapsedSeconds()) > c_ClockResyncThresholdSeconds)
            {
                m_clock.Resync(audioElapsed);
            }
        }
    }

    double secondsPerBeat = m_song.SecondsPerBeat();
    double now = m_clock.ElapsedSeconds();

    // Held-past-late-release timeout, independent of phase/pending-advance so a hold in flight
    // keeps resolving even if the section has started passing. Skipped in easy mode, where a hold
    // is already judged at press time.
    if (m_currentInstance.SectionIndex() >= 0 && !m_easyMode)
    {
        const ChartSection& heldSection = m_song.Sections()[m_currentInstance.SectionIndex()];
        if (heldSection.kind == SectionKind::Learn)
        {
            const ChartClip& heldClip = m_song.Clips()[heldSection.clipIndex];
            double toleranceSeconds = heldClip.ReleaseToleranceMs() / 1000.0;
            for (int lane = 0; lane < c_LaneCount; ++lane)
            {
                if (!m_currentInstance.IsLaneHeld(lane))
                {
                    continue;
                }
                double endSeconds = m_currentInstance.LaneHoldExpectedEndBeat(lane) * secondsPerBeat;
                if (now > endSeconds + toleranceSeconds)
                {
                    double startBeat = m_currentInstance.LaneHoldStartBeat(lane);
                    RegisterMiss(lane);
                    RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
                    m_currentInstance.ClearLaneHold(lane);
                }
            }
        }
    }

    if (m_phase == GamePhase::CountIn)
    {
        // The count-in is bar-aligned, so the first section always has its full start-tolerance
        // window ahead of it.
        CatchUpCountIn();
        return;
    }

    if (m_phase == GamePhase::Learning)
    {
        if (m_currentInstance.HasPendingAdvance() && now >= m_currentInstance.PendingAdvanceAtSeconds())
        {
            const ChartSection& finishedSection = m_song.Sections()[m_currentInstance.SectionIndex()];

            // A learn section that reached its candidate advance without locking in just repeats:
            // push the advance back one full loop and re-check next time this instant is reached.
            // The audio was looping continuously anyway, so only the "am I allowed to leave" check
            // repeats.
            if (finishedSection.kind == SectionKind::Learn && !m_currentInstance.IsPassing())
            {
                double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[finishedSection.clipIndex]);
                m_currentInstance.ExtendPendingAdvance(stemDuration);
                return;
            }

            m_currentInstance.ClearPendingAdvance();

            // The section genuinely finished - pay the bank out into the permanent total at the
            // current streak multiplier, then reset the streak. A payout is the other trigger (besides
            // the 3-miss trip) that zeroes m_streakTracker, so the next section starts at x1.
            if (m_bank > 0)
            {
                int multiplierBefore = MultiplierForStreak(m_streakTracker.Streak());
                m_score += m_bank * multiplierBefore;
                m_bank = 0;
                m_streakTracker.Reset();
                PushHudChanged(HudField::Total, m_score);
                PushHudChanged(HudField::Bank, m_bank);
                if (multiplierBefore != 1)
                {
                    PushHudChanged(HudField::Multiplier, 1);
                }
            }

            int nextIndex = m_currentInstance.SectionIndex() + 1;

            // A learn section reaching here is always passing; its clip keeps playing by design,
            // so nothing to do for it. A break section is a one-off interlude:
            if (finishedSection.kind == SectionKind::Break)
            {
                // Stop it once its loop_count wait completes so it doesn't drone on under every
                // later section. (Reset never reaches here - it sets no pending advance.) If the
                // next section re-queues this same clip as a background layer, this stop-then-restart
                // stays phase-continuous: it restarts against the origin re-anchored just below,
                // which lands on this same instant.
                StopClipLoop(finishedSection.clipIndex);

                // A Break implicitly ends with a Reset, so the bar-alignment origin must re-anchor
                // here too, or whatever picks up next only lands on a shared loop boundary if the
                // break's played duration happens to be a whole multiple of that clip's length.
                // Deferred to BeginSection (see m_arrangementResetPending).
                m_arrangementResetPending = true;
            }

            if (nextIndex < static_cast<int>(m_song.Sections().size()))
            {
                BeginSection(nextIndex, m_currentInstance.PendingAdvanceAtSeconds() / secondsPerBeat);
            }
            else
            {
                m_phase = GamePhase::Complete;
            }
            return;
        }

        // Press-phase timeout: any Learn lane still awaiting a press whose window has closed. Runs
        // even while a pending advance is scheduled, so notes keep getting judged after lock-in.
        const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
        if (section.kind == SectionKind::Learn)
        {
            const ChartClip& clip = m_song.Clips()[section.clipIndex];
            if (clip.Mode() == LearnMode::Pass && m_currentInstance.IsPassing())
            {
                // Locked in, Pass mode: IsLaneJudgeable blocks real presses now, so this is the
                // sole scorer to the section's advance - walk each lane forward from its auto-score
                // cursor and credit every newly-crossed note as a precise hit.
                const ClipInstance& instance = m_clipInstances.at(&clip);
                double nowBeat = now / secondsPerBeat;
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    if (clip.LaneNotes(lane).empty())
                    {
                        continue;
                    }
                    for (double onsetBeat :
                         instance.OnsetsInRange(m_song.Bpm(), m_autoScoreCursorBeat[lane], nowBeat, lane))
                    {
                        RegisterHit(lane, /*wasPrecise=*/true);
                        RecordOnsetJudgement(onsetBeat, lane, JudgementResult::Hit, /*precise=*/true);
                    }
                    m_autoScoreCursorBeat[lane] = nowBeat;
                }
            }
            else
            {
                double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    if (clip.LaneNotes(lane).empty())
                    {
                        continue;
                    }
                    double expectedBeat = m_currentInstance.NextExpectedBeatForLane(lane);
                    double onsetSeconds = expectedBeat * secondsPerBeat;
                    if (now > onsetSeconds + toleranceSeconds)
                    {
                        RegisterMiss(lane);
                        RecordOnsetJudgement(expectedBeat, lane, JudgementResult::Miss);
                        AdvanceExpectedNote(lane);
                    }
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
    return m_currentInstance.SectionIndex();
}

StemHandle GameSession::DebugStemHandle(int clipIndex) const
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_stemHandles.size()))
    {
        return StemHandle{};
    }
    return m_stemHandles[clipIndex];
}

const ChartClip* GameSession::CurrentClip() const
{
    int sectionIndex = m_currentInstance.SectionIndex();
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(m_song.Sections().size()))
    {
        return nullptr;
    }
    int clipIndex = m_song.Sections()[sectionIndex].clipIndex;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.Clips().size()))
    {
        return nullptr;
    }
    return &m_song.Clips()[clipIndex];
}

SectionKind GameSession::CurrentSectionKind() const
{
    int sectionIndex = m_currentInstance.SectionIndex();
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(m_song.Sections().size()))
    {
        return SectionKind::Learn;
    }
    return m_song.Sections()[sectionIndex].kind;
}

int GameSession::CurrentStreak() const
{
    return m_currentInstance.Streak();
}

int GameSession::CurrentScore() const
{
    return m_score;
}

int GameSession::CurrentBank() const
{
    return m_bank;
}

int GameSession::CurrentMultiplier() const
{
    return MultiplierForStreak(m_streakTracker.Streak());
}

int GameSession::ScoringStreak() const
{
    return m_streakTracker.Streak();
}

// Returns the beat of the next note this lane is awaiting a press for.
double GameSession::NextExpectedBeatForLane(int lane) const
{
    return m_currentInstance.NextExpectedBeatForLane(lane);
}

double GameSession::CurrentClipOriginBeat() const
{
    if (CurrentClip() == nullptr)
    {
        return 0.0;
    }
    return ArrangementOriginBeat();
}

const SongClock& GameSession::Clock() const
{
    return m_clock;
}

bool GameSession::IsAwaitingAdvance() const
{
    return m_currentInstance.HasPendingAdvance();
}

bool GameSession::IsPassing() const
{
    return m_currentInstance.IsPassing();
}

double GameSession::PendingAdvanceAtSeconds() const
{
    return m_currentInstance.HasPendingAdvance() ? m_currentInstance.PendingAdvanceAtSeconds() : -1.0;
}

int GameSession::NextPersistentSectionAtOrAfter(int startIndex) const
{
    for (int i = std::max(startIndex, 0); i < static_cast<int>(m_song.Sections().size()); ++i)
    {
        if (m_song.Sections()[i].kind != SectionKind::Background && m_song.Sections()[i].kind != SectionKind::Reset)
        {
            return i;
        }
    }
    return -1;
}

int GameSession::PreviewSectionIndex() const
{
    if (m_phase == GamePhase::CountIn)
    {
        int idx = NextPersistentSectionAtOrAfter(0);
        if (idx < 0 || m_song.Sections()[idx].kind != SectionKind::Learn)
        {
            return -1;
        }
        return idx;
    }
    int sectionIndex = m_currentInstance.SectionIndex();
    if (m_phase != GamePhase::Learning || sectionIndex < 0)
    {
        return -1;
    }

    // A non-passing learn section doesn't know when it'll advance (it may repeat any number of
    // loops first), so there's nothing legitimate to preview yet - a real preview would have to be
    // retracted if it repeats. A break section always knows its advance up front.
    const ChartSection& current = m_song.Sections()[sectionIndex];
    if (current.kind == SectionKind::Learn && !m_currentInstance.IsPassing())
    {
        return -1;
    }

    // Learn-awaiting-advance and Break-awaiting-advance both set a pending advance the same way,
    // so a break's hold previews the next learn section's dots just as a learn's hold does.
    if (m_currentInstance.HasPendingAdvance())
    {
        int nextIdx = NextPersistentSectionAtOrAfter(sectionIndex + 1);
        if (nextIdx < 0 || m_song.Sections()[nextIdx].kind != SectionKind::Learn)
        {
            return -1;
        }
        return nextIdx;
    }
    return -1;
}

bool GameSession::ArrangementResetsBeforeSection(int sectionIndex) const
{
    for (int i = m_currentInstance.SectionIndex() + 1; i < sectionIndex; ++i)
    {
        if (m_song.Sections()[i].kind == SectionKind::Reset)
        {
            return true;
        }
    }
    return false;
}

const ChartClip* GameSession::PreviewClip() const
{
    int idx = PreviewSectionIndex();
    if (idx < 0)
    {
        return nullptr;
    }
    return &m_song.Clips()[m_song.Sections()[idx].clipIndex];
}

double GameSession::PreviewTransitionSeconds() const
{
    if (m_phase == GamePhase::CountIn)
    {
        return CountInSeconds();
    }
    if (m_phase == GamePhase::Learning && m_currentInstance.HasPendingAdvance())
    {
        return m_currentInstance.PendingAdvanceAtSeconds();
    }
    return -1.0;
}

double GameSession::PreviewFirstOnsetBeatForLane(int lane) const
{
    int idx = PreviewSectionIndex();
    if (idx < 0 || lane < 0 || lane >= c_LaneCount)
    {
        return -1.0;
    }
    const ChartClip& preview = m_song.Clips()[m_song.Sections()[idx].clipIndex];

    double secondsPerBeat = m_song.SecondsPerBeat();
    double transitionBeat = PreviewTransitionSeconds() / secondsPerBeat;
    double originBeat = PreviewClipOriginBeat();

    // PreviewClipOriginBeat() resolves to transitionBeat itself for a fresh arrangement, so this
    // is correct whether preview joins fresh or continues an open groove.
    return preview.NextOnsetAfter(originBeat, transitionBeat - c_FreshJoinEpsilonBeats, lane);
}

double GameSession::PreviewClipOriginBeat() const
{
    int idx = PreviewSectionIndex();
    if (idx < 0)
    {
        return -1.0;
    }
    double secondsPerBeat = m_song.SecondsPerBeat();
    // During the count-in the playthrough hasn't begun, so there's no established origin to inherit
    // yet - same as a Reset lying between here and idx.
    if (m_phase != GamePhase::CountIn && !ArrangementResetsBeforeSection(idx))
    {
        return ArrangementOriginBeat();
    }
    if (m_phase == GamePhase::CountIn)
    {
        // BeginPlaythrough() sets the origin directly from this value with no rounding, so the raw
        // prediction already matches.
        return PreviewTransitionSeconds() / secondsPerBeat;
    }
    // A Reset re-anchors the origin to the nearest whole beat, so predict that rounded value, not
    // the raw seconds-derived one - otherwise a lane with a note exactly at phase 0 can land on the
    // wrong side of a bar boundary between this prediction and the real section start.
    long long predictedOriginBeat = std::llround(m_song.SecondsToAbsoluteBeat(PreviewTransitionSeconds()));
    return m_song.AbsoluteBeatToSeconds(predictedOriginBeat) / secondsPerBeat;
}

JudgementResult GameSession::ConsumeLastJudgement()
{
    JudgementResult result = m_lastJudgement;
    m_lastJudgement = JudgementResult::None;
    return result;
}

std::vector<GameSession::JudgementEvent> GameSession::ConsumeJudgementEvents()
{
    std::vector<JudgementEvent> events = std::move(m_judgementEvents);
    m_judgementEvents.clear();
    return events;
}

std::vector<GameSession::HudChangeEvent> GameSession::ConsumeHudChangeEvents()
{
    std::vector<HudChangeEvent> events = std::move(m_hudChangeEvents);
    m_hudChangeEvents.clear();
    return events;
}

std::vector<GameSession::SfxCue> GameSession::ConsumeSfxEvents()
{
    std::vector<SfxCue> events = std::move(m_sfxEvents);
    m_sfxEvents.clear();
    return events;
}

void GameSession::PushHudChanged(HudField field, int newValue)
{
    m_hudChangeEvents.push_back({field, newValue});
}

void GameSession::PushSfx(SfxCue cue)
{
    m_sfxEvents.push_back(cue);
}

JudgementResult GameSession::OnsetJudgement(double startBeat, int lane) const
{
    return m_currentInstance.OnsetJudgement(startBeat, lane);
}

bool GameSession::OnsetPrecise(double startBeat, int lane) const
{
    return m_currentInstance.OnsetPrecise(startBeat, lane);
}

bool GameSession::IsLaneHeld(int lane) const
{
    return m_currentInstance.IsLaneHeld(lane);
}

double GameSession::LaneHoldStartBeat(int lane) const
{
    return m_currentInstance.LaneHoldStartBeat(lane);
}

// Forwards to m_currentInstance, adding RHYTHM_DEBUG_JUDGEMENTS tracing.
void GameSession::RecordOnsetJudgement(double startBeat, int lane, JudgementResult result, bool precise)
{
#ifdef RHYTHM_DEBUG_JUDGEMENTS
    int sectionIndex = m_currentInstance.SectionIndex();
    std::fprintf(stderr, "[RecordOnsetJudgement] t=%.4f beat=%.4f lane=%d result=%d section=%d clip=%d\n",
                 m_clock.ElapsedSeconds(), startBeat, lane, static_cast<int>(result), sectionIndex,
                 sectionIndex >= 0 ? m_song.Sections()[sectionIndex].clipIndex : -1);
#endif
    m_currentInstance.RecordOnsetJudgement(startBeat, lane, result, precise);
}

void GameSession::BeginSection(int sectionIndex, double scheduledBeat)
{
    // Resolved before constructing m_currentInstance, since a Learn clip's learnMode decides the
    // instance's starting passing state. Pass is a harmless default for every other kind.
    const ChartSection& section = m_song.Sections()[sectionIndex];
    LearnMode mode =
        section.kind == SectionKind::Learn ? m_song.Clips()[section.clipIndex].Mode() : LearnMode::Pass;
    m_currentInstance = SectionInstance(sectionIndex, mode);
    m_phase = GamePhase::Learning;

    // Section 0 defines the song's beat 0 (see ChartSong::BeginPlaythrough).
    if (sectionIndex == 0)
    {
        m_song.BeginPlaythrough(scheduledBeat * m_song.SecondsPerBeat());
    }

    // A Break's advance fired since the last BeginSection call; re-anchor the origin here against
    // this call's scheduledBeat (see m_arrangementResetPending).
    if (m_arrangementResetPending)
    {
        m_arrangementResetPending = false;
        double nowSeconds = scheduledBeat * m_song.SecondsPerBeat();
        m_song.SetOriginBeat(std::llround(m_song.SecondsToAbsoluteBeat(nowSeconds)));
    }

    // Start whatever the previous section queued (if it was [background]) before this section's own
    // logic runs, so the two play in parallel. A background clip then loops indefinitely until a
    // later [break]/[reset] silences it; loop_count has no effect on it.
    if (m_queuedBackground.clipIndex >= 0)
    {
        int bgClipIndex = m_queuedBackground.clipIndex;
        m_queuedBackground = QueuedBackground{};
        StartClipLoop(bgClipIndex, m_song.Clips()[bgClipIndex].Volume());
    }

    switch (section.kind)
    {
        case SectionKind::Background:
        {
            // Never blocks, never occupies "current" for judging - queue its clip for the next
            // BeginSection and fall straight through to that section.
            m_queuedBackground = QueuedBackground{section.clipIndex, section.loopCount};
            int nextIndex = sectionIndex + 1;
            if (nextIndex < static_cast<int>(m_song.Sections().size()))
            {
                BeginSection(nextIndex, scheduledBeat);
            }
            else
            {
                // Last section was [background] - its queued clip never starts; acceptable.
                m_phase = GamePhase::Complete;
            }
            return;
        }

        case SectionKind::Break:
        {
            const ChartClip& clip = m_song.Clips()[section.clipIndex];

            // StopAllExcept this clip, not StopAll(): it's about to be restarted fresh by
            // StartClipLoop below anyway, and stopping it here too would reflush the same voice a
            // second time, widening the window for a fragment of its stale audio to bleed through.
            m_audioEngine.StopAllExcept(m_stemHandles[section.clipIndex]);
            for (auto& entry : m_clipInstances)
            {
                if (entry.first != &clip)
                {
                    entry.second.MarkStopped();
                }
            }
            // The same c_NoteFallBeats lead-time guarantee the Learn case gives: without it a
            // short break can hand off with the next note's beat an instant away, so its preview
            // lands past the judge line. ChartClip::ComputeBreakAdvance has the extended-loop math.
            double secondsPerBeat = m_song.SecondsPerBeat();
            double tFallSeconds = c_NoteFallBeats * secondsPerBeat;

            // scheduledBeat, not a live clock read - ComputeLoopFloorSeconds asserts this is
            // exactly one of the clip's loop boundaries, so it needs the deterministic instant.
            double nowSeconds = scheduledBeat * secondsPerBeat;

            // A Break unconditionally re-anchors the bar-alignment origin to this instant.
            // ValidateArrangementAlignment guarantees it's already one of the clip's loop
            // boundaries, so this doesn't move where a still-playing voice's pattern is judged to be.
            m_song.SetOriginBeat(std::llround(m_song.SecondsToAbsoluteBeat(nowSeconds)));
            double originSeconds = nowSeconds;

            ClipInstance& instance = m_clipInstances[&clip];
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
            instance.SetContext(clip, originSeconds, stemDuration);

            ChartClip::BreakAdvance advance = instance.ComputeBreakAdvance(nowSeconds, section.loopCount, tFallSeconds);

            // isPlaying may still be true (left alone above) - flip it false so StartClipLoop still
            // performs its one necessary restart to apply this break's own (possibly finite) loop
            // count, rather than early-returning as "already playing".
            instance.MarkStopped();

            // A break clip never outlives its section, so its known loop count goes straight to
            // StartClipLoop: the voice stops itself sample-accurately once its loops finish,
            // instead of relying only on the polled StopClipLoop() below.
            StartClipLoop(section.clipIndex, clip.Volume(), advance.loopCount);
            // Measured from the deterministic nowSeconds, not StartClipLoop's live-clock loop start.
            m_currentInstance.SchedulePendingAdvance(advance.advanceSeconds);
            return;
        }

        case SectionKind::Reset:
        {
            // No clip and no screen time - stop whatever's looping and fall straight through to
            // the next section. Not a pause: no elapsed time between this and the next section.
            m_audioEngine.StopAll();
            for (auto& entry : m_clipInstances)
            {
                entry.second.MarkStopped();
            }
            // Unconditionally re-anchors the bar-alignment origin to this instant.
            double nowSeconds = scheduledBeat * m_song.SecondsPerBeat();
            m_song.SetOriginBeat(std::llround(m_song.SecondsToAbsoluteBeat(nowSeconds)));

            int nextIndex = sectionIndex + 1;
            if (nextIndex < static_cast<int>(m_song.Sections().size()))
            {
                BeginSection(nextIndex, scheduledBeat);
            }
            else
            {
                m_phase = GamePhase::Complete;
            }
            return;
        }

        case SectionKind::Learn:
        {
            const ChartClip& clip = m_song.Clips()[section.clipIndex];
            double secondsPerBeat = m_song.SecondsPerBeat();
            // scheduledBeat, not a live clock read, so this matches the instant
            // PreviewFirstOnsetBeatForLane predicted and agrees exactly with sectionStartSeconds below.
            double nowSeconds = scheduledBeat * secondsPerBeat;

            // A Learn never moves the origin, so StartClipLoop's read moments later sees this same value.
            ClipInstance& instance = m_clipInstances[&clip];
            double originSeconds = m_song.OriginSeconds();
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
            instance.SetContext(clip, originSeconds, stemDuration);

            // One formula covers both a clip's first appearance and a later reuse mid-groove: the
            // bar-alignment invariant guarantees scheduledBeat lands on one of this clip's cycle
            // boundaries, so querying "next onset after (scheduledBeat - epsilon)" returns its
            // pattern's true first note on a fresh join.
            for (int lane = 0; lane < c_LaneCount; ++lane)
            {
                m_currentInstance.SetNextExpectedBeat(
                    lane, instance.NextOnsetAfter(m_song.Bpm(), scheduledBeat - c_FreshJoinEpsilonBeats, lane));
            }

            // Starts and schedules its advance immediately, like Break - the section advances on
            // this fixed schedule whether or not the player passes (passing only affects the
            // glow/volume treatment and, in Pass mode, the 3-miss penalty).
            double tFallSeconds = c_NoteFallBeats * secondsPerBeat;

            StartClipLoop(section.clipIndex, clip.InitVolume());
            // A Learn always starts its clip fresh, so its loop start is nowSeconds itself
            // (see ComputeLearnAdvanceSeconds).
            m_currentInstance.SchedulePendingAdvance(
                instance.ComputeLearnAdvanceSeconds(nowSeconds, section.loopCount, tFallSeconds));

            // Now that this section's onsets are established, replay any early-buffered presses.
            // Must run after SchedulePendingAdvance (a buffered press completing easy mode's
            // hits_required needs one already scheduled).
            ConsumeBufferedPresses(section, clip);
            return;
        }
    }
}

void GameSession::RegisterHit(int lane, bool wasPrecise)
{
    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.Clips()[section.clipIndex];

    int multiplierBefore = MultiplierForStreak(m_streakTracker.Streak());
    m_bank += wasPrecise ? c_PrecisePoints : c_ImprecisePoints;
    PushHudChanged(HudField::Bank, m_bank);

    bool newlyPassing = m_currentInstance.RegisterHit(clip.HitsRequired(), m_streakTracker);

    int multiplierAfter = MultiplierForStreak(m_streakTracker.Streak());
    if (multiplierAfter != multiplierBefore)
    {
        PushHudChanged(HudField::Multiplier, multiplierAfter);
        if (multiplierAfter > multiplierBefore)
        {
            PushSfx(SfxCue::MultiplierUp);
        }
    }

    if (newlyPassing)
    {
        m_audioEngine.SetVolume(m_stemHandles[section.clipIndex], static_cast<float>(clip.Volume()));

        // The section just (re-)started passing - eagerly extend its scheduled advance now if it
        // no longer leaves the next section's preview a full c_NoteFallBeats before hand-off, so
        // PendingAdvanceAtSeconds() is correct the instant anything reads it (NoteLaneModel's
        // early-handoff math starts trusting it the moment IsPassing() flips). A reactive fix in
        // Update() would leave a window where the note lane shows the next notes early then snaps back.
        double now = m_clock.ElapsedSeconds();
        double secondsPerBeat = m_song.SecondsPerBeat();
        double tFallSeconds = c_NoteFallBeats * secondsPerBeat;
        double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
        double extendedAdvance = ChartClip::ExtendAdvanceForFallLeadTime(m_currentInstance.PendingAdvanceAtSeconds(),
                                                                            now, stemDuration, tFallSeconds);
        if (extendedAdvance != m_currentInstance.PendingAdvanceAtSeconds())
        {
            m_currentInstance.SchedulePendingAdvance(extendedAdvance);
        }

        // Pass mode only - seed every lane's auto-score cursor to now, so Update()'s post-lock-in
        // auto-accrual walks from this instant, not a stale value.
        if (clip.Mode() == LearnMode::Pass)
        {
            double lockInBeat = now / secondsPerBeat;
            for (double& cursor : m_autoScoreCursorBeat)
            {
                cursor = lockInBeat;
            }
        }
    }
    StartClipLoop(section.clipIndex, clip.InitVolume());

    m_lastJudgement = JudgementResult::Hit;
    m_judgementEvents.push_back({JudgementResult::Hit, lane, m_currentInstance.IsPassing(), wasPrecise});
}

void GameSession::StartClipLoop(int clipIndex, double volume, int finiteLoopCount)
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.Clips().size()))
    {
        return;
    }
    const ChartClip* clip = &m_song.Clips()[clipIndex];
    ClipInstance& instance = m_clipInstances[clip];
    if (instance.IsPlaying())
    {
        return;
    }

    // Read the clock once and reuse it for the phase seek and the recorded loop-start time.
    // StartLooping below does real XAudio2 work, so reading the clock afterward would record a
    // loop-start later than the phase-seeked instant, biasing every later GetPositionSeconds() calc.
    StemHandle handle = m_stemHandles[clipIndex];
    double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
    double nowSeconds = m_clock.ElapsedSeconds();
    // Always well-defined here - BeginSection(0, ...) already called m_song.BeginPlaythrough().
    double originSeconds = m_song.OriginSeconds();
    instance.SetContext(*clip, originSeconds, stemDuration);
    // ComputeClipPhaseSeconds, not a raw fmod against stemDuration: the audio phase-aligns to the
    // judged beat grid (clip.SpanBeats()), not the file's measured length, or the two drift apart.
    double phaseSeconds = instance.ComputeClipPhaseSeconds(nowSeconds, m_song.Bpm());
    m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(volume), finiteLoopCount);
    instance.MarkStarted(nowSeconds);
}

double GameSession::ArrangementOriginBeat() const
{
    return m_song.OriginSeconds() / m_song.SecondsPerBeat();
}

// Stops clipIndex's stem if it's playing.
void GameSession::StopClipLoop(int clipIndex)
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.Clips().size()))
    {
        return;
    }
    auto it = m_clipInstances.find(&m_song.Clips()[clipIndex]);
    if (it == m_clipInstances.end() || !it->second.IsPlaying())
    {
        return;
    }
    m_audioEngine.Stop(m_stemHandles[clipIndex]);
    it->second.MarkStopped();
}

void GameSession::RegisterMiss(int lane)
{
    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.Clips()[section.clipIndex];

    int multiplierBefore = MultiplierForStreak(m_streakTracker.Streak());
    SectionInstance::MissResult result = m_currentInstance.RegisterMiss(m_easyMode, m_streakTracker);
    if (result.shouldStopClip)
    {
        StopClipLoop(section.clipIndex);

        // The shared streak tripped - wipe the bank (an isolated miss never does this, only the
        // full trip). Already-paid-out score (m_score) is untouched.
        if (m_bank > 0)
        {
            PushSfx(SfxCue::StreakBroken);
        }
        m_bank = 0;
        PushHudChanged(HudField::Bank, m_bank);
        int multiplierAfter = MultiplierForStreak(m_streakTracker.Streak());
        if (multiplierAfter != multiplierBefore)
        {
            PushHudChanged(HudField::Multiplier, multiplierAfter);
        }
    }
    if (result.justEnteredFailState)
    {
        m_audioEngine.SetVolume(m_stemHandles[section.clipIndex], static_cast<float>(clip.InitVolume()));
    }

    // Once a Pass-mode section is passing, a miss is a genuine no-op, so it shouldn't produce a
    // judgement event either.
    if (result.wasNoOpAlreadyPassing)
    {
        return;
    }

    m_lastJudgement = JudgementResult::Miss;
    m_judgementEvents.push_back({JudgementResult::Miss, lane, m_currentInstance.IsPassing()});
}

void GameSession::AdvanceExpectedNote(int lane)
{
    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.Clips()[section.clipIndex];
    double originBeat = ArrangementOriginBeat();
#ifdef RHYTHM_DEBUG_JUDGEMENTS
    double before = m_currentInstance.NextExpectedBeatForLane(lane);
#endif
    m_currentInstance.AdvanceExpectedNote(lane, originBeat, clip);
#ifdef RHYTHM_DEBUG_JUDGEMENTS
    std::fprintf(stderr, "[AdvanceExpectedNote] t=%.4f lane=%d section=%d clip=%d origin=%.4f before=%.4f after=%.4f\n",
                 m_clock.ElapsedSeconds(), lane, m_currentInstance.SectionIndex(), section.clipIndex, originBeat,
                 before, m_currentInstance.NextExpectedBeatForLane(lane));
#endif
}

double GameSession::EffectiveStartToleranceSeconds(const ChartClip& clip) const
{
    double toleranceMs = clip.StartToleranceMs();
    if (m_easyMode)
    {
        toleranceMs *= c_EasyModeToleranceMultiplier;
        auto it = m_clipInstances.find(&clip);
        bool clipPlaying = it != m_clipInstances.end() && it->second.IsPlaying();
        if (!clipPlaying)
        {
            toleranceMs *= c_EasyModeStoppedToleranceMultiplier;
        }
    }
    return toleranceMs / 1000.0;
}

// One full bar at the song's own tempo/time signature.
double GameSession::CountInSeconds() const
{
    double secondsPerBeat = m_song.SecondsPerBeat();
    return m_song.BeatsPerBar() * secondsPerBeat;
}

// Returns the lane note whose phase-within-span matches absoluteStartBeat's phase, or nullptr if none does.
const LaneNote* GameSession::FindLaneNote(const ChartClip& clip, int lane, double originBeat,
                                           double absoluteStartBeat) const
{
    double span = clip.SpanBeats();
    double phase = std::fmod(absoluteStartBeat - originBeat, span);
    if (phase < 0.0)
    {
        phase += span;
    }

    for (const LaneNote& note : clip.LaneNotes(lane))
    {
        if (std::abs(note.startBeat - phase) < 1e-6)
        {
            return &note;
        }
    }
    return nullptr;
}
