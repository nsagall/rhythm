#include "GameSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "ChartTiming.h"

namespace
{

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
        // only ever played back whole (break/background), never judged.
        if (clip.hasMidi)
        {
            if (easyMode)
            {
                ApplyEasyModeTransform(clip);
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
            // block scheduler (ChartTiming::ClipFitsOneLoop), so both
            // reject exactly the same charts.
            if (!ChartTiming::ClipFitsOneLoop(clip, stemDuration, song.bpm))
            {
                double secondsPerBeat = 60.0 / song.bpm;
                double clipBeats = stemDuration / secondsPerBeat;
                outError = L"clip '" + clip.name + L"': its MIDI pattern (" + std::to_wstring(clip.spanBeats) +
                           L" beats) is longer than one loop of its audio ('" + clip.wavFilePath + L"', " +
                           std::to_wstring(clipBeats) + L" beats) - trim the MIDI pattern or use a longer audio stem";
                return false;
            }
            ChartTiming::ExpandLaneNotesToFillClip(clip, stemDuration, song.bpm);
        }
    }

    m_song = std::move(song);
    m_stemHandles = std::move(stemHandles);
    m_phase = GamePhase::Idle;
    m_easyMode = easyMode;
    m_currentInstance = SectionInstance();
    m_clipInstances.clear();
    m_queuedBackground = QueuedBackground{};
    m_lastJudgement = JudgementResult::None;
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
    // Clears both isPlaying and every clip's established origin - a fresh
    // start re-anchors each clip's phase reference the next time it plays,
    // exactly like a brand new LoadChart (see EnsureClipInstance).
    m_clipInstances.clear();
    m_queuedBackground = QueuedBackground{};
    m_lastJudgement = JudgementResult::None;

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
    m_queuedBackground = QueuedBackground{};
    m_lastJudgement = JudgementResult::None;
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
// mirrors every one of OnPress's own early-return guards, just without any
// of its judging side effects.
bool GameSession::IsLaneJudgeable(int lane) const
{
    if (m_phase != GamePhase::Learning)
    {
        return false;
    }
    if (lane < 0 || lane >= kLaneCount || m_currentInstance.SectionIndex() < 0)
    {
        return false;
    }
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    if (section.kind != SectionKind::Learn)
    {
        return false; // break/reset/background: no judging, ever
    }
    const ChartClip& clip = m_song.clips[section.clipIndex];
    return !clip.laneNotes[lane].empty();
}

// Registers a key-down for lane; judges it against that lane's next expected note if the current section is learning.
void GameSession::OnPress(int lane)
{
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
        double originBeat = m_clipInstances.at(&clip).startSeconds / secondsPerBeat;
        const LaneNote* note = FindLaneNote(clip, lane, originBeat, startBeat);
        double durationBeats = note ? note->durationBeats : 0.0;

        StartClipLoop(section.clipIndex, clip.initVolume);
        m_currentInstance.StartLaneHold(lane, startBeat, startBeat + durationBeats);
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
    if (lane < 0 || lane >= kLaneCount || !m_currentInstance.IsLaneHeld(lane) || m_currentInstance.SectionIndex() < 0)
    {
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
        RegisterHit();
        m_lastJudgement = JudgementResult::Hit;
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Hit);
    }
    else
    {
        RegisterMiss();
        m_lastJudgement = JudgementResult::Miss;
        RecordOnsetJudgement(startBeat, lane, JudgementResult::Miss);
    }

    m_currentInstance.ClearLaneHold(lane);
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
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (!m_currentInstance.IsLaneHeld(lane))
                {
                    continue;
                }
                double endSeconds = m_currentInstance.LaneHoldExpectedEndBeat(lane) * secondsPerBeat;
                if (now > endSeconds + toleranceSeconds)
                {
                    double startBeat = m_currentInstance.LaneHoldStartBeat(lane);
                    RegisterMiss();
                    m_lastJudgement = JudgementResult::Miss;
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
        // nothing here to protect against tolerance-wise: whatever
        // FreshOnsetForAllLanes anchors to at this boundary always has its
        // full start-tolerance window still ahead of it.
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
            // ChartTiming::ComputeLearnAdvanceSeconds/ComputeBreakAdvance's
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
                StopClipLoop(finishedSection.clipIndex);
            }

