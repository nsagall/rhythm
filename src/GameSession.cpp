#include "GameSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// Easy mode's start-tolerance widening - see EffectiveStartToleranceSeconds.
// The chart's own declared tolerance is widened by this much unconditionally...
constexpr double c_EasyModeToleranceMultiplier = 1.5;
// ...and, on top of that, by this much more while the clip isn't currently
// playing (never started, or stopped after too many misses), to help the
// player get back on track.
constexpr double c_EasyModeStoppedToleranceMultiplier = 2.0;

// Easy mode's density-thinning targets, expressed as fixed millisecond
// targets and converted to beats via the clip's own song bpm (see
// EasyModeMsToBeats) rather than fixed beat counts, so felt difficulty
// stays consistent across this game's real ~77-150bpm content range
// instead of silently swinging with tempo - a fixed beat count is a much
// shorter real gap at a fast tempo than a slow one. Starting points only,
// expected to move after playtesting - see ApplyEasyModeTransform.
constexpr double c_EasyModePerLaneMinGapMs = 260.0;     // ~2.3 hits/sec cap within one lane
constexpr double c_EasyModeGlobalMinGapMs = 170.0;      // tighter cross-lane cap, catches fast lane-alternating patterns
constexpr double c_EasyModeNoteDurationFloorMs = 130.0; // a kept note is never shorter than this

// Float-safety-only margin (not a musical rest) so a kept note's duration
// can approach but never touch/overlap the next kept note in its own lane -
// a lane can hold at most one note at a time (ChartMidi's own overlap
// rejection at parse time relies on the same invariant), so an actual
// overlap here would be a correctness bug, not a taste issue.
constexpr double c_EasyModeSafetyEpsilonBeats = 1e-4;

// Below this, two kept notes (regardless of lane) are treated as
// simultaneous for chord-collapse purposes - a floating-point-safety
// epsilon, not a musical threshold (c_EasyModeGlobalMinGapMs above is
// always far bigger in practice; this is a defense-in-depth backstop).
constexpr double c_EasyModeChordEpsilonBeats = 1e-6;

// How far the judging clock (a free-running QueryPerformanceCounter timer)
// is allowed to drift from the actual audio hardware's playback position
// before Update() pulls it back in line - see the resync block there. Small
// enough to catch real drift promptly, but larger than the normal
// quantization noise from GetPositionSeconds() only advancing once per
// XAudio2 processing pass (~a few ms), so a perfectly healthy clock doesn't
// get re-anchored (and its otherwise-smooth motion subtly stair-stepped)
// every single frame over nothing.
constexpr double c_ClockResyncThresholdSeconds = 0.008;

// Points a single hit pays into the bank - flat, no combo scaling; the
// streak multiplier (GameSession::MultiplierForStreak) is what rewards
// sustained accuracy now, applied once at payout rather than per hit.
constexpr int c_PrecisePoints = 10;
constexpr int c_ImprecisePoints = 5;

// A press landing beyond this fraction of the effective start tolerance
// (still within the full window, so still a correct press) is judged
// imprecise rather than precise - see GameSession::OnPress's own comment.
constexpr double c_ImprecisionToleranceFraction = 0.5;

// Streak-length breakpoints for GameSession::MultiplierForStreak - streak
// >= c_MultiplierTierStreaks[i] earns a multiplier of (i + 2) (index 0 is the
// first tier above the base x1), so { 10, 20, 30 } means 0-9 -> x1,
// 10-19 -> x2, 20-29 -> x3, 30+ -> x4.
constexpr int c_MultiplierTierStreaks[] = {10, 20, 30};

// One candidate note being considered by ApplyEasyModeTransform's cross-lane
// thinning pass - startBeat/durationBeats are carried straight from the
// originating LaneNote, lane just tags which of the 4 lanes it came from so
// the note can be written back after the lanes have been merged into one
// combined time-ordered stream.
struct EasyModeNote
{
    double startBeat = 0.0;
    double durationBeats = 0.0;
    int lane = 0;
};

// Converts a millisecond target to beats at bpm - see c_EasyModePerLaneMinGapMs's
// own comment for why easy mode's thresholds are expressed this way instead
// of as fixed beat counts.
double EasyModeMsToBeats(double ms, double bpm)
{
    return ms * bpm / 60000.0;
}

