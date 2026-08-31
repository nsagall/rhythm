#include "GameSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// Easy mode's start-tolerance widening - see EffectiveStartToleranceSeconds.
constexpr double c_EasyModeToleranceMultiplier = 1.5;        // Applied unconditionally.
constexpr double c_EasyModeStoppedToleranceMultiplier = 2.0; // Applied on top, only while the clip isn't playing.

// How far the judging clock is allowed to drift from the audio hardware's playback
// position before Update() resyncs it - see Update()'s own resync block.
constexpr double c_ClockResyncThresholdSeconds = 0.008;

// Points a single hit pays into the bank - flat, no combo scaling; MultiplierForStreak
// rewards sustained accuracy instead, applied once at payout.
constexpr int c_PrecisePoints = 10;
constexpr int c_ImprecisePoints = 5;

// A press beyond this fraction of the effective start tolerance is judged imprecise, not precise.
constexpr double c_ImprecisionToleranceFraction = 0.5;

// Streak-length breakpoints for MultiplierForStreak: 0-9 -> x1, 10-19 -> x2, 20-29 -> x3, 30+ -> x4.
constexpr int c_MultiplierTierStreaks[] = {10, 20, 30};

} // namespace

// See the header's own comment.
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

// Binds this session to the audio engine it will drive.
GameSession::GameSession(AudioEngine& audioEngine) : m_audioEngine(audioEngine)
{
}

// Parses a chart and loads its clips' audio into the audio engine.
//   chartFilePath - path to the .chart file to load.
//   easyMode      - simplifies every clip's pattern and eases judging tolerances when true.
//   outError      - filled with one message per problem found if loading fails.
// Returns false if the chart is invalid or a stem fails to load.
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
    // Captured before ExpandLaneNotesToFillClip (below) can widen any clip's
    // spanBeats to its real audio length - ValidateArrangementAlignment
    // needs each clip's own AUTHORED bar-aligned length (ChartSong::Load's
    // AlignToBarBoundary output), not that widened one, to check other
    // clips' lengths against - see ChartClip::ClipAlignmentInfo's own
    // comment for why those are two different things.
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

        // A clip with no .mid file (hasMidi == false) has no pattern to
        // validate against the stem's length or tile to fill it - it's
        // only ever played back whole (break/background), never judged.
        if (clip.HasMidi())
        {
            if (easyMode)
            {
                clip.ApplyEasyModeTransform(song.Bpm());
            }

            double stemDuration = m_audioEngine.GetStemDurationSeconds(handle);
            // The reverse of the "MIDI shorter than the audio" case (which
            // ExpandLaneNotesToFillClip below handles by tiling): if the
            // pattern doesn't even fit in a single loop of the stem, the
            // audio would already have wrapped back to its start before
            // the pattern's own last notes are reached - notes get judged
            // against a moment the audio isn't actually at anymore. Not a
            // crash, just silently wrong, so it's rejected at load time
            // instead of shipped. Shared with the editor's analytical
            // block scheduler (ChartClip::ClipFitsOneLoop), so both
            // reject exactly the same charts.
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

    // The whole-chart bar-alignment invariant ChartClip's own class
    // comment describes - checked once here, with real stem durations, so a
    // chart that could only misbehave at runtime (or only for an unlucky
    // player - see ChartClip::ValidateArrangementAlignment's own comment)
    // is refused up front instead.
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

// Starts gameplay from the beginning of the loaded chart.
void GameSession::Start()
{
    if (m_song.Sections().empty())
    {
        return;
    }

    // A previous run may have left clips passing and still looping
    // (that's by design while a song is in progress/just completed - each
    // passing loop keeps playing to build up the full arrangement), so a
    // fresh start has to stop them explicitly or they'd keep playing
    // underneath the new run.
    m_audioEngine.StopAll();
    m_currentInstance = SectionInstance();
    // Clears every clip's isPlaying - the bar-alignment origin re-anchors on
    // its own the moment BeginSection reaches section 0 again (see
    // ChartSong::BeginPlaythrough), so there's nothing to reset here.
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

// Stops all playback and returns to Idle.
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

// See the header comment.
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

// See the header comment.
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

// See the header comment.
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

// See the header comment.
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
    // Once a Pass-mode section has locked in, Update()'s own post-lock-in
    // auto-accrual becomes the section's sole scorer for every remaining
    // note (see its own comment) - a real press reaching RegisterHit/
    // RegisterMiss again from here on would double-count against the same
    // notes the auto-walk is already crediting. DontFail is unaffected -
    // its passing is reversible, so the player must keep actually playing
    // to hold onto it.
    if (clip.Mode() == LearnMode::Pass && m_currentInstance.IsPassing())
    {
        return false;
    }
    return true;
}

// Registers a key-down for lane; judges it against that lane's next expected note if the current section is learning.
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
        // Too early to judge against this note at all - silently ignored
        // rather than failed immediately, so a stray early tap doesn't
        // cost the player a note they haven't actually reached yet. The
        // lane keeps awaiting this same note exactly as if the press never
        // happened: a real press later, on time, still resolves it
        // normally, and one that never comes still times out via Update()'s
        // own press-phase timeout, same as always.
    }
    else
    {
        // Too late: fails immediately, but doesn't advance - this lane keeps
        // awaiting the same note until it's hit correctly or times out,
        // exactly like a mistimed tap did in the single-lane model.
        RegisterMiss(lane);
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
    }
}

