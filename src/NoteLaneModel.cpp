#include "NoteLaneModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// When true, a section hands its dots off early to the next clip's preview as soon as that clip's
// notes are due, so they scroll down from the top like any other instead of the whole
// c_BeatsAhead-deep window materializing at once. False keeps a section's dots/judging live right
// up to the scheduled advance, with no preview lead time. See nextClipShowing below.
constexpr bool c_PreviewNextClipBeforeHandoff = true;

// Returns value formatted with thousands separators (12345 -> "12,345"). Duplicated from
// MainWindow's FormatScoreWithCommas rather than shared - a few lines of pure formatting.
std::wstring FormatScoreWithCommas(int value)
{
    std::wstring digits = std::to_wstring(value);
    std::wstring result;
    int sinceComma = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (sinceComma == 3)
        {
            result.push_back(L',');
            sinceComma = 0;
        }
        result.push_back(*it);
        ++sinceComma;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

} // namespace

std::vector<SceneNote> NoteLaneModel::NotesInRange(int lane, double originBeat, double fromBeat, double toBeat,
                                                    const std::vector<LaneNote>& notes, double spanBeats)
{
    std::vector<SceneNote> result;
    if (notes.empty() || spanBeats <= 0.0)
    {
        return result;
    }

    double localFromBeat = fromBeat - originBeat;
    double localToBeat = toBeat - originBeat;
    long long firstBar = static_cast<long long>(std::floor(localFromBeat / spanBeats)) - 1;
    // Never tile a bar before the clip's origin - bar 0 is its first repetition. Without this,
    // a fromBeat just before originBeat (routine, since the judged pass looks c_BeatsBehind "now")
    // synthesizes a phantom copy of a note near the pattern's end, one spanBeats too early.
    if (firstBar < 0)
    {
        firstBar = 0;
    }
    long long lastBar = static_cast<long long>(std::floor(localToBeat / spanBeats)) + 1;

    for (long long bar = firstBar; bar <= lastBar; ++bar)
    {
        for (const LaneNote& note : notes)
        {
            double absoluteStart = originBeat + bar * spanBeats + note.startBeat;
            double absoluteEnd = absoluteStart + note.durationBeats;
            // toBeat is exclusive: for the judged pass it's the scheduled advance beat, a whole-loop
            // boundary, so a note at local offset 0 tiles a candidate exactly on toBeat that
            // represents a next repetition that never plays. An inclusive check would draw it.
            if (absoluteEnd >= fromBeat && absoluteStart < toBeat)
            {
                SceneNote sceneNote;
                sceneNote.lane = lane;
                sceneNote.startBeat = absoluteStart;
                sceneNote.durationBeats = note.durationBeats;
                result.push_back(sceneNote);
            }
        }
    }
    return result;
}