// Returns indices into startBeats (already ascending, all in [0, spanBeats))
// to keep, such that no two consecutive kept notes - walking the circle,
// including the wrap from the last kept note back to the first - start
// closer together than minGapBeats. Greedy, seeded right after the single
// largest circular gap: this makes an already-adequately-spaced input pass
// through completely untouched (any starting point would keep everything),
// and needs no separate wraparound fixup - the gap from the true last-kept
// note back around to the seed can only be >= the largest original gap,
// since this pass only ever drops notes (never adds or moves one), which
// can only widen that gap further. Used by ApplyEasyModeTransform for both
// its per-lane and cross-lane thinning passes. Returned indices are sorted
// ascending (not in scan order), matching the ascending-by-startBeat order
// callers elsewhere in this codebase (ChartClip::NextOnsetAfter etc.)
// require of a lane's notes.
std::vector<int> CircularGreedyThinIndices(const std::vector<double>& startBeats, double spanBeats,
                                            double minGapBeats)
{
    int count = static_cast<int>(startBeats.size());
    if (count <= 1)
    {
        return count == 0 ? std::vector<int>{} : std::vector<int>{0};
    }

    std::vector<double> gaps(count);
    for (int i = 0; i < count; ++i)
    {
        double next = (i + 1 < count) ? startBeats[i + 1] : startBeats[0] + spanBeats;
        gaps[i] = next - startBeats[i];
    }
    int seed = static_cast<int>(std::max_element(gaps.begin(), gaps.end()) - gaps.begin());
    seed = (seed + 1) % count;

    std::vector<int> kept{seed};
    double lastKeptUnrolled = startBeats[seed];
    for (int step = 1; step < count; ++step)
    {
        int idx = (seed + step) % count;
        double unrolled = startBeats[idx] + spanBeats * ((seed + step) / count); // 0 or 1 loops wrapped
        if (unrolled - lastKeptUnrolled >= minGapBeats)
        {
            kept.push_back(idx);
            lastKeptUnrolled = unrolled;
        }
    }
    std::sort(kept.begin(), kept.end());
    return kept;
}

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

// Parses and validates a chart and loads all its clips' stems into the
// audio engine. Returns false if the chart fails validation or any of its
// stems can't be loaded, with outError describing every problem found.
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
        clipAlignmentInfo[&clip] = {clip.spanBeats, m_audioEngine.GetStemDurationSeconds(handle)};

        // A clip with no .mid file (hasMidi == false) has no pattern to
        // validate against the stem's length or tile to fill it - it's
        // only ever played back whole (break/background), never judged.
        if (clip.hasMidi)
        {
            if (easyMode)
            {
                ApplyEasyModeTransform(clip, song.bpm);
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
            if (!clip.ClipFitsOneLoop(stemDuration, song.bpm))
            {
                double secondsPerBeat = 60.0 / song.bpm;
                double clipBeats = stemDuration / secondsPerBeat;
                outError = L"clip '" + clip.name + L"': its MIDI pattern (" + std::to_wstring(clip.spanBeats) +
                           L" beats) is longer than one loop of its audio ('" + clip.wavFilePath + L"', " +
                           std::to_wstring(clipBeats) + L" beats) - trim the MIDI pattern or use a longer audio stem";
                return false;
            }
            clip.ExpandLaneNotesToFillClip(stemDuration, song.bpm);
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
    m_arrangementOriginValid = false;
    m_queuedBackground = QueuedBackground{};
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
    if (m_song.sections.empty())
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
    // Clears every clip's isPlaying and the arrangement origin - a fresh
    // start re-anchors the next clip that plays, exactly like a brand new
    // LoadChart.
    m_clipInstances.clear();
    m_arrangementOriginValid = false;
    m_queuedBackground = QueuedBackground{};
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

    m_clock.Start(m_song.bpm);
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
        entry.second.isPlaying = false;
    }
    m_arrangementOriginValid = false;
    m_queuedBackground = QueuedBackground{};
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

// See the header comment - lets a press that lands right on the count-in's
// own boundary be judged immediately instead of waiting for the next
// Update() tick to flip the phase.
void GameSession::CatchUpCountIn()
{
    if (m_phase != GamePhase::CountIn)
    {
        return;
    }
    double secondsPerBeat = 60.0 / m_song.bpm;
    double transitionSeconds = CountInSeconds();
    if (m_clock.ElapsedSeconds() >= transitionSeconds)
    {
        BeginSection(0, transitionSeconds / secondsPerBeat);
    }
}

// True exactly when OnPress(lane) would actually judge a press right now -
// mirrors every one of OnPress's own early-return guards except the pause
// check (deliberately - see the header's own comment), without any of its
// judging side effects.
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
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    if (section.kind != SectionKind::Learn)
    {
        return false; // break/reset/background: no judging, ever
    }
    const ChartClip& clip = m_song.clips[section.clipIndex];
    if (clip.laneNotes[lane].empty())
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
    if (clip.learnMode == LearnMode::Pass && m_currentInstance.IsPassing())
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

    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];

    double secondsPerBeat = 60.0 / m_song.bpm;
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