// See the header comment.
void GameSession::ApplyInTolerancePress(int lane, const ChartSection& section, const ChartClip& clip,
                                         double startBeat, double pressSeconds)
{
    double secondsPerBeat = m_song.SecondsPerBeat();
    double startSeconds = startBeat * secondsPerBeat;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
    double originBeat = ArrangementOriginBeat();
    const LaneNote* note = FindLaneNote(clip, lane, originBeat, startBeat);
    double durationBeats = note ? note->durationBeats : 0.0;

    // How close this press landed to the note's own onset, as a fraction of
    // the tolerance window it was judged against - past
    // c_ImprecisionToleranceFraction of it, still a correct press (it's
    // within the full window), but not a precise one. See
    // SectionInstance::LaneHoldWasPrecise/GameSession::RegisterHit.
    bool wasPrecise = std::abs(pressSeconds - startSeconds) <= toleranceSeconds * c_ImprecisionToleranceFraction;

    StartClipLoop(section.clipIndex, clip.InitVolume());
    m_currentInstance.StartLaneHold(lane, startBeat, startBeat + durationBeats, wasPrecise);
    AdvanceExpectedNote(lane);

    if (m_easyMode)
    {
        // Release timing is ignored entirely in easy mode, so the press
        // itself is the final judgement - mirrors OnRelease's in-tolerance
        // branch below, the only other place a Hit gets registered.
        RegisterHit(lane, wasPrecise);
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Hit);
    }
    else
    {
        // A correct press doesn't produce a final judgement yet - that only
        // happens at release - so any stale Hit/Miss left over from an
        // earlier press/release must be cleared here, or the caller's very
        // next ConsumeLastJudgement() would misattribute it to this press.
        m_lastJudgement = JudgementResult::None;
    }
}

// See the header comment.
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