void NoteLaneModel::CollectNotes(const GameSession& session, const ClipPlaythrough* instance, double originBeat,
                                  bool judged, double fromBeat, double upperBoundBeat, NoteLaneScene& scene) const
{
    if (!instance)
    {
        return;
    }
    const ChartClip* drawClip = instance->chartClip;

    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        double dotsFromBeat = fromBeat;
        bool revealFromFirstOnset = dotsFromBeat < 0.0;
        if (revealFromFirstOnset)
        {
            dotsFromBeat = session.PreviewFirstOnsetBeatForLane(lane);
            if (dotsFromBeat < 0.0)
            {
                continue;
            }
        }

        std::vector<SceneNote> visibleNotes = NotesInRange(lane, originBeat, dotsFromBeat, upperBoundBeat,
                                                             drawClip->LaneNotes(lane), drawClip->SpanBeats());
        if (revealFromFirstOnset)
        {
            // NotesInRange tiles one bar earlier than dotsFromBeat to catch a still-animating tail.
            // For a clip established long ago (e.g. looping as [background] before a [learn] reuses
            // it) that earlier bar is real - the previous repetition's last note - and would show
            // as if it were coming up. The judged pass excludes it via NextExpectedBeatForLane;
            // mirror that here.
            visibleNotes.erase(std::remove_if(visibleNotes.begin(), visibleNotes.end(),
                                               [dotsFromBeat](const SceneNote& note)
                                               { return note.startBeat < dotsFromBeat - 1e-6; }),
                                visibleNotes.end());
        }
        // Build with -DRHYTHM_DEBUG_RENDER to trace what NotesInRange tiles per lane/pass every
        // frame - the way to catch a phantom/duplicate-note bug, since a headless diagnostic
        // renders nothing. Redirect stderr to a file and let the game free-run.
#ifdef RHYTHM_DEBUG_RENDER
        if (!visibleNotes.empty())
        {
            std::fwprintf(stderr, L"[NoteLaneModel] judged=%d clip=%ls lane=%d origin=%.4f from=%.4f to=%.4f:",
                           judged ? 1 : 0, drawClip->Name().c_str(), lane, originBeat, dotsFromBeat, upperBoundBeat);
            for (const SceneNote& n : visibleNotes)
            {
                std::fwprintf(stderr, L" [%.4f+%.4f]", n.startBeat, n.durationBeats);
            }
            std::fwprintf(stderr, L"\n");
        }
#endif
        for (SceneNote& sceneNote : visibleNotes)
        {
            sceneNote.clip = instance;

            // Upcoming notes stay Normal. A correct press makes a note Held through the hold; a
            // release or press-window timeout resolves it to Hit/Miss, which then holds for the
            // rest of the note's time on screen.
            if (judged && session.IsLaneHeld(lane) &&
                std::abs(session.LaneHoldStartBeat(lane) - sceneNote.startBeat) < 1e-6)
            {
                sceneNote.state = NoteVisualState::Held;
            }
            else if (judged)
            {
                JudgementResult result = session.OnsetJudgement(sceneNote.startBeat, lane);
                if (result == JudgementResult::Hit)
                {
                    sceneNote.state = NoteVisualState::Hit;
                    sceneNote.precise = session.OnsetPrecise(sceneNote.startBeat, lane);
                }
                else if (result == JudgementResult::Miss)
                {
                    sceneNote.state = NoteVisualState::Miss;
                }
                else if (sceneNote.startBeat < session.NextExpectedBeatForLane(lane) - 1e-6)
                {
                    // Never held or judged - only possible for a clip's first-ever appearance,
                    // which can start partway into a bar. Not included in the scene.
                    continue;
                }
            }

            scene.notes.push_back(sceneNote);
        }
    }
}

std::unique_ptr<ClipPlaythrough> NoteLaneModel::MakeClipInstance(const GameSession& session, const ChartClip* chartClip,
                                                                double startBeat)
{
    if (!chartClip)
    {
        return nullptr;
    }
    auto instance = std::make_unique<ClipPlaythrough>();
    instance->chartClip = chartClip;
    instance->startBeat = startBeat;
    const ChartClip* base = session.Song().Clips().data();
    instance->color = ClipColor::ForIndex(static_cast<int>(chartClip - base));
    return instance;
}

namespace
{
// Returns the beat the loop repetition covering nowBeat started at: originBeat for the first
// repetition, originBeat + spanBeats for the second, and so on.
double CurrentLoopStartBeat(double originBeat, double nowBeat, double spanBeats)
{
    if (spanBeats <= 0.0)
    {
        return originBeat;
    }
    return originBeat + std::floor((nowBeat - originBeat) / spanBeats) * spanBeats;
}
} // namespace