// Judges a press already confirmed within its note's start tolerance - see
// the header comment.
void GameSession::ApplyInTolerancePress(int lane, const ChartSection& section, const ChartClip& clip,
                                         double startBeat, double pressSeconds)
{
    double secondsPerBeat = 60.0 / m_song.bpm;
    double startSeconds = startBeat * secondsPerBeat;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
    double originBeat = m_arrangementOriginSeconds / secondsPerBeat;
    const LaneNote* note = FindLaneNote(clip, lane, originBeat, startBeat);
    double durationBeats = note ? note->durationBeats : 0.0;

    // How close this press landed to the note's own onset, as a fraction of
    // the tolerance window it was judged against - past
    // c_ImprecisionToleranceFraction of it, still a correct press (it's
    // within the full window), but not a precise one. See
    // SectionInstance::LaneHoldWasPrecise/GameSession::RegisterHit.
    bool wasPrecise = std::abs(pressSeconds - startSeconds) <= toleranceSeconds * c_ImprecisionToleranceFraction;

    StartClipLoop(section.clipIndex, clip.initVolume);
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

// See the header comment - lets a press whose note belongs to a section
// that hasn't begun yet still be judged, instead of losing the early half
// of that note's own tolerance window purely because the section transition
// hadn't happened yet.
bool GameSession::TryBufferEarlyPress(int lane)
{
    if (m_paused || lane < 0 || lane >= c_LaneCount)
    {
        return false;
    }

    int previewIdx = PreviewSectionIndex();
    if (previewIdx < 0 || m_song.sections[previewIdx].kind != SectionKind::Learn)
    {
        return false;
    }
    const ChartClip& clip = m_song.clips[m_song.sections[previewIdx].clipIndex];
    if (clip.laneNotes[lane].empty())
    {
        return false;
    }

    double onsetBeat = PreviewFirstOnsetBeatForLane(lane);
    if (onsetBeat < 0.0)
    {
        return false;
    }
    double secondsPerBeat = 60.0 / m_song.bpm;
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
    double secondsPerBeat = 60.0 / m_song.bpm;
    double toleranceSeconds = EffectiveStartToleranceSeconds(clip);

    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        BufferedPress buffered = m_bufferedPress[lane];
        m_bufferedPress[lane] = BufferedPress{}; // never carries over past this section beginning

        if (!buffered.active || clip.laneNotes[lane].empty())
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
                double releaseToleranceSeconds = clip.releaseToleranceMs / 1000.0;
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

    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
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

    const ChartClip& clip = m_song.clips[section.clipIndex];
    double secondsPerBeat = 60.0 / m_song.bpm;
    double endSeconds = m_currentInstance.LaneHoldExpectedEndBeat(lane) * secondsPerBeat;
    double toleranceSeconds = clip.releaseToleranceMs / 1000.0;
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
        int clipIndex = m_song.sections[m_currentInstance.SectionIndex()].clipIndex;
        auto it = clipIndex >= 0 ? m_clipInstances.find(&m_song.clips[clipIndex]) : m_clipInstances.end();
        if (it != m_clipInstances.end() && it->second.isPlaying && m_audioEngine.IsPlaying(m_stemHandles[clipIndex]))
        {
            double audioElapsed = it->second.loopStartSeconds + m_audioEngine.GetPositionSeconds(m_stemHandles[clipIndex]);
            if (std::abs(audioElapsed - m_clock.ElapsedSeconds()) > c_ClockResyncThresholdSeconds)
            {
                m_clock.Resync(audioElapsed);
            }
        }
    }

    double secondsPerBeat = 60.0 / m_song.bpm;
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
        const ChartSection& heldSection = m_song.sections[m_currentInstance.SectionIndex()];
        if (heldSection.kind == SectionKind::Learn)
        {
            const ChartClip& heldClip = m_song.clips[heldSection.clipIndex];
            double toleranceSeconds = heldClip.releaseToleranceMs / 1000.0;
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
        // The count-in is one full bar, which is bar-aligned by
        // construction - unlike the old fixed wall-clock duration, it can't
        // land in the middle of the first section's pattern, so there's
        // nothing here to protect against tolerance-wise: whatever the
        // first section anchors to at this boundary always has its full
        // start-tolerance window still ahead of it.
        CatchUpCountIn();
        return;
    }

    if (m_phase == GamePhase::Learning)
    {
        if (m_currentInstance.HasPendingAdvance() && now >= m_currentInstance.PendingAdvanceAtSeconds())
        {
            const ChartSection& finishedSection = m_song.sections[m_currentInstance.SectionIndex()];

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
                // phase-continuous on its own: the arrangement origin isn't
                // touched by a single clip stopping (only Reset/Start/Stop/
                // a Break silencing everything does that - see
                // m_arrangementOriginValid's own comment), so
                // BeginSection's queued-background realize, moments later
                // in this same synchronous chain, restarts it against the
                // exact same still-valid origin - with the default infinite
                // loop count, replacing this break's own finite one.
                StopClipLoop(finishedSection.clipIndex);
            }

            if (nextIndex < static_cast<int>(m_song.sections.size()))
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
        const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
        if (section.kind == SectionKind::Learn)
        {
            const ChartClip& clip = m_song.clips[section.clipIndex];
            if (clip.learnMode == LearnMode::Pass && m_currentInstance.IsPassing())
            {
                // Locked in, Pass mode: IsLaneJudgeable already keeps real
                // presses from reaching RegisterHit/RegisterMiss again (see
                // its own comment), so this is the section's sole scorer
                // from here to its own advance - walk every lane forward
                // from its own auto-score cursor and credit each note newly
                // crossed as a precise hit, exactly "as though the player is
                // playing perfectly for the remainder of the section."
                double originBeat = CurrentClipOriginBeat();
                double nowBeat = now / secondsPerBeat;
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    if (clip.laneNotes[lane].empty())
                    {
                        continue;
                    }
                    for (double onsetBeat : ChartClip::OnsetsInRange(originBeat, m_autoScoreCursorBeat[lane],
                                                                        nowBeat, clip.laneNotes[lane], clip.spanBeats))
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
                    if (clip.laneNotes[lane].empty())
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
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(m_song.sections.size()))
    {
        return nullptr;
    }
    int clipIndex = m_song.sections[sectionIndex].clipIndex;
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.clips.size()))
    {
        return nullptr;
    }
    return &m_song.clips[clipIndex];
}