// See the header comment.
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
        // Re-validated against the note's real, just-established onset,
        // not trusted from whenever it was buffered - see the header
        // comment on why the real onset can end up later than it looked
        // back then.
        if (std::abs(buffered.pressSeconds - startSeconds) > toleranceSeconds)
        {
            continue;
        }

        ApplyInTolerancePress(lane, section, clip, startBeat, buffered.pressSeconds);

        if (buffered.released && m_currentInstance.IsLaneHeld(lane))
        {
            // The player had already let go before this section even
            // began (a very fast anticipatory tap) - resolve that release
            // now too, against the hold ApplyInTolerancePress just
            // started, exactly as OnRelease would have. In easy mode the
            // press alone already produced the final judgement (see
            // ApplyInTolerancePress), so this just lets go of the hold,
            // mirroring OnRelease's own easy-mode branch.
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

// Registers a key-up for lane; judges it against the note that lane was holding, if any.
void GameSession::OnRelease(int lane)
{
    if (lane < 0 || lane >= c_LaneCount || !m_currentInstance.IsLaneHeld(lane) || m_currentInstance.SectionIndex() < 0)
    {
        // Not holding anything right now - but if a press was just buffered
        // for this lane (see TryBufferEarlyPress) and hasn't been consumed
        // yet, remember this release too, so a fast tap-and-release just
        // ahead of a section transition resolves the same way it would if
        // the section had already begun, instead of leaving
        // ConsumeBufferedPresses to start a hold with no release to ever
        // resolve it.
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
        // Already judged Hit at press time - release timing is ignored
        // entirely, so releasing (whenever it happens) just lets go of the
        // hold and produces no judgement of its own.
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

// Advances count-in/miss-detection/hold-timeout timing; call once per frame.
void GameSession::Update()
{
    if (m_paused)
    {
        return;
    }

    // Keep the judging clock locked to actual audio playback. m_clock is a
    // free-running CPU timer, while the music is actually driven by
    // XAudio2's own hardware clock - the two can slowly drift apart over a
    // long-looping clip (different oscillators, buffer scheduling, etc.),
    // which would otherwise show up as hit judging (and the falling notes)
    // slipping out of sync with what's actually audible. Every currently-
    // playing clip is started phase-aligned to the same shared beat grid
    // (see StartClipLoop), so the current section's own clip - if it has
    // one and it's already playing, true for Learn and Break alike - is an
    // equally valid drift reference regardless of which section is active.
    // Also requires AudioEngine::IsPlaying(): a break section's clip plays a
    // *finite* loop_count, and once that last pass finishes XAudio2 stops
    // the voice on its own - the voice's isPlaying stays true until the
    // section's own scheduled advance explicitly stops it (below), but
    // GetPositionSeconds() has already frozen at that point instead of
    // still advancing. Without this check, resyncing against that frozen
    // position every tick pins the clock to that one instant forever, so
    // "now" can never reach the scheduled advance time - the whole game
    // hangs the moment a break section's clip finishes playing.
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

    // Held-past-late-release timeout: deliberately independent of
    // phase/pending-advance, so a hold already in flight keeps resolving
    // even if the section has since started passing, instead of being
    // abandoned mid-air. Lane holds structurally only ever populate during
    // a Learn section, but the play-mode check is kept anyway as defense-in-depth.
    // Skipped entirely in easy mode: a hold there is already judged Hit at
    // press time (see OnPress), so there's nothing left to time out - it
    // just sits active until the real key-up, which OnRelease resolves as
    // a no-op.
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
        // The count-in is bar-aligned, so it can't land mid-pattern - whatever the first
        // section anchors to at this boundary always has its full start-tolerance window ahead of it.
        CatchUpCountIn();
        return;
    }

    if (m_phase == GamePhase::Learning)
    {
        if (m_currentInstance.HasPendingAdvance() && now >= m_currentInstance.PendingAdvanceAtSeconds())
        {
            const ChartSection& finishedSection = m_song.Sections()[m_currentInstance.SectionIndex()];

            // A learn section that's reached its own candidate advance
            // without locking in doesn't get abandoned - it hasn't proven
            // itself yet, so it just repeats: push the candidate advance
            // back by exactly one more full loop (mirrors
            // ChartClip::ComputeLearnAdvanceSeconds/ComputeBreakAdvance's
            // own "extend by whole loops" logic, just applied reactively
            // here instead of decided once up front, since whether it's
            // needed at all depends on the player) and check again next
            // time this same instant is reached. The clip's own audio was
            // already looping continuously the whole time (StartClipLoop's
            // finiteLoopCount==0 for a learn clip), so nothing about
            // playback itself needs to restart - only the section's own
            // "am I allowed to leave yet" decision repeats.
            if (finishedSection.kind == SectionKind::Learn && !m_currentInstance.IsPassing())
            {
                double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[finishedSection.clipIndex]);
                m_currentInstance.ExtendPendingAdvance(stemDuration);
                return;
            }

            m_currentInstance.ClearPendingAdvance();

            // The section genuinely finished (a passing Learn section, or a
            // Break's own wait elapsed) - pay out whatever's in the bank,
            // multiplied by the streak multiplier in effect right now, into
            // the permanent total (see RegisterHit/RegisterMiss/
            // CurrentScore()/CurrentBank()), then reset the streak itself -
            // a payout is the other trigger that zeroes m_streakTracker,
            // distinct from (and unconditional on) the 3-miss trip inside
            // RegisterMiss. Both bank and streak are left at 0 once this
            // runs, so the next section always starts its own build-up from
            // a clean slate, at the base x1 multiplier.
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

            // A learn section reaching here is always passing (see
            // above) - its clip already switched from init_volume to
            // volume back in RegisterHit, and keeps playing by design (to
            // build up the arrangement), so there's nothing to do for it
            // here, unlike break below.
            if (finishedSection.kind == SectionKind::Break)
            {
                // Unlike a passing learn clip (which keeps playing by
                // design, to build up the arrangement), a break section
                // is a one-off scripted interlude - stop it once its own
                // loop_count wait completes so it doesn't drone on
                // underneath every subsequent section until the next
                // break/reset's StopAll() happens to kill it. (Reset can
                // never be finishedSection here - it never sets a pending
                // advance in the first place, see BeginSection.)
                //
                // If the very next section immediately re-queues this exact
                // clip as a background layer (a common chart idiom - "keep
                // this riff going after its scripted break moment", e.g.
                // Byte Blaster's [break] clip=arp -> [background] clip=arp),
                // this plain stop-then-restart already comes out
                // phase-continuous on its own: it restarts against the
                // origin BeginSection re-anchors immediately below (see
                // m_arrangementResetPending's own comment), with the
                // default infinite loop count, replacing this break's own
                // finite one - and since that re-anchor lands on exactly
                // the same instant this restart happens at, it's trivially
                // still its own loop boundary (phase 0), which - because
                // the break's own now-finished duration was already an
                // exact multiple of this same clip's own length - is
                // exactly the same audible position an origin left
                // untouched would have given.
                StopClipLoop(finishedSection.clipIndex);

                // A Break implicitly ends with a Reset: by this point
                // everything it left playing is stopped (StopAllExcept
                // already silenced everything else back when the Break
                // itself began; the line above just stopped its own clip
                // too), so the bar-alignment origin needs to re-anchor here
                // too, exactly as an explicit [reset] would (see
                // ChartSong::OriginBeat()'s own comment). Without this,
                // whatever picks up next only lands on a shared loop
                // boundary if the break's own played duration (its
                // loop_count, possibly tFallSeconds-extended) happens to be
                // a whole multiple of that next clip's own length - which
                // nothing guarantees (a confirmed real repro: "A Real Good
                // Time"'s break_1 - one loop, ~2.53s - followed by
                // [background] you_be_dead -> [background] drums ->
                // [learn] bass, whose own ~10.11s length isn't a multiple
                // of that 2.53s at all). Deferred to BeginSection rather
                // than done right here - see m_arrangementResetPending's
                // own comment for why.
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

        // Press-phase timeout: any lane still awaiting a press whose window
        // has closed. Learn-only. Runs even while a pending advance is
        // already scheduled (the post-lock-in extension, before the
        // section actually advances above) - keeps notes timing out/
        // getting judged instead of freezing the instant the streak
        // requirement is met.
        const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
        if (section.kind == SectionKind::Learn)
        {
            const ChartClip& clip = m_song.Clips()[section.clipIndex];
            if (clip.Mode() == LearnMode::Pass && m_currentInstance.IsPassing())
            {
                // Locked in, Pass mode: IsLaneJudgeable already keeps real
                // presses from reaching RegisterHit/RegisterMiss again (see
                // its own comment), so this is the section's sole scorer
                // from here to its own advance - walk every lane forward
                // from its own auto-score cursor and credit each note newly
                // crossed as a precise hit, exactly "as though the player is
                // playing perfectly for the remainder of the section."
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

// Returns the audio engine stem handle for a clip, for debugging.
StemHandle GameSession::DebugStemHandle(int clipIndex) const
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_stemHandles.size()))
    {
        return StemHandle{};
    }
    return m_stemHandles[clipIndex];
}

// Returns the clip the current section refers to, or nullptr if there's no current section or it's a reset.
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

// Returns the current section's kind, or Learn as a harmless default if there's no current section.
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

// See the header's own comment.
int GameSession::CurrentScore() const
{
    return m_score;
}

// See the header's own comment.
int GameSession::CurrentBank() const
{
    return m_bank;
}

// See the header's own comment.
int GameSession::CurrentMultiplier() const
{
    return MultiplierForStreak(m_streakTracker.Streak());
}

// See the header's own comment.
int GameSession::ScoringStreak() const
{
    return m_streakTracker.Streak();
}

// Returns the beat of the next note this lane is awaiting a press for.
double GameSession::NextExpectedBeatForLane(int lane) const
{
    return m_currentInstance.NextExpectedBeatForLane(lane);
}

// See its own header comment.
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

// See the header comment.
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

    // A learn section that isn't currently passing doesn't know when it'll
    // really advance - it might repeat any number of further loops first
    // (see Update()'s own comment) - so there's nothing legitimate to
    // preview yet: showing the *real* next clip's notes here would just
    // have to be retracted the moment the section turns out to repeat
    // instead of advancing. The clip's own notes simply keep scrolling by
    // themselves in the meantime (see CurrentClip()), no preview needed. A
    // break section always knows its own advance exactly up front
    // (loop_count is fixed, not gated on any player performance), so it
    // isn't held back by this at all.
    const ChartSection& current = m_song.Sections()[sectionIndex];
    if (current.kind == SectionKind::Learn && !m_currentInstance.IsPassing())
    {
        return -1;
    }

    // Applies uniformly whether the current section is Learn-awaiting-
    // advance or Break-awaiting-advance - both set a pending advance the
    // same way, so a break section's own hold is the natural place to
    // preview the *next* learn section's dots, exactly like a learn
    // section's own hold already was. Reset never reaches here at all -
    // it never persists as current (see NextPersistentSectionAtOrAfter).
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

// See the header's own comment.
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

    // PreviewClipOriginBeat() already resolves to transitionBeat itself
    // whenever this preview would start a fresh arrangement (see its own
    // comment), so this is correct whether preview is joining fresh or
    // continuing an already-open groove - both cases want the first onset
    // at/after the instant this section actually begins.
    return preview.NextOnsetAfter(originBeat, transitionBeat - c_FreshJoinEpsilonBeats, lane);
}

// See its own header comment.
double GameSession::PreviewClipOriginBeat() const
{
    int idx = PreviewSectionIndex();
    if (idx < 0)
    {
        return -1.0;
    }
    double secondsPerBeat = m_song.SecondsPerBeat();
    // During the count-in, m_song's playthrough hasn't begun yet -
    // BeginPlaythrough() only runs once BeginSection actually reaches
    // section 0 (see its own comment) - so there's no established origin to
    // inherit yet, same as a Reset lying between here and idx.
    if (m_phase != GamePhase::CountIn && !ArrangementResetsBeforeSection(idx))
    {
        return ArrangementOriginBeat();
    }
    if (m_phase == GamePhase::CountIn)
    {
        // The playthrough hasn't started yet - BeginPlaythrough() (unlike a
        // mid-song Reset, see below) sets the origin directly from this same
        // value, with no rounding, so the raw prediction already matches
        // exactly what will actually happen.
        return PreviewTransitionSeconds() / secondsPerBeat;
    }
    // A Reset between here and idx re-anchors the origin to a whole beat
    // near PreviewTransitionSeconds() (Reset never adds elapsed time - see
    // BeginSection's own Reset case - but does round to the nearest whole
    // beat, per ChartSong::OriginBeat()'s own comment) - predict that same
    // rounded value here too, not the raw seconds-derived one, or a lane
    // whose pattern has a note exactly at phase 0 can end up on the wrong
    // side of a bar boundary between this prediction and the real thing
    // (confirmed real repro: "A Real Good Time"'s drums, lane 0 - previewed
    // correctly at its pattern's true first note, but once the section
    // actually began, ChartClip::NextOnsetAfter's own barIndex landed one
    // bar later for real, silently skipping to the pattern's *second* note
    // instead - so the real first note's own Hit judgement was recorded
    // against a beat nothing on screen matched, and it vanished the instant
    // it was pressed).
    long long predictedOriginBeat = std::llround(m_song.SecondsToAbsoluteBeat(PreviewTransitionSeconds()));
    return m_song.AbsoluteBeatToSeconds(predictedOriginBeat) / secondsPerBeat;
}

// Returns and clears the most recent judgement (Hit/Miss/None).
JudgementResult GameSession::ConsumeLastJudgement()
{
    JudgementResult result = m_lastJudgement;
    m_lastJudgement = JudgementResult::None;
    return result;
}

// Returns and clears every judgement recorded since the last call - see the
// header's own comment.
std::vector<GameSession::JudgementEvent> GameSession::ConsumeJudgementEvents()
{
    std::vector<JudgementEvent> events = std::move(m_judgementEvents);
    m_judgementEvents.clear();
    return events;
}

// Returns and clears every HudChangeEvent recorded since the last call -
// see the header's own comment.
std::vector<GameSession::HudChangeEvent> GameSession::ConsumeHudChangeEvents()
{
    std::vector<HudChangeEvent> events = std::move(m_hudChangeEvents);
    m_hudChangeEvents.clear();
    return events;
}

// Returns and clears every SfxCue recorded since the last call - see the
// header's own comment.
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

// Returns how a specific lane note was judged, or None if untracked.
JudgementResult GameSession::OnsetJudgement(double startBeat, int lane) const
{
    return m_currentInstance.OnsetJudgement(startBeat, lane);
}

// See the header's own comment.
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

// See the header comment.
void GameSession::BeginSection(int sectionIndex, double scheduledBeat)
{
    // Resolved before constructing m_currentInstance, since the Learn
    // clip's own learnMode decides that instance's starting passing state
    // (see SectionInstance's own constructor comment) - irrelevant/unused
    // for every other section kind, where Pass is just a harmless default.
    const ChartSection& section = m_song.Sections()[sectionIndex];
    LearnMode mode =
        section.kind == SectionKind::Learn ? m_song.Clips()[section.clipIndex].Mode() : LearnMode::Pass;
    m_currentInstance = SectionInstance(sectionIndex, mode);
    m_phase = GamePhase::Learning;

    // Section 0 defines the song's own beat 0 - see ChartSong::
    // BeginPlaythrough's own comment.
    if (sectionIndex == 0)
    {
        m_song.BeginPlaythrough(scheduledBeat * m_song.SecondsPerBeat());
    }

    // A Break's own advance fired since the last BeginSection call - see
    // m_arrangementResetPending's own comment for why this re-anchor
    // happens here, against this call's own scheduledBeat, rather than
    // back in Update() where the flag was set.
    if (m_arrangementResetPending)
    {
        m_arrangementResetPending = false;
        double nowSeconds = scheduledBeat * m_song.SecondsPerBeat();
        m_song.SetOriginBeat(std::llround(m_song.SecondsToAbsoluteBeat(nowSeconds)));
    }

    // "The next section begins" is exactly this call - kick off whatever
    // the previous section (if it was `background`) queued, before this
    // section's own logic runs, so the two play out in parallel from here.
    // Once started, a background clip loops indefinitely - exactly like a
    // passing learn clip - until a later `break`/`reset` section's
    // StopAll() (or Stop()/Start()) silences it; loop_count has no effect
    // on background sections.
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
            // Never blocks, never itself occupies "current" for judging
            // purposes - queue its clip for the *next* BeginSection and
            // fall straight through to that next section immediately.
            m_queuedBackground = QueuedBackground{section.clipIndex, section.loopCount};
            int nextIndex = sectionIndex + 1;
            if (nextIndex < static_cast<int>(m_song.Sections().size()))
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

        case SectionKind::Break:
        {
            const ChartClip& clip = m_song.Clips()[section.clipIndex];

            // StopAllExcept this section's own clip, not StopAll() - if
            // that clip was already playing uninterrupted (e.g. as a
            // still-open background layer), it's about to be restarted
            // fresh by StartClipLoop below regardless - stopping it here too
            // would just mean StartLooping's own Stop()/FlushSourceBuffers()
            // stops-and-reflushes the SAME voice a second time moments
            // later for no benefit, widening the window for a fragment of
            // its own stale audio to bleed through before the fresh restart
            // takes hold (a confirmed real repro: "a tiny bit of the
            // beginning" of a still-playing clip's own track audible right
            // as its Break section begins). See AudioEngine::
            // StopAllExcept's own comment.
            m_audioEngine.StopAllExcept(m_stemHandles[section.clipIndex]);
            for (auto& entry : m_clipInstances)
            {
                if (entry.first != &clip)
                {
                    entry.second.MarkStopped();
                }
            }
            // Same c_NoteFallBeats guarantee a learn section's own advance
            // gives (see the Learn case below): without it, a short break
            // can hand off to the next learn section with its first note's
            // scheduled beat only an instant away, so the note lane's
            // preview lands it already at (or past) the judge line instead
            // of spawning at the top edge with its full travel time. See
            // ChartClip::ComputeBreakAdvance for the extended-loop-count
            // math (shared with the editor's analytical block scheduler).
            double secondsPerBeat = m_song.SecondsPerBeat();
            double tFallSeconds = c_NoteFallBeats * secondsPerBeat;

            // scheduledBeat, not a live clock read - exactly like the Learn
            // case below, and for the same reason: ComputeLoopFloorSeconds
            // (shared by both) now asserts this is already exactly one of
            // the clip's own loop boundaries rather than re-deriving it via
            // ceil(), so it needs the same jitter-free, deterministic
            // instant Learn uses, not whatever "now" happened to poll as by
            // the time this transition was actually processed.
            double nowSeconds = scheduledBeat * secondsPerBeat;

            // A Break always re-anchors the bar-alignment origin to this
            // exact instant, unconditionally - including when its own clip
            // (or a still-open background layer left alone above) was
            // already playing uninterrupted: ValidateArrangementAlignment
            // guarantees this instant is already one of that clip's own
            // loop boundaries, so re-anchoring here doesn't move where its
            // pattern is judged to be - it lands exactly where a
            // still-playing voice already was, not at a new sample 0.
            // Rounded to a whole beat - see ChartSong::OriginBeat()'s own
            // comment for why that's always exactly right, never a
            // fp-drift approximation.
            m_song.SetOriginBeat(std::llround(m_song.SecondsToAbsoluteBeat(nowSeconds)));
            double originSeconds = nowSeconds;

            ClipInstance& instance = m_clipInstances[&clip];
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
            instance.SetContext(clip, originSeconds, stemDuration);

            ChartClip::BreakAdvance advance = instance.ComputeBreakAdvance(nowSeconds, section.loopCount, tFallSeconds);

            // clip's own isPlaying may still be true here (left alone
            // above) - flip it false right before StartClipLoop so it still
            // performs its one necessary restart (an already-submitted
            // buffer's loop count can't be changed in place, and this break
            // needs its own, possibly finite, count applied) instead of
            // early-returning as "already playing, nothing to do".
            instance.MarkStopped();

            // Unlike a learn clip (which always loops forever - it might
            // still need to keep playing past this section's own end, if
            // passing), a break clip never outlives its own section, so
            // its now-known loop count is handed straight to StartClipLoop:
            // the voice stops itself naturally and sample-accurately once
            // its (possibly loop-count-extended) loops are done, instead of
            // relying solely on the polled StopClipLoop() call below to
            // catch the exact instant - which could otherwise let a
            // fraction of a second of the loop's beginning bleed through
            // first, especially audible at the very end of a chart where
            // nothing else is left playing to mask it.
            StartClipLoop(section.clipIndex, clip.Volume(), advance.loopCount);
            // Measured from the deterministic nowSeconds above, not
            // StartClipLoop's own (separate, live-clock-read) recorded loop
            // start - matches how the Learn case below does this for its
            // own advance floor.
            m_currentInstance.SchedulePendingAdvance(advance.advanceSeconds);
            return;
        }

        case SectionKind::Reset:
        {
            // No clip of its own and no screen time - just stops whatever's
            // currently looping (a still-open Background layer, or a
            // passing Learn clip) and falls straight through to the next
            // section immediately, exactly like Background above (whose
            // queued-clip-realize block at the top of this function
            // already handles any [background] section(s) that happen to
            // follow, with no special-casing needed here). Not a pause -
            // there's no elapsed time between this and the next section
            // beginning.
            m_audioEngine.StopAll();
            for (auto& entry : m_clipInstances)
            {
                entry.second.MarkStopped();
            }
            // Re-anchors the bar-alignment origin to this exact instant,
            // unconditionally - see ChartSong::OriginBeat()'s own comment
            // for why that's always safe (and always a whole beat).
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
            // scheduledBeat, not a live clock read, so this is exactly the
            // same instant PreviewFirstOnsetBeatForLane already predicted
            // (see its own comment) - and so this section's own origin and
            // its sectionStartSeconds (below) agree exactly, with no tiny
            // live-clock-read gap between them.
            double nowSeconds = scheduledBeat * secondsPerBeat;

            // Read here, ahead of the onset computation below, which needs
            // it right away - a Learn never moves the origin (only Reset/
            // Break do, see ChartSong::OriginBeat()'s own comment), so
            // StartClipLoop's own read moments later just sees this exact
            // same value.
            ClipInstance& instance = m_clipInstances[&clip];
            double originSeconds = m_song.OriginSeconds();
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
            instance.SetContext(clip, originSeconds, stemDuration);

            // A single formula covers both a clip's first-ever appearance
            // and a later section reusing one already mid-groove: the
            // chart-wide bar-alignment invariant (see ChartClip's own class
            // comment, checked at LoadChart) guarantees
            // scheduledBeat always lands exactly on one of this clip's own
            // cycle boundaries from originBeat, so querying "next onset
            // after (scheduledBeat - epsilon)" already returns its own
            // pattern's true first note whenever this is a fresh join.
            for (int lane = 0; lane < c_LaneCount; ++lane)
            {
                m_currentInstance.SetNextExpectedBeat(
                    lane, instance.NextOnsetAfter(m_song.Bpm(), scheduledBeat - c_FreshJoinEpsilonBeats, lane));
            }

            // Starts immediately and schedules its own advance right away
            // too, exactly like Break above - the section advances on this
            // fixed schedule whether or not the player ever passes (see
            // RegisterHit/RegisterMiss for what passing still does: purely
            // the glow/confetti/volume-switch treatment, and - in Pass mode
            // only - turning off the "3 misses stops the loop" penalty;
            // none of it affects this timing, which is already decided).
            double tFallSeconds = c_NoteFallBeats * secondsPerBeat;

            StartClipLoop(section.clipIndex, clip.InitVolume());
            // A Learn section always starts its own clip fresh (never joins
            // one still open from an earlier section - ValidateArrangementAlignment
            // rejects any chart where it would), so its loop start is just
            // nowSeconds itself - see ComputeLearnAdvanceSeconds's own doc comment.
            m_currentInstance.SchedulePendingAdvance(
                instance.ComputeLearnAdvanceSeconds(nowSeconds, section.loopCount, tFallSeconds));

            // Now that this section's own onsets are established, replay
            // whatever presses (see TryBufferEarlyPress) arrived early
            // enough to anticipate them - must run after
            // SchedulePendingAdvance above (a buffered press completing
            // easy mode's hits_required needs one already scheduled - see
            // RegisterHit's own extension logic).
            ConsumeBufferedPresses(section, clip);
            return;
        }
    }
}

// Records a hit: pays points into m_bank, registers with the streak tracker, and starts the clip if needed.
//   lane       - the lane the hit was judged on.
//   wasPrecise - whether the press was precise; scores fewer points when false.
// Flips the section to passing once hitsRequired is met - see the header comment on IsPassing().
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

        // The section just (re-)started passing - make sure its already-
        // scheduled candidate advance still leaves the next section's
        // preview (PreviewClip(), gated on IsPassing() - see
        // PreviewSectionIndex()) a full c_NoteFallBeats to actually show
        // before hand-off, extending it right now if not, rather than
        // waiting for Update() to notice later once "now" has already
        // caught up to a too-close advance instant. Fixing it up eagerly,
        // right here, keeps PendingAdvanceAtSeconds() always correct from
        // the instant it's read by anything - critically, NoteLaneModel's
        // own early-handoff math (BuildScene's c_PreviewNextClipBeforeHandoff
        // branch), which starts trusting PreviewClip()'s onsets the moment
        // IsPassing() flips true, same as PreviewClip() itself. A reactive
        // fix-up in Update() instead (extend only once "now" reaches the
        // stale instant) would leave a real window of frames where
        // PendingAdvanceAtSeconds() is still the old, too-close value -
        // during which the note lane would already start showing the next
        // section's notes early, then visibly snap back the instant
        // Update() finally corrects it. See ChartClip::
        // ComputeLearnAdvanceSeconds's own doc comment for why a candidate
        // advance is otherwise never revisited once scheduled.
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

        // Pass mode only (see IsLaneJudgeable/Update()'s own comment) - seed
        // every lane's auto-score cursor to right now, so Update()'s
        // post-lock-in auto-accrual starts walking from exactly this
        // instant instead of from whatever stale value it last held (e.g.
        // 0, or a previous section's own cursor position).
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

// See the header comment.
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
    // Always well-defined by this point - BeginSection(0, ...) already
    // called m_song.BeginPlaythrough() before any clip can ever start (see
    // its own comment), so there's nothing to lazily establish here.
    double originSeconds = m_song.OriginSeconds();
    instance.SetContext(*clip, originSeconds, stemDuration);
    // ChartClip::ComputeClipPhaseSeconds, not a raw fmod against
    // stemDuration - see its own doc comment for why: the audio needs to
    // phase-align to the judged notes' own beat grid (clip.SpanBeats()),
    // measured from the arrangement origin, not the audio file's own
    // measured length, or the two drift apart over many loops and a clip
    // reached late in a song can start audibly partway through (even near
    // the end of) its own pattern.
    double phaseSeconds = instance.ComputeClipPhaseSeconds(nowSeconds, m_song.Bpm());
    m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(volume), finiteLoopCount);
    instance.MarkStarted(nowSeconds);
}