void NoteLaneModel::UpdateClipInstances(const GameSession& session, double nowBeat)
{
    const ChartClip* currentChartClip = session.CurrentClip();
    double currentStartBeat =
        currentChartClip ? CurrentLoopStartBeat(session.CurrentClipOriginBeat(), nowBeat, currentChartClip->SpanBeats())
                          : 0.0;

    // Identity is the (chartClip, startBeat) pair: a new loop repetition of the same clip is still
    // a new playthrough and must be detected here like a section transition.
    const ChartClip* trackedCurrentClip = m_currentClip ? m_currentClip->chartClip : nullptr;
    bool currentChanged = trackedCurrentClip != currentChartClip ||
                           (currentChartClip && std::abs(m_currentClip->startBeat - currentStartBeat) > 1e-6);
    if (currentChanged)
    {
        m_previousClip = std::move(m_currentClip);

        bool nextMatches = m_nextClip && m_nextClip->chartClip == currentChartClip &&
                            std::abs(m_nextClip->startBeat - currentStartBeat) <= 1e-6;
        if (nextMatches)
        {
            // What we were predicting turned out right - reuse that instance instead of a new one.
            m_currentClip = std::move(m_nextClip);
        }
        else
        {
            m_currentClip = MakeClipInstance(session, currentChartClip, currentStartBeat);
        }
    }
    if (m_currentClip)
    {
        // Carries the current run's passing state forward. Never mirrored for a DontFail clip,
        // which conveys progress through the hits meter instead of a glow that would flicker on
        // every miss/recovery.
        m_currentClip->passing = session.IsPassing() && m_currentClip->chartClip->Mode() != LearnMode::DontFail;
    }

    const ChartClip* previewChartClip = session.PreviewClip();
    double previewStartBeat = 0.0;
    if (previewChartClip)
    {
        previewStartBeat = session.PreviewClipOriginBeat();
    }
    else if (m_currentClip && session.Phase() == GamePhase::Learning &&
             session.CurrentSectionKind() == SectionKind::Learn && !session.IsPassing())
    {
        // session.PreviewClip() has nothing to offer for an unlocked Learn section, but a predicted
        // repeat of the current clip's own pattern is legitimate to preview. Its start beat is one
        // spanBeats past the current instance's.
        previewChartClip = m_currentClip->chartClip;
        previewStartBeat = m_currentClip->startBeat + previewChartClip->SpanBeats();
    }

    const ChartClip* trackedNextClip = m_nextClip ? m_nextClip->chartClip : nullptr;
    bool nextChanged = trackedNextClip != previewChartClip ||
                        (previewChartClip && std::abs(m_nextClip->startBeat - previewStartBeat) > 1e-6);
    if (nextChanged)
    {
        m_nextClip = MakeClipInstance(session, previewChartClip, previewStartBeat);
    }
}

void NoteLaneModel::ResetIfSongChanged(const GameSession& session)
{
    const std::vector<ChartClip>& clips = session.Song().Clips();
    const ChartClip* clipsBase = clips.empty() ? nullptr : clips.data();
    if (clipsBase == m_lastSongClipsBase)
    {
        return;
    }
    m_lastSongClipsBase = clipsBase;
    m_previousClip.reset();
    m_currentClip.reset();
    m_nextClip.reset();
    // A leftover true from the old song would suppress the new song's first legitimate
    // justLockedIn/justFailed/justHandedOff edge.
    m_prevPassing = false;
    m_prevNotesHandoff = false;
}