            int nextIndex = m_currentInstance.SectionIndex() + 1;
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
            double toleranceSeconds = EffectiveStartToleranceSeconds(clip);
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                if (clip.laneNotes[lane].empty())
                {
                    continue;
                }
                double expectedBeat = m_currentInstance.NextExpectedBeatForLane(lane);
                double onsetSeconds = expectedBeat * secondsPerBeat;
                if (now > onsetSeconds + toleranceSeconds)
                {
                    RegisterMiss();
                    m_lastJudgement = JudgementResult::Miss;
                    RecordOnsetJudgement(expectedBeat, lane, JudgementResult::Miss);
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

// Returns the beat of the next note this lane is awaiting a press for.
double GameSession::NextExpectedBeatForLane(int lane) const
{
    return m_currentInstance.NextExpectedBeatForLane(lane);
}

// See its own header comment.
double GameSession::CurrentClipOriginBeat() const
{
    const ChartClip* clip = CurrentClip();
    if (clip == nullptr)
    {
        return 0.0;
    }
    auto it = m_clipInstances.find(clip);
    if (it == m_clipInstances.end())
    {
        return 0.0;
    }
    double secondsPerBeat = 60.0 / m_song.bpm;
    return it->second.startSeconds / secondsPerBeat;
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
    if (idx < 0 || lane < 0 || lane >= kLaneCount)
    {
        return -1.0;
    }
    int clipIndex = m_song.sections[idx].clipIndex;
    const ChartClip& preview = m_song.clips[clipIndex];

    double transitionSeconds = PreviewTransitionSeconds();
    double secondsPerBeat = 60.0 / m_song.bpm;
    double transitionBeat = transitionSeconds / secondsPerBeat;

    // Must mirror BeginSection's own Learn-case anchoring choice exactly
    // (see ClipInstance::startSeconds) - a clip previewed here before it
    // has ever actually been started (its first appearance, whether that's
    // the count-in or any later point in the song) needs
    // FreshOnsetForAllLanes, same as BeginSection will use once this
    // section actually goes live - otherwise the notes shown here would
    // silently disagree with what ends up judged. transitionBeat is
    // exactly the beat BeginSection will use as scheduledBeat (and
    // therefore as the fresh origin) once this section actually begins -
    // see BeginSection's own doc comment.
    auto it = m_clipInstances.find(&preview);
    if (it == m_clipInstances.end())
    {
        double allLanes[kLaneCount];
        ChartTiming::FreshOnsetForAllLanes(transitionBeat, preview, allLanes);
        return allLanes[lane];
    }
    double originBeat = it->second.startSeconds / secondsPerBeat;
    return ChartTiming::NextOnsetAfter(originBeat, transitionBeat - 1e-6, preview, lane);
}

// See its own header comment.
double GameSession::PreviewClipOriginBeat() const
{
    int idx = PreviewSectionIndex();
    if (idx < 0)
    {
        return -1.0;
    }
    int clipIndex = m_song.sections[idx].clipIndex;
    const ChartClip& preview = m_song.clips[clipIndex];

    double secondsPerBeat = 60.0 / m_song.bpm;
    auto it = m_clipInstances.find(&preview);
    if (it == m_clipInstances.end())
    {
        // Mirrors PreviewFirstOnsetBeatForLane's own not-yet-established
        // branch exactly - transitionBeat is what BeginSection will
        // actually use as the fresh origin once this section goes live.
        return PreviewTransitionSeconds() / secondsPerBeat;
    }
    return it->second.startSeconds / secondsPerBeat;
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
    return m_currentInstance.OnsetJudgement(startBeat, lane);
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
void GameSession::RecordOnsetJudgement(double startBeat, int lane, JudgementResult result)
{
#ifdef RHYTHM_DEBUG_JUDGEMENTS
    int sectionIndex = m_currentInstance.SectionIndex();
    std::fprintf(stderr, "[RecordOnsetJudgement] t=%.4f beat=%.4f lane=%d result=%d section=%d clip=%d\n",
                 m_clock.ElapsedSeconds(), startBeat, lane, static_cast<int>(result), sectionIndex,
                 sectionIndex >= 0 ? m_song.sections[sectionIndex].clipIndex : -1);
#endif
    m_currentInstance.RecordOnsetJudgement(startBeat, lane, result);
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
            m_audioEngine.StopAll();
            for (auto& entry : m_clipInstances)
            {
                entry.second.isPlaying = false;
            }

            // Same kNoteFallBeats guarantee a learn section's own advance
            // gives (see the Learn case below): without it, a short break
            // can hand off to the next learn section with its first note's
            // scheduled beat only an instant away, so the note lane's
            // preview lands it already at (or past) the judge line instead
            // of spawning at the top edge with its full travel time. See
            // ChartTiming::ComputeBreakAdvance for the extended-loop-count
            // math (shared with the editor's analytical block scheduler).
            double secondsPerBeat = 60.0 / m_song.bpm;
            double tFallSeconds = kNoteFallBeats * secondsPerBeat;

            const ChartClip& clip = m_song.clips[section.clipIndex];
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);
            double nowSeconds = m_clock.ElapsedSeconds();

            // Established here, ahead of ComputeBreakAdvance below, which
            // needs it right away - StartClipLoop's own establishment
            // (moments later) is then just a no-op confirming the same
            // value. See EnsureClipInstance's own comment.
            EnsureClipInstance(&clip, nowSeconds);
            double originSeconds = m_clipInstances.at(&clip).startSeconds;

            ChartTiming::BreakAdvance advance =
                ChartTiming::ComputeBreakAdvance(originSeconds, nowSeconds, stemDuration, section.loopCount, tFallSeconds);

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
            // for a fresh origin (see its own comment) - and so this
            // section's own origin and its sectionStartSeconds (below)
            // agree exactly, with no tiny live-clock-read gap between them.
            double nowSeconds = scheduledBeat * secondsPerBeat;

            // Established here, ahead of the anchor computation below,
            // which needs it right away - StartClipLoop's own
            // establishment (moments later) is then just a no-op
            // confirming the same value. See EnsureClipInstance's
            // own comment.
            bool freshOrigin = EnsureClipInstance(&clip, nowSeconds);
            double originBeat = m_clipInstances.at(&clip).startSeconds / secondsPerBeat;

            // Anchoring per clip, not per song: independent per-lane
            // NextOnsetAfter is only safe once THIS clip already has an
            // established origin (it's played before, so continuing its
            // groove from wherever its own beat grid says it'd be is the
            // right, seamless behavior) - a clip's first-ever appearance
            // instead gets FreshOnsetForAllLanes, which starts it playing
            // (and judging) from its own true beginning right now, at
            // scheduledBeat itself - no waiting for a pattern-cycle
            // boundary, and no requirement that scheduledBeat line up with
            // any other clip's own beat grid (a background clip already
            // playing just keeps looping on its own, entirely unaffected -
            // see ClipInstance::startSeconds' own comment).
            if (freshOrigin)
            {
                double allLanes[kLaneCount];
                ChartTiming::FreshOnsetForAllLanes(originBeat, clip, allLanes);
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    m_currentInstance.SetNextExpectedBeat(lane, allLanes[lane]);
                }
            }
            else
            {
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    m_currentInstance.SetNextExpectedBeat(
                        lane, ChartTiming::NextOnsetAfter(originBeat, scheduledBeat - 1e-6, clip, lane));
                }
            }