// Returns the current section's kind, or Learn as a harmless default if there's no current section.
SectionKind GameSession::CurrentSectionKind() const
{
    int sectionIndex = m_currentInstance.SectionIndex();
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(m_song.sections.size()))
    {
        return SectionKind::Learn;
    }
    return m_song.sections[sectionIndex].kind;
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
    if (CurrentClip() == nullptr || !m_arrangementOriginValid)
    {
        return 0.0;
    }
    double secondsPerBeat = 60.0 / m_song.bpm;
    return m_arrangementOriginSeconds / secondsPerBeat;
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

// Returns the index of the first section at or after startIndex that
// persists as "current" for any real duration (i.e. not Background or
// Reset - neither ever occupies "current" for an observable moment;
// BeginSection always recurses straight through them), or -1 if none
// remain.
int GameSession::NextPersistentSectionAtOrAfter(int startIndex) const
{
    for (int i = std::max(startIndex, 0); i < static_cast<int>(m_song.sections.size()); ++i)
    {
        if (m_song.sections[i].kind != SectionKind::Background && m_song.sections[i].kind != SectionKind::Reset)
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
        if (idx < 0 || m_song.sections[idx].kind != SectionKind::Learn)
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
    const ChartSection& current = m_song.sections[sectionIndex];
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
        if (nextIdx < 0 || m_song.sections[nextIdx].kind != SectionKind::Learn)
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
        if (m_song.sections[i].kind == SectionKind::Reset)
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
    return &m_song.clips[m_song.sections[idx].clipIndex];
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
    const ChartClip& preview = m_song.clips[m_song.sections[idx].clipIndex];

    double secondsPerBeat = 60.0 / m_song.bpm;
    double transitionBeat = PreviewTransitionSeconds() / secondsPerBeat;
    double originBeat = PreviewClipOriginBeat();

    // PreviewClipOriginBeat() already resolves to transitionBeat itself
    // whenever this preview would start a fresh arrangement (see its own
    // comment), so this is correct whether preview is joining fresh or
    // continuing an already-open groove - both cases want the first onset
    // at/after the instant this section actually begins.
    return preview.NextOnsetAfter(originBeat, transitionBeat - 1e-6, lane);
}

// See its own header comment.
double GameSession::PreviewClipOriginBeat() const
{
    int idx = PreviewSectionIndex();
    if (idx < 0)
    {
        return -1.0;
    }
    double secondsPerBeat = 60.0 / m_song.bpm;
    if (m_arrangementOriginValid && !ArrangementResetsBeforeSection(idx))
    {
        return m_arrangementOriginSeconds / secondsPerBeat;
    }
    // Nothing valid to inherit - this preview clip will start a fresh
    // arrangement of its own, at its own transition beat.
    return PreviewTransitionSeconds() / secondsPerBeat;
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
                 sectionIndex >= 0 ? m_song.sections[sectionIndex].clipIndex : -1);
#endif
    m_currentInstance.RecordOnsetJudgement(startBeat, lane, result, precise);
}