NoteLaneScene NoteLaneModel::BuildScene(const GameSession& session)
{
    NoteLaneScene scene;
    ResetIfSongChanged(session);

    scene.clockRunning = session.Phase() != GamePhase::Idle;
    scene.nowBeat = scene.clockRunning ? session.Clock().BeatPosition() : 0.0;
    scene.beatsPerBar = session.Song().BeatsPerBar();

    const ChartClip* clip = session.CurrentClip();

    // A learn section's dots keep coming (and being judged) until nextClipShowing flips true -
    // at the scheduled advance, or earlier (when c_PreviewNextClipBeforeHandoff) once the next
    // clip's first note comes within c_BeatsAhead of now.
    bool isLearnSection =
        clip && session.Phase() == GamePhase::Learning && session.CurrentSectionKind() == SectionKind::Learn;
    bool nowPassing = isLearnSection && session.IsPassing();

    if (isLearnSection)
    {
        scene.hitsMeterIsDontFail = clip->Mode() == LearnMode::DontFail;
    }
    // Visible for the whole learn section in both modes - a Pass meter holds at its full 1.0 value
    // after lock-in rather than hiding (see hitsMeterProgress/hitsMeterPulsing).
    scene.showHitsMeter = isLearnSection;
    scene.hitsMeterPulsing = nowPassing && !scene.hitsMeterIsDontFail;

    // DontFail mode only: detect a passing->failing reversal for the same clip tracked last frame,
    // before UpdateClipInstances runs. When IsPassing() reverts, PreviewClip() goes null and
    // UpdateClipInstances swaps m_nextClip to a loop-repeat prediction; detecting it here first
    // means m_nextClip still holds what's about to be discarded, so it can be exploded.
    bool sameClipAsLastFrame = m_currentClip && m_currentClip->chartClip == clip;
    bool justFailedThisFrame = sameClipAsLastFrame && m_prevPassing && !nowPassing;
    if (justFailedThisFrame && m_nextClip)
    {
        const ClipPlaythrough* explosionClip = m_nextClip.get();
        const ChartClip* failedNextChartClip = m_nextClip->chartClip;
        for (int lane = 0; lane < c_LaneCount; ++lane)
        {
            for (SceneNote& sceneNote :
                 NotesInRange(lane, m_nextClip->startBeat, scene.nowBeat - c_BeatsBehind, scene.nowBeat + c_BeatsAhead,
                              failedNextChartClip->LaneNotes(lane), failedNextChartClip->SpanBeats()))
            {
                sceneNote.clip = explosionClip;
                scene.explodingNotes.push_back(sceneNote);
            }
        }
    }

    // Hits meter fill amount (see NoteLaneScene::hitsMeterProgress). After justFailedThisFrame
    // (DontFail's freeze logic needs it) but before UpdateClipInstances (needs only clip/nowBeat).
    if (scene.showHitsMeter && scene.hitsMeterIsDontFail)
    {
        double originBeat = session.CurrentClipOriginBeat();
        double currentLoopStartBeat = CurrentLoopStartBeat(originBeat, scene.nowBeat, clip->SpanBeats());
        double liveProgress = clip->SpanBeats() > 0.0
                                   ? std::clamp((scene.nowBeat - currentLoopStartBeat) / clip->SpanBeats(), 0.0, 1.0)
                                   : 0.0;
        if (nowPassing)
        {
            // Live while passing - real elapsed time within this loop.
            scene.hitsMeterProgress = liveProgress;
        }
        else
        {
            if (justFailedThisFrame || m_dontFailFrozenLoopStartBeat < 0.0 ||
                std::abs(currentLoopStartBeat - m_dontFailFrozenLoopStartBeat) > 1e-6)
            {
                // The miss dropped this to failing this frame (freeze at the live value so it
                // "stops filling"), or a whole loop elapsed while failing (this attempt restarted,
                // so reset to 0).
                m_dontFailFrozenProgress = justFailedThisFrame ? liveProgress : 0.0;
                m_dontFailFrozenLoopStartBeat = currentLoopStartBeat;
            }
            // Same failing stretch, same loop - hold steady rather than recomputing from nowBeat.
            scene.hitsMeterProgress = m_dontFailFrozenProgress;
        }
    }
    else if (scene.showHitsMeter && clip->HitsRequired() > 0)
    {
        scene.hitsMeterProgress =
            std::clamp(static_cast<double>(session.CurrentStreak()) / clip->HitsRequired(), 0.0, 1.0);
    }

    UpdateClipInstances(session, scene.nowBeat);

    auto clipInstanceName = [](const ClipPlaythrough* instance)
    { return instance ? instance->chartClip->Name() : L"(none)"; };
    scene.debugPreviousClipName = clipInstanceName(m_previousClip.get());
    scene.debugCurrentClipName = clipInstanceName(m_currentClip.get());
    scene.debugNextClipName = clipInstanceName(m_nextClip.get());

    // Whichever clip is most relevant now - the actively-playing one if there is one, otherwise
    // whatever's about to start (the count-in, or a Reset's zero-time gap).
    scene.primaryClip = m_currentClip ? m_currentClip.get() : m_nextClip.get();

    bool nextClipShowing = false;

    // Caps how far the judged pass tiles this pattern - nowBeat + c_BeatsAhead by default,
    // tightened to the current candidate advance below, since a note judged past that might belong
    // to a repeat that never plays. The gap is filled unjudged by the preview passes below.
    double notesUpperBoundBeat = scene.nowBeat + c_BeatsAhead;

    if (isLearnSection)
    {
        double secondsPerBeat = 60.0 / session.Song().Bpm();
        double advanceAtBeat = session.PendingAdvanceAtSeconds() / secondsPerBeat;

        // DontFail never takes the early branch below: its notes stay live (drawn AND judged) to
        // the section's actual advance, since GameSession keeps judging them and a DontFail miss
        // acts on one the player was never shown. The gap-fill CollectNotes call below still blends
        // in the next section / a loop repeat, just without hiding this clip's tail.
        if (!c_PreviewNextClipBeforeHandoff || session.PreviewClip() == nullptr ||
            clip->Mode() == LearnMode::DontFail)
        {
            // Nothing to hand off to yet - keep this clip's dots/judging live until the scheduled
            // advance rather than going blank.
            nextClipShowing = (scene.nowBeat >= advanceAtBeat);
            notesUpperBoundBeat = std::min(notesUpperBoundBeat, advanceAtBeat);
        }
        else
        {
            double earliestOnset = -1.0;
            for (int lane = 0; lane < c_LaneCount; ++lane)
            {
                double onset = session.PreviewFirstOnsetBeatForLane(lane);
                if (onset >= 0.0 && (earliestOnset < 0.0 || onset < earliestOnset))
                {
                    earliestOnset = onset;
                }
            }

            double visibleAtBeat =
                (earliestOnset >= 0.0) ? (earliestOnset - c_BeatsAhead) : (advanceAtBeat - c_BeatsAhead);
            double triggerBeat = (visibleAtBeat <= advanceAtBeat) ? visibleAtBeat : (advanceAtBeat - c_BeatsAhead);
            nextClipShowing = (scene.nowBeat >= triggerBeat);
        }
    }

    // Pass mode only: once passing (a one-way latch), force this true for the rest of the section,
    // overriding the branch above. This makes justHandedOff fire on the same frame as justLockedIn,
    // so the exploding-range logic clears this clip's entire visible window at once.
    if (isLearnSection && nowPassing && clip->Mode() == LearnMode::Pass)
    {
        nextClipShowing = true;
    }

    bool isLiveJudging = isLearnSection && !nextClipShowing;

    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        scene.receptors[lane].held = isLiveJudging && session.IsLaneHeld(lane);
    }

    // Edge-triggered flags: true only on the frame each condition first becomes true, so a renderer
    // reacts once. justLockedIn covers both a first-ever pass and a DontFail failing->passing re-entry.
    scene.justLockedIn = nowPassing && !m_prevPassing;
    scene.justHandedOff = nextClipShowing && !m_prevNotesHandoff;
    scene.justFailed = justFailedThisFrame;

    if ((scene.justHandedOff || scene.justLockedIn) && clip)
    {
        // m_currentClip still mirrors session.CurrentClip() here - the transition that would retire
        // it into m_previousClip hasn't happened on the frame justHandedOff/justLockedIn first fires.
        const ClipPlaythrough* explosionClip = m_currentClip.get();
        double originBeat = session.CurrentClipOriginBeat();

        auto addExplodingRange = [&](double fromBeat, double toBeat)
        {
            for (int lane = 0; lane < c_LaneCount; ++lane)
            {
                for (SceneNote& sceneNote :
                     NotesInRange(lane, originBeat, fromBeat, toBeat, clip->LaneNotes(lane), clip->SpanBeats()))
                {
                    sceneNote.clip = explosionClip;
                    scene.explodingNotes.push_back(sceneNote);
                }
            }
        };

        // The outgoing clip's own still-on-screen judged notes, right as
        // the handoff happens.
        if (scene.justHandedOff)
        {
            addExplodingRange(scene.nowBeat - c_BeatsBehind, notesUpperBoundBeat);
        }
        // Whatever the self-repeat preview (beyond notesUpperBoundBeat)
        // was still showing, right as lock-in makes that preview obsolete.
        if (scene.justLockedIn)
        {
            addExplodingRange(notesUpperBoundBeat, scene.nowBeat + c_BeatsAhead);
        }
    }

    m_prevPassing = nowPassing;
    m_prevNotesHandoff = nextClipShowing;

    // m_nextClip may mirror a real GameSession preview or a synthetic predicted loop repeat of the
    // current clip. For the latter, PreviewClipOriginBeat() is -1 and the "reveal each lane's own
    // first note" sentinel doesn't apply - a predicted loop is an ordinary repeat, revealed from
    // its own start beat.
    bool nextIsRealPreview = session.PreviewClip() != nullptr;
    double nextOriginBeat = nextIsRealPreview ? session.PreviewClipOriginBeat() : session.CurrentClipOriginBeat();
    double nextFromBeat = nextIsRealPreview ? -1.0 : (m_nextClip ? m_nextClip->startBeat : -1.0);

    // While the player can act, draw the live judged pass; otherwise draw only the upcoming clip's
    // preview, from each lane's first required note onward so lanes reveal one at a time.
    if (isLiveJudging)
    {
        CollectNotes(session, m_currentClip.get(), session.CurrentClipOriginBeat(), /*judged=*/true,
                     scene.nowBeat - c_BeatsBehind, notesUpperBoundBeat, scene);
    }
    else
    {
        CollectNotes(session, m_nextClip.get(), nextOriginBeat, /*judged=*/false, nextFromBeat, notesUpperBoundBeat,
                     scene);
    }

    // Fill the gap the judged pass leaves before notesUpperBoundBeat: preview more of this clip's
    // own pattern if it's not locked in (its loop will repeat), or the real next clip if it is.
    // Both unjudged, layered on top of the judged pass. Guarded so a zero-width range isn't passed
    // to CollectNotes, whose one-bar-back tail-catch would otherwise duplicate a note on the boundary.
    if (notesUpperBoundBeat < scene.nowBeat + c_BeatsAhead - 1e-9)
    {
        if (isLiveJudging && !session.IsPassing())
        {
            CollectNotes(session, m_currentClip.get(), session.CurrentClipOriginBeat(), /*judged=*/false,
                         notesUpperBoundBeat, scene.nowBeat + c_BeatsAhead, scene);
        }
        else if (isLiveJudging)
        {
            CollectNotes(session, m_nextClip.get(), nextOriginBeat, /*judged=*/false, nextFromBeat,
                         scene.nowBeat + c_BeatsAhead, scene);
        }
    }

    switch (session.Phase())
    {
        case GamePhase::Idle:
            scene.statusText = L"Load a chart, then Start";
            break;
        case GamePhase::CountIn:
            scene.statusText = L"Get ready...";
            break;
        case GamePhase::Learning:
            if (session.CurrentSectionKind() == SectionKind::Break)
            {
                scene.statusText = clip ? (clip->DisplayName() + L" - Listen...") : L"...";
            }
            else if (clip)
            {
                scene.statusText = clip->DisplayName();
            }
            break;
        case GamePhase::Complete:
            scene.statusText = L"Song complete!";
            break;
    }

    // Visible for the whole song except Idle, where there's no score yet.
    if (session.Phase() != GamePhase::Idle)
    {
        scene.scoreText = L"Score " + FormatScoreWithCommas(session.CurrentScore());

        int bank = session.CurrentBank();
        if (bank > 0)
        {
            scene.bankText = L"+" + FormatScoreWithCommas(bank);
        }

        // x1 (the base rate) is not shown - the multiplier readout only appears once boosted.
        int multiplier = session.CurrentMultiplier();
        if (multiplier > 1)
        {
            scene.multiplierText = L"x" + std::to_wstring(multiplier);
        }
    }

    // Overrides the switch above - only the HUD text changes while paused; scene.nowBeat is already
    // frozen, since it's read from the paused session.Clock().
    if (session.IsPaused())
    {
        scene.statusText = L"Paused";
    }

    return scene;
}