            // Starts immediately and schedules its own advance right away
            // too, exactly like Break above - the section advances on this
            // fixed schedule whether or not the player ever passes (see
            // RegisterHit/RegisterMiss for what passing still does: purely
            // the glow/confetti/volume-switch treatment, and - in Pass mode
            // only - turning off the "3 misses stops the loop" penalty;
            // none of it affects this timing, which is already decided).
            double tFallSeconds = kNoteFallBeats * secondsPerBeat;
            double stemDuration = m_audioEngine.GetStemDurationSeconds(m_stemHandles[section.clipIndex]);

            StartClipLoop(section.clipIndex, clip.initVolume);
            const ClipInstance& instance = m_clipInstances.at(&clip);
            m_currentInstance.SchedulePendingAdvance(ChartTiming::ComputeLearnAdvanceSeconds(
                instance.startSeconds, nowSeconds, instance.loopStartSeconds, stemDuration, section.loopCount,
                tFallSeconds));
            return;
        }
    }
}

// Records a hit: advances the shared streak, resets the shared miss
// counter, and (re)starts the current section's clip loop if it isn't
// already playing - only actually needed to recover a clip StopClipLoop
// silenced after 3 consecutive misses, since BeginSection already started
// it once. Once the streak reaches the clip's hits_required, starts the
// section passing: IsPassing() flips true and the clip's volume switches
// from init_volume to volume - purely a reward/feedback treatment, since
// the section's own advance timing was already decided in BeginSection and
// doesn't change either way. Once already passing, the streak/miss
// counters are left alone (frozen at their passing value) since they no
// longer drive anything - in Pass mode that's permanent; in DontFail mode
// a later miss (see RegisterMiss) can still drop back to failing and start
// the streak over.
void GameSession::RegisterHit()
{
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];

    if (m_currentInstance.RegisterHit(clip.hitsRequired))
    {
        m_audioEngine.SetVolume(m_stemHandles[section.clipIndex], static_cast<float>(clip.volume));
    }
    StartClipLoop(section.clipIndex, clip.initVolume);
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
    auto existing = m_clipInstances.find(clip);
    if (existing != m_clipInstances.end() && existing->second.isPlaying)
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
    // BeginSection's Learn/Break cases (which each establish the origin
    // explicitly, ahead of their own origin-dependent computations) - a
    // no-op here whenever that's already happened. Background clips (and
    // any other direct StartClipLoop call) rely on this to establish their
    // own origin fresh, right here.
    EnsureClipInstance(clip, nowSeconds);
    ClipInstance& instance = m_clipInstances.at(clip);
    double originSeconds = instance.startSeconds;
    // ChartTiming::ComputeClipPhaseSeconds, not a raw fmod against
    // stemDuration - see its own doc comment for why: the audio needs to
    // phase-align to the judged notes' own beat grid (clip.spanBeats),
    // measured from this clip's own persistent origin, not the audio
    // file's own measured length, or the two drift apart over many loops
    // and a clip reached late in a song can start audibly partway through
    // (even near the end of) its own pattern. For a first-ever start,
    // nowSeconds == originSeconds exactly, so this is always 0 - the
    // clip's own true beginning, immediately, with zero wait.
    double phaseSeconds = ChartTiming::ComputeClipPhaseSeconds(originSeconds, nowSeconds, *clip, stemDuration, m_song.bpm);
    m_audioEngine.StartLooping(handle, phaseSeconds, static_cast<float>(volume), finiteLoopCount);
    instance.isPlaying = true;
    instance.loopStartSeconds = nowSeconds;
}