// Begins the section at the given index, kicking off any background clip
// queued by the previous section first, then dispatching on this section's
// own kind.
void GameSession::BeginSection(int sectionIndex, double scheduledBeat)
{
    // Resolved before constructing m_currentInstance, since the Learn
    // clip's own learnMode decides that instance's starting passing state
    // (see SectionInstance's own constructor comment) - irrelevant/unused
    // for every other section kind, where Pass is just a harmless default.
    const ChartSection& section = m_song.sections[sectionIndex];
    LearnMode mode =
        section.kind == SectionKind::Learn ? m_song.clips[section.clipIndex].learnMode : LearnMode::Pass;
    m_currentInstance = SectionInstance(sectionIndex, mode);
    m_phase = GamePhase::Learning;

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
        StartClipLoop(bgClipIndex, m_song.clips[bgClipIndex].volume);
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

        case SectionKind::Break:
        {
            const ChartClip& clip = m_song.clips[section.clipIndex];

            // Whether to invalidate the arrangement origin below - read
            // before StopAllExcept touches anything else's isPlaying, since
            // that's exactly what StopAllExcept itself just did or didn't
            // silence.
            auto existing = m_clipInstances.find(&clip);
            bool ownClipWasPlaying = existing != m_clipInstances.end() && existing->second.isPlaying;

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
                    entry.second.isPlaying = false;
                }
            }
            if (!ownClipWasPlaying)
            {
                // StopAllExcept a clip that wasn't already playing silences
                // everything - the next clip to start (this one) becomes a
                // fresh arrangement's own origin instead of inheriting one
                // nothing is still audibly following.
                m_arrangementOriginValid = false;
            }

            // Same c_NoteFallBeats guarantee a learn section's own advance
            // gives (see the Learn case below): without it, a short break
            // can hand off to the next learn section with its first note's
            // scheduled beat only an instant away, so the note lane's
            // preview lands it already at (or past) the judge line instead
            // of spawning at the top edge with its full travel time. See
            // ChartClip::ComputeBreakAdvance for the extended-loop-count
            // math (shared with the editor's analytical block scheduler).
            double secondsPerBeat = 60.0 / m_song.bpm;
            double tFallSeconds = c_NoteFallBeats * secondsPerBeat;

            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
            double nowSeconds = m_clock.ElapsedSeconds();
            double originSeconds = EnsureArrangementOrigin(nowSeconds);

            ChartClip::BreakAdvance advance =
                ChartClip::ComputeBreakAdvance(originSeconds, nowSeconds, stemDuration, section.loopCount, tFallSeconds);

            // clip's own isPlaying may still be true here (left alone
            // above) - flip it false right before StartClipLoop so it still
            // performs its one necessary restart (an already-submitted
            // buffer's loop count can't be changed in place, and this break
            // needs its own, possibly finite, count applied) instead of
            // early-returning as "already playing, nothing to do". The
            // arrangement origin captured above is untouched either way, so
            // this restart's phase seek lands exactly where a still-playing
            // voice already was, not at sample 0.
            m_clipInstances[&clip].isPlaying = false;

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
            StartClipLoop(section.clipIndex, clip.volume, advance.loopCount);
            // Measured from nowSeconds (captured just above, the same
            // instant StartClipLoop records into the clip's own voice),
            // not a later clock read - matches how the Learn case below
            // does this for its own advance floor.
            m_currentInstance.SchedulePendingAdvance(advance.advanceSeconds);
            return;
        }

        case SectionKind::Reset:
        {
            // A silence gate with no clip of its own and no screen time -
            // stop everything and fall straight through to the next
            // section immediately, exactly like Background above (whose
            // queued-clip-realize block at the top of this function
            // already handles any [background] section(s) that happen to
            // follow, with no special-casing needed here).
            m_audioEngine.StopAll();
            for (auto& entry : m_clipInstances)
            {
                entry.second.isPlaying = false;
            }
            m_arrangementOriginValid = false;

            int nextIndex = sectionIndex + 1;
            if (nextIndex < static_cast<int>(m_song.sections.size()))
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
            const ChartClip& clip = m_song.clips[section.clipIndex];
            double secondsPerBeat = 60.0 / m_song.bpm;
            // scheduledBeat, not a live clock read, so this is exactly the
            // same instant PreviewFirstOnsetBeatForLane already predicted
            // (see its own comment) - and so this section's own origin and
            // its sectionStartSeconds (below) agree exactly, with no tiny
            // live-clock-read gap between them.
            double nowSeconds = scheduledBeat * secondsPerBeat;

            // Established here, ahead of the onset computation below, which
            // needs it right away - StartClipLoop's own establishment
            // (moments later) is then just a no-op confirming the same
            // value.
            double originSeconds = EnsureArrangementOrigin(nowSeconds);
            double originBeat = originSeconds / secondsPerBeat;

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
                    lane, clip.NextOnsetAfter(originBeat, scheduledBeat - 1e-6, lane));
            }

            // Starts immediately and schedules its own advance right away
            // too, exactly like Break above - the section advances on this
            // fixed schedule whether or not the player ever passes (see
            // RegisterHit/RegisterMiss for what passing still does: purely
            // the glow/confetti/volume-switch treatment, and - in Pass mode
            // only - turning off the "3 misses stops the loop" penalty;
            // none of it affects this timing, which is already decided).
            double tFallSeconds = c_NoteFallBeats * secondsPerBeat;
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);

            StartClipLoop(section.clipIndex, clip.initVolume);
            const ClipInstance& instance = m_clipInstances.at(&clip);
            m_currentInstance.SchedulePendingAdvance(ChartClip::ComputeLearnAdvanceSeconds(
                originSeconds, nowSeconds, instance.loopStartSeconds, stemDuration, section.loopCount,
                tFallSeconds));

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