// See its own header comment.
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

// Records a miss: registers with the streak tracker and stops the clip loop if it trips 3 in a row.
//   lane - the lane the miss was judged on.
// No-op in Pass mode once already passing. In DontFail mode, drops a passing section back to failing.
// In easy mode, each section's first miss is instead fully forgiven.
void GameSession::RegisterMiss(int lane)
{
    const ChartSection& section = m_song.Sections()[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.Clips()[section.clipIndex];

    int multiplierBefore = MultiplierForStreak(m_streakTracker.Streak());
    SectionInstance::MissResult result = m_currentInstance.RegisterMiss(m_easyMode, m_streakTracker);
    if (result.shouldStopClip)
    {
        StopClipLoop(section.clipIndex);

        // The shared streak just tripped - wipe whatever's built up in the
        // bank (see StreakTracker's own comment: an isolated miss never
        // does this, only the full trip does). Already-paid-out score from
        // earlier, finished sections (m_score) is untouched.
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

    // Once a Pass-mode section is already passing, a miss is a genuine
    // no-op there (see SectionInstance::RegisterMiss's own early-return),
    // not just for the streak/clip-stopping consequences above - so it
    // shouldn't produce a judgement event either.
    if (result.wasNoOpAlreadyPassing)
    {
        return;
    }

    m_lastJudgement = JudgementResult::Miss;
    m_judgementEvents.push_back({JudgementResult::Miss, lane, m_currentInstance.IsPassing()});
}

// See the header comment.
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

// See the header comment.
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

// ApplyEasyModeTransform lives on ChartClip itself.

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