// See its own header comment.
bool GameSession::EnsureClipInstance(const ChartClip* clip, double nowSeconds)
{
    if (m_clipInstances.find(clip) != m_clipInstances.end())
    {
        return false;
    }
    ClipInstance instance;
    instance.chartClip = clip;
    instance.startSeconds = nowSeconds;
    m_clipInstances.emplace(clip, instance);
    return true;
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

// Records a miss: resets the shared streak, and stops the current section's clip loop after 3 in a row - unchanged in both modes. In Pass mode, a no-op once already passing - further misses shouldn't stop the clip or unfreeze the streak display once it's proven itself, though the section's own advance timing was never affected by any of this either way. In DontFail mode, a miss while passing additionally drops the section back to failing and reverts the clip's volume to init_volume (the mirror of RegisterHit's own switch to volume on newly passing). In easy mode, the first miss each section is instead fully forgiven regardless of mode (see SectionInstance's own easy-grace comment) - streak and consecutive-miss count both left untouched, and neither of the above fires, as if it never happened.
void GameSession::RegisterMiss()
{
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];

    SectionInstance::MissResult result = m_currentInstance.RegisterMiss(m_easyMode);
    if (result.shouldStopClip)
    {
        StopClipLoop(section.clipIndex);
    }
    if (result.justEnteredFailState)
    {
        m_audioEngine.SetVolume(m_stemHandles[section.clipIndex], static_cast<float>(clip.initVolume));
    }
}

// Moves this lane's next-expected-note pointer forward to the next note
// after it, on this clip's own established origin (see
// ClipInstance::startSeconds) - always already set by the time this
// runs, since a lane can only be judgeable mid-Learn-section, which only
// happens after BeginSection has already anchored it.
void GameSession::AdvanceExpectedNote(int lane)
{
    const ChartSection& section = m_song.sections[m_currentInstance.SectionIndex()];
    const ChartClip& clip = m_song.clips[section.clipIndex];
    double secondsPerBeat = 60.0 / m_song.bpm;
    double originBeat = m_clipInstances.at(&clip).startSeconds / secondsPerBeat;
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
        toleranceMs *= kEasyModeToleranceMultiplier;
        auto it = m_clipInstances.find(&clip);
        bool clipPlaying = it != m_clipInstances.end() && it->second.isPlaying;
        if (!clipPlaying)
        {
            toleranceMs *= kEasyModeStoppedToleranceMultiplier;
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