// Records a hit: pays c_PrecisePoints/c_ImprecisePoints into m_bank and
// registers with m_streakTracker (see its own comment - this drives
// CurrentMultiplier() and no longer resets on an isolated miss), then
// (re)starts the current section's clip loop if it isn't already playing -
// only actually needed to recover a clip StopClipLoop silenced after 3
// consecutive misses, since BeginSection already started it once. Once this
// section's own hitsRequired progress reaches the clip's hits_required,
// starts the section passing: IsPassing() flips true, the clip's volume
// switches from init_volume to volume, and every lane's auto-score cursor
// seeds to right now (see Update()'s own Pass-mode auto-accrual comment) -
// purely a reward/feedback treatment for the volume switch, since the
// section's own advance timing was already decided in BeginSection and
// doesn't change either way. Once already passing, this section's own
// hitsRequired progress is left alone (frozen at its passing value) since it
// no longer drives anything - in Pass mode that's permanent; in DontFail
// mode a later miss (see RegisterMiss) can still drop back to failing and
// start it over. m_bank is paid regardless of passing state, since every
// judged note is worth something - including, in Pass mode, the ones
// Update() judges automatically after lock-in.
void GameSession::RegisterHit(int lane, bool wasPrecise)
{
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];

    int multiplierBefore = MultiplierForStreak(m_streakTracker.Streak());
    m_bank += wasPrecise ? c_PrecisePoints : c_ImprecisePoints;
    PushHudChanged(HudField::Bank, m_bank);

    bool newlyPassing = m_currentInstance.RegisterHit(clip.hitsRequired, m_streakTracker);

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
        m_audioEngine.SetVolume(m_stemHandles[section.clipIndex], static_cast<float>(clip.volume));

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
        double secondsPerBeat = 60.0 / m_song.bpm;
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
        if (clip.learnMode == LearnMode::Pass)
        {
            double lockInBeat = now / secondsPerBeat;
            for (double& cursor : m_autoScoreCursorBeat)
            {
                cursor = lockInBeat;
            }
        }
    }
    StartClipLoop(section.clipIndex, clip.initVolume);

    m_lastJudgement = JudgementResult::Hit;
    m_judgementEvents.push_back({JudgementResult::Hit, lane, m_currentInstance.IsPassing(), wasPrecise});
}

// Starts clipIndex's stem looping now (phase-aligned to the beat grid, at
// the given volume) if it isn't already playing, and records the start
// time for loop_count to measure from.
void GameSession::StartClipLoop(int clipIndex, double volume, int finiteLoopCount)
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.clips.size()))
    {
        return;
    }
    const ChartClip* clip = &m_song.clips[clipIndex];
    ClipInstance& instance = m_clipInstances[clip];
    if (instance.isPlaying)
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
    // A safety net for any caller that starts a clip without going through
    // BeginSection's Learn/Break cases (which each establish the
    // arrangement origin explicitly, ahead of their own origin-dependent
    // computations) - a no-op here whenever that's already happened.
    // Background clips (and any other direct StartClipLoop call) rely on
    // this to establish it fresh, right here.
    double originSeconds = EnsureArrangementOrigin(nowSeconds);
    // ChartClip::ComputeClipPhaseSeconds, not a raw fmod against
    // stemDuration - see its own doc comment for why: the audio needs to
    // phase-align to the judged notes' own beat grid (clip.spanBeats),
    // measured from the arrangement origin, not the audio file's own
    // measured length, or the two drift apart over many loops and a clip
    // reached late in a song can start audibly partway through (even near
    // the end of) its own pattern.
    double phaseSeconds = clip->ComputeClipPhaseSeconds(originSeconds, nowSeconds, stemDuration, m_song.bpm);
    m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(volume), finiteLoopCount);
    instance.isPlaying = true;
    instance.loopStartSeconds = nowSeconds;
}

// See its own header comment.
double GameSession::EnsureArrangementOrigin(double nowSeconds)
{
    if (!m_arrangementOriginValid)
    {
        m_arrangementOriginValid = true;
        m_arrangementOriginSeconds = nowSeconds;
    }
    return m_arrangementOriginSeconds;
}

// Stops clipIndex's stem if it's playing.
void GameSession::StopClipLoop(int clipIndex)
{
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_song.clips.size()))
    {
        return;
    }
    auto it = m_clipInstances.find(&m_song.clips[clipIndex]);
    if (it == m_clipInstances.end() || !it->second.isPlaying)
    {
        return;
    }
    m_audioEngine.Stop(m_stemHandles[clipIndex]);
    it->second.isPlaying = false;
}

// Records a miss: registers with m_streakTracker (see its own comment - an
// isolated miss no longer wipes m_bank or the multiplier by itself, only
// counts toward the shared 3-in-a-row trip) and stops the current section's
// clip loop once that trip fires - unchanged in both modes. In Pass mode, a
// no-op once already passing - once locked in, IsLaneJudgeable already keeps
// real presses from reaching here again, so this path is unreachable for a
// real press; still no-ops defensively. In DontFail mode, a miss while
// passing additionally drops the section back to failing and reverts the
// clip's volume to init_volume (the mirror of RegisterHit's own switch to
// volume on newly passing). In easy mode, the first miss each section is
// instead fully forgiven regardless of mode (see SectionInstance's own
// easy-grace comment) - streakTracker isn't touched, and neither of the
// above fires, as if it never happened.
void GameSession::RegisterMiss(int lane)
{
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];

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
        m_audioEngine.SetVolume(m_stemHandles[section.clipIndex], static_cast<float>(clip.initVolume));
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

// Moves this lane's next-expected-note pointer forward to the next note
// after it, on the current arrangement origin (see
// m_arrangementOriginSeconds) - always already valid by the time this runs,
// since a lane can only be judgeable mid-Learn-section, which only happens
// after BeginSection has already established it.
void GameSession::AdvanceExpectedNote(int lane)
{
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];
    double secondsPerBeat = 60.0 / m_song.bpm;
    double originBeat = m_arrangementOriginSeconds / secondsPerBeat;
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

// Returns the start-tolerance window (seconds) to judge a press with -
// see the header comment for the easy-mode widening this applies.
double GameSession::EffectiveStartToleranceSeconds(const ChartClip& clip) const
{
    double toleranceMs = clip.startToleranceMs;
    if (m_easyMode)
    {
        toleranceMs *= c_EasyModeToleranceMultiplier;
        auto it = m_clipInstances.find(&clip);
        bool clipPlaying = it != m_clipInstances.end() && it->second.isPlaying;
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
    double secondsPerBeat = 60.0 / m_song.bpm;
    return m_song.beatsPerBar * secondsPerBeat;
}

// Simplifies clip's MIDI-derived pattern for easy mode - see the header's
// doc comment for the full contract. Runs on one repetition's worth of
// notes (before ExpandLaneNotesToFillClip tiles it), so spanBeats here
// still means "one repetition's length."
void GameSession::ApplyEasyModeTransform(ChartClip& clip, double bpm)
{
    if (!clip.hasMidi || clip.spanBeats <= 0.0)
    {
        return;
    }

    double span = clip.spanBeats;
    double perLaneMinGapBeats = EasyModeMsToBeats(c_EasyModePerLaneMinGapMs, bpm);
    double globalMinGapBeats = EasyModeMsToBeats(c_EasyModeGlobalMinGapMs, bpm);
    double durationFloorBeats = EasyModeMsToBeats(c_EasyModeNoteDurationFloorMs, bpm);

    // Stage 1: per-lane density thinning. A surviving note's startBeat is
    // never moved - only which notes survive changes - so a lane whose
    // original gaps are already all >= perLaneMinGapBeats passes through
    // completely untouched (see CircularGreedyThinIndices's own comment).
    std::vector<EasyModeNote> perLaneSurvivors[c_LaneCount];
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        const std::vector<LaneNote>& notes = clip.laneNotes[lane];
        if (notes.empty())
        {
            continue;
        }
        std::vector<double> starts;
        starts.reserve(notes.size());
        for (const LaneNote& note : notes)
        {
            starts.push_back(note.startBeat);
        }
        for (int index : CircularGreedyThinIndices(starts, span, perLaneMinGapBeats))
        {
            perLaneSurvivors[lane].push_back(EasyModeNote{notes[index].startBeat, notes[index].durationBeats, lane});
        }
    }

    // Stage 2: cross-lane thinning. Merge every lane's stage-1 survivors
    // into one time-ordered stream - ties broken by lane index so a
    // would-be chord deterministically favors the lowest lane, matching
    // stage 3's own tie-break below - and thin again with a tighter gap.
    // This is what catches a pattern that rapidly alternates lanes (e.g. a
    // cycling arpeggio) where any single lane looks sparse on its own but
    // the combined stream doesn't.
    std::vector<EasyModeNote> merged;
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        for (const EasyModeNote& note : perLaneSurvivors[lane])
        {
            merged.push_back(note);
        }
    }
    std::sort(merged.begin(), merged.end(), [](const EasyModeNote& a, const EasyModeNote& b) {
        if (a.startBeat != b.startBeat)
        {
            return a.startBeat < b.startBeat;
        }
        return a.lane < b.lane;
    });

    std::vector<double> mergedStarts;
    mergedStarts.reserve(merged.size());
    for (const EasyModeNote& note : merged)
    {
        mergedStarts.push_back(note.startBeat);
    }
    std::vector<EasyModeNote> globallyThinned;
    globallyThinned.reserve(merged.size());
    for (int index : CircularGreedyThinIndices(mergedStarts, span, globalMinGapBeats))
    {
        globallyThinned.push_back(merged[index]);
    }

    // Stage 3: chord collapse - a defensive backstop, independent of
    // however globalMinGapBeats gets tuned later. Any run of notes still
    // within c_EasyModeChordEpsilonBeats of the same beat (regardless of
    // lane) survives only as its lowest-indexed lane's note - lanes are
    // pitch-ordered ascending, so this keeps a chord's root/bass note.
    // globallyThinned is already sorted (startBeat asc, lane asc) by the
    // merge above and CircularGreedyThinIndices preserves that relative
    // order, so a run of near-simultaneous notes is always contiguous here
    // with the lowest lane first.
    std::vector<EasyModeNote> collapsed;
    collapsed.reserve(globallyThinned.size());
    for (const EasyModeNote& note : globallyThinned)
    {
        if (!collapsed.empty() && note.startBeat - collapsed.back().startBeat < c_EasyModeChordEpsilonBeats)
        {
            continue; // same near-simultaneous run as the previous (lower-lane) survivor
        }
        collapsed.push_back(note);
    }

    // Stage 4: duration. Each surviving note keeps its own authored length
    // instead of a fixed blip, clamped to a floor (never imperceptibly
    // short) and a ceiling of "don't run into the next surviving note in
    // this lane" - computed circularly (a lane's last note is bounded by
    // its own first note, one span later), since ExpandLaneNotesToFillClip
    // tiles whole repetitions independently and won't clip an overrun
    // against the next repetition. perLaneMinGapMs is always comfortably
    // larger than c_EasyModeNoteDurationFloorMs, so this ceiling can never
    // actually fall below the floor in practice - the std::max below is
    // just defensive insurance against changing those constants later.
    std::vector<LaneNote> perLaneFinal[c_LaneCount];
    for (const EasyModeNote& note : collapsed)
    {
        perLaneFinal[note.lane].push_back(LaneNote{note.startBeat, note.durationBeats});
    }
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        std::vector<LaneNote>& notes = perLaneFinal[lane];
        for (size_t i = 0; i < notes.size(); ++i)
        {
            double nextStart = (i + 1 < notes.size()) ? notes[i + 1].startBeat : notes[0].startBeat + span;
            double ceiling = nextStart - notes[i].startBeat - c_EasyModeSafetyEpsilonBeats;
            notes[i].durationBeats =
                std::clamp(notes[i].durationBeats, durationFloorBeats, std::max(durationFloorBeats, ceiling));
        }
        clip.laneNotes[lane] = std::move(notes);
    }
}

// Returns the lane note whose phase-within-span matches absoluteStartBeat's phase, or nullptr if none does.
const LaneNote* GameSession::FindLaneNote(const ChartClip& clip, int lane, double originBeat,
                                           double absoluteStartBeat) const
{
    double span = clip.spanBeats;
    double phase = std::fmod(absoluteStartBeat - originBeat, span);
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
