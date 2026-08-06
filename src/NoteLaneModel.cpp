#include "NoteLaneModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// When true (the current setting), a section hands its dots off early to
// the next clip's preview as soon as that clip's own notes are due to
// appear - so the upcoming clip's notes scroll down from the top like any
// other, instead of its entire kBeatsAhead-deep window materializing at
// once the instant it becomes current (confirmed real symptom: a note
// landing near "now" at handoff read as already-past-the-judge-line,
// since nothing had scrolled it into view first). False keeps a section's
// dots/judging live for its whole duration, right up to the scheduled
// advance, with no lead time for the next clip's own preview at all. See
// nextClipShowing below.
constexpr bool kPreviewNextClipBeforeHandoff = true;

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
    // Never tile a bar before the clip's own origin - bar 0 is this clip's
    // first-ever repetition, so there's no earlier content to show. Without
    // this clamp, a fromBeat landing just before originBeat (routine right
    // when a fresh clip's live-judged pass starts, since it always looks
    // kBeatsBehind "now") synthesizes a phantom copy of a note near the
    // *end* of the pattern, one spanBeats too early.
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
            // toBeat is exclusive: for the live-judged pass it's the
            // clip's own scheduled advance beat, always a whole-loop
            // boundary from this clip's origin (ChartTiming::
            // ComputeLearnAdvanceSeconds/ComputeBreakAdvance) - so a note
            // at local offset 0 has a bar-tiled candidate landing exactly
            // on toBeat, representing a *next* repetition that never
            // actually plays. An inclusive check here would draw that
            // phantom note.
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

void NoteLaneModel::CollectNotes(const GameSession& session, const ClipInstance* instance, double originBeat,
                                  bool judged, double fromBeat, double upperBoundBeat, NoteLaneScene& scene) const
{
    if (!instance)
    {
        return;
    }
    const ChartClip* drawClip = instance->chartClip;

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        double dotsFromBeat = fromBeat;
        if (dotsFromBeat < 0.0)
        {
            dotsFromBeat = session.PreviewFirstOnsetBeatForLane(lane);
            if (dotsFromBeat < 0.0)
            {
                continue;
            }
        }

        std::vector<SceneNote> visibleNotes = NotesInRange(lane, originBeat, dotsFromBeat, upperBoundBeat,
                                                             drawClip->laneNotes[lane], drawClip->spanBeats);
        // Build with -DRHYTHM_DEBUG_RENDER (add to the Rhythm target's own
        // target_compile_definitions in CMakeLists.txt, not left on by
        // default) to trace exactly what NotesInRange tiles for each
        // lane/pass, every frame it returns anything - the only way to
        // catch a phantom/duplicate-note rendering bug from outside the
        // game itself (a headless GameSession diagnostic never renders
        // anything to inspect). Redirect stderr to a file (e.g. via
        // PowerShell's Start-Process -RedirectStandardError) and let the
        // game free-run - it advances on a fixed schedule regardless of
        // player input, so no interaction is needed.
#ifdef RHYTHM_DEBUG_RENDER
        if (!visibleNotes.empty())
        {
            std::fwprintf(stderr, L"[NoteLaneModel] judged=%d clip=%ls lane=%d origin=%.4f from=%.4f to=%.4f:",
                           judged ? 1 : 0, drawClip->name.c_str(), lane, originBeat, dotsFromBeat, upperBoundBeat);
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

            // Upcoming notes stay Normal (a renderer colors that by
            // clip->color). The instant a press starts a note correctly
            // (within tolerance) it becomes Held and stays that way
            // through the hold; a release that's too early/late (or a
            // press-window timeout with no press at all) resolves it to
            // Hit/Miss - both then hold for the rest of the note's time
            // on screen, matching the real outcome rather than reverting
            // to Normal.
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
                }
                else if (result == JudgementResult::Miss)
                {
                    sceneNote.state = NoteVisualState::Miss;
                }
                else if (sceneNote.startBeat < session.NextExpectedBeatForLane(lane) - 1e-6)
                {
                    // Never held or judged - only possible for a clip's
                    // first-ever appearance, which anchors to its
                    // pattern's true beginning (GameSession's
                    // ClipVoice::originEstablished) and so can start
                    // partway into a bar. Never included in the scene.
                    continue;
                }
            }

            scene.notes.push_back(sceneNote);
        }
    }
}

std::unique_ptr<ClipInstance> NoteLaneModel::MakeClipInstance(const GameSession& session, const ChartClip* chartClip,
                                                                double startBeat)
{
    if (!chartClip)
    {
        return nullptr;
    }
    auto instance = std::make_unique<ClipInstance>();
    instance->chartClip = chartClip;
    instance->startBeat = startBeat;
    const ChartClip* base = session.Song().clips.data();
    instance->color = ClipColor::ForIndex(static_cast<int>(chartClip - base));
    return instance;
}

namespace
{
// Which beat the loop repetition covering nowBeat actually started at,
// given the clip's own persistent origin - originBeat itself for the very
// first repetition, originBeat + spanBeats for the second, and so on.
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
        currentChartClip ? CurrentLoopStartBeat(session.CurrentClipOriginBeat(), nowBeat, currentChartClip->spanBeats)
                          : 0.0;

    // Identity is the (chartClip, startBeat) pair, not chartClip alone -
    // same clip, same section, but a new loop repetition has begun is
    // still a new playthrough (same currentChartClip, different
    // currentStartBeat), and must be detected here exactly like a genuine
    // section transition is.
    const ChartClip* trackedCurrentClip = m_currentClip ? m_currentClip->chartClip : nullptr;
    bool currentChanged = trackedCurrentClip != currentChartClip ||
                           (currentChartClip && std::abs(m_currentClip->startBeat - currentStartBeat) > 1e-6);
    if (currentChanged)
    {
        // Whatever m_currentClip was a moment ago becomes m_previousClip,
        // exactly once, right here.
        m_previousClip = std::move(m_currentClip);

        bool nextMatches = m_nextClip && m_nextClip->chartClip == currentChartClip &&
                            std::abs(m_nextClip->startBeat - currentStartBeat) <= 1e-6;
        if (nextMatches)
        {
            // Whatever we were already predicting (a real next section, or
            // this same clip's own predicted loop repeat) turned out to be
            // exactly right - reuse that same instance rather than building
            // a redundant new one.
            m_currentClip = std::move(m_nextClip);
        }
        else
        {
            m_currentClip = MakeClipInstance(session, currentChartClip, currentStartBeat);
        }
    }
    if (m_currentClip)
    {
        // A fresh (or freshly-promoted) instance already starts at false;
        // this also carries a same-instance run's passing state forward as
        // it happens - including DontFail mode's reversals, both
        // directions.
        m_currentClip->passing = session.IsPassing();
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
        // session.PreviewClip() has nothing to offer here - an unlocked
        // Learn section might still repeat any number of further loops
        // before it actually advances (see PreviewClip()'s own comment) -
        // but a predicted repeat of the CURRENT clip's own pattern is
        // legitimate to preview, unlike a real (and possibly wrong) guess
        // at the next section. Its start beat is simply one spanBeats past
        // the current instance's own.
        previewChartClip = m_currentClip->chartClip;
        previewStartBeat = m_currentClip->startBeat + previewChartClip->spanBeats;
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
    const std::vector<ChartClip>& clips = session.Song().clips;
    const ChartClip* clipsBase = clips.empty() ? nullptr : clips.data();
    if (clipsBase == m_lastSongClipsBase)
    {
        return;
    }
    m_lastSongClipsBase = clipsBase;
    m_previousClip.reset();
    m_currentClip.reset();
    m_nextClip.reset();
    // Also stale otherwise: a leftover true from the old song could
    // suppress the new song's own first legitimate justLockedIn/justFailed/
    // justHandedOff edge (nowX && !m_prevX never firing once for it).
    m_prevPassing = false;
    m_prevNotesHandoff = false;
    m_prevNowBeat = 0.0;
}

NoteLaneScene NoteLaneModel::BuildScene(const GameSession& session)
{
    NoteLaneScene scene;
    ResetIfSongChanged(session);

    scene.clockRunning = session.Phase() != GamePhase::Idle;
    scene.nowBeat = scene.clockRunning ? session.Clock().BeatPosition() : 0.0;
    scene.beatsPerBar = session.Song().beatsPerBar;

    const ChartClip* clip = session.CurrentClip();

    // A learn section's dots keep coming (and being judged) until
    // nextClipShowing flips true - either at the scheduled advance itself,
    // or (only while kPreviewNextClipBeforeHandoff is true) earlier, once
    // the next clip's first note comes within kBeatsAhead of now.
    bool isLearnSection =
        clip && session.Phase() == GamePhase::Learning && session.CurrentSectionKind() == SectionKind::Learn;
    bool nowPassing = isLearnSection && session.IsPassing();

    scene.showHitsMeter = isLearnSection && !nowPassing;
    if (isLearnSection)
    {
        scene.hitsMeterIsDontFail = clip->learnMode == LearnMode::DontFail;
    }
    if (scene.showHitsMeter && clip->hitsRequired > 0)
    {
        scene.hitsMeterProgress =
            std::clamp(static_cast<double>(session.CurrentStreak()) / clip->hitsRequired, 0.0, 1.0);
    }

    // DontFail mode only: detect a passing->failing reversal for the SAME
    // clip already tracked as m_currentClip last frame (ruling out an
    // ordinary section/loop change - a different event UpdateClipInstances'
    // own identity check already handles) before calling UpdateClipInstances
    // below. The instant IsPassing() reverts, session.PreviewClip() goes
    // null (see GameSession::PreviewSectionIndex()), which is exactly what
    // makes UpdateClipInstances silently swap m_nextClip from the real
    // next-section preview to a loop-repeat prediction of this same clip -
    // detecting the transition here, first, means m_nextClip still holds
    // whatever was about to be thrown away, so it can still be exploded.
    bool sameClipAsLastFrame = m_currentClip && m_currentClip->chartClip == clip;
    bool justFailedThisFrame = sameClipAsLastFrame && m_prevPassing && !nowPassing;
    if (justFailedThisFrame && m_nextClip)
    {
        const ClipInstance* explosionClip = m_nextClip.get();
        const ChartClip* failedNextChartClip = m_nextClip->chartClip;
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            for (SceneNote& sceneNote :
                 NotesInRange(lane, m_nextClip->startBeat, scene.nowBeat - kBeatsBehind, scene.nowBeat + kBeatsAhead,
                              failedNextChartClip->laneNotes[lane], failedNextChartClip->spanBeats))
            {
                sceneNote.clip = explosionClip;
                scene.explodingNotes.push_back(sceneNote);
            }
        }
    }

    UpdateClipInstances(session, scene.nowBeat);

    auto clipInstanceName = [](const ClipInstance* instance)
    { return instance ? instance->chartClip->name : L"(none)"; };
    scene.debugPreviousClipName = clipInstanceName(m_previousClip.get());
    scene.debugCurrentClipName = clipInstanceName(m_currentClip.get());
    scene.debugNextClipName = clipInstanceName(m_nextClip.get());

    // Whichever clip is most relevant right now - the actively-playing one
    // if there is one (Learn or Break alike; CurrentClip() is non-null for
    // both), otherwise whatever's about to start (the count-in, or a
    // Reset's own zero-time gap).
    scene.primaryClip = m_currentClip ? m_currentClip.get() : m_nextClip.get();

    bool nextClipShowing = false;

    // Caps how far the live *judged* pass tiles this clip's pattern -
    // nowBeat + kBeatsAhead by default, tightened to the section's current
    // candidate advance in the "nothing to hand off to" branch below: a
    // note judged past that point might belong to a repeat that never
    // plays, since the section could lock in there instead of repeating
    // (see GameSession::Update). The resulting gap is filled back in,
    // unjudged, by the self-repeat/next-clip preview passes below.
    double notesUpperBoundBeat = scene.nowBeat + kBeatsAhead;

    if (isLearnSection)
    {
        double secondsPerBeat = 60.0 / session.Song().bpm;
        double advanceAtBeat = session.PendingAdvanceAtSeconds() / secondsPerBeat;

        // DontFail mode never takes the early branch below, regardless of
        // kPreviewNextClipBeforeHandoff/PreviewClip() - its own notes stay
        // live (drawn AND judged) all the way to the section's actual
        // advance, unlike Pass mode's post-lock-in handoff. GameSession's
        // own judging (the press-phase timeout in Update()) has no idea
        // this clip's notes are being hidden early - it keeps judging every
        // one of them for as long as the section is current, so hiding one
        // here would silently miss-judge something the player was never
        // shown, which a DontFail miss then acts on (unlike Pass mode,
        // where a miss after passing is already a no-op - see
        // SectionInstance::RegisterMiss). The "stream next section in
        // early while passing / show a loop repeat while failing" nuance
        // still happens - see the gap-fill CollectNotes call below, which
        // already blends in exactly that once notesUpperBoundBeat is
        // capped short of nowBeat+kBeatsAhead - just without ever hiding
        // this clip's own tail to make room for it.
        if (!kPreviewNextClipBeforeHandoff || session.PreviewClip() == nullptr ||
            clip->learnMode == LearnMode::DontFail)
        {
            // Nothing to hand off to yet (no preview, or early handoff is
            // disabled) - keep this clip's own dots/judging live right up
            // until the actual scheduled advance, rather than going blank
            // for however long the wait until the real advance takes.
            nextClipShowing = (scene.nowBeat >= advanceAtBeat);
            notesUpperBoundBeat = std::min(notesUpperBoundBeat, advanceAtBeat);
        }
        else
        {
            double earliestOnset = -1.0;
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                double onset = session.PreviewFirstOnsetBeatForLane(lane);
                if (onset >= 0.0 && (earliestOnset < 0.0 || onset < earliestOnset))
                {
                    earliestOnset = onset;
                }
            }

            double visibleAtBeat =
                (earliestOnset >= 0.0) ? (earliestOnset - kBeatsAhead) : (advanceAtBeat - kBeatsAhead);
            double triggerBeat = (visibleAtBeat <= advanceAtBeat) ? visibleAtBeat : (advanceAtBeat - kBeatsAhead);
            nextClipShowing = (scene.nowBeat >= triggerBeat);
        }
    }

    // Pass mode only: passing is a one-way latch, so once reached this
    // stays forced true for the rest of the section's run - overriding
    // whichever branch above just computed nextClipShowing, and regardless
    // of whether there's even a real next section to hand off to yet. This
    // makes justHandedOff below fire on the exact same frame as
    // justLockedIn, so the exploding-range logic further down clears this
    // clip's *entire* currently-visible window at once instead of leaving
    // its notes live and judged until the next section's own notes happen
    // to scroll into view. DontFail mode is untouched - the branch above
    // already keeps its notes live all the way to the real advance
    // regardless of preview state (see its own comment for why).
    if (isLearnSection && nowPassing && clip->learnMode == LearnMode::Pass)
    {
        nextClipShowing = true;
    }

    bool isLiveJudging = isLearnSection && !nextClipShowing;

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        scene.receptors[lane].held = isLiveJudging && session.IsLaneHeld(lane);
    }

    // Edge-triggered flags: true only on the exact frame each condition
    // first becomes true, so a renderer can react once (a celebration
    // burst, an explosion) instead of every frame the condition holds.
    // justLockedIn covers both a first-ever pass and (DontFail mode) a
    // failing->passing re-entry - see NoteLaneScene::justLockedIn's own
    // comment for why it doesn't need a separate name for the latter.
    scene.justLockedIn = nowPassing && !m_prevPassing;
    scene.justHandedOff = nextClipShowing && !m_prevNotesHandoff;
    scene.justFailed = justFailedThisFrame;

    if ((scene.justHandedOff || scene.justLockedIn) && clip)
    {
        // m_currentClip still mirrors session.CurrentClip() == clip here:
        // GameSession's own transition to whatever comes next (which is
        // what would make UpdateClipInstances next frame retire this into
        // m_previousClip) hasn't happened yet on the very frame
        // justHandedOff/justLockedIn first fires - see UpdateClipInstances'
        // own comment.
        const ClipInstance* explosionClip = m_currentClip.get();
        double originBeat = session.CurrentClipOriginBeat();

        auto addExplodingRange = [&](double fromBeat, double toBeat)
        {
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                for (SceneNote& sceneNote :
                     NotesInRange(lane, originBeat, fromBeat, toBeat, clip->laneNotes[lane], clip->spanBeats))
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
            addExplodingRange(scene.nowBeat - kBeatsBehind, notesUpperBoundBeat);
        }
        // Whatever the self-repeat preview (beyond notesUpperBoundBeat)
        // was still showing, right as lock-in makes that preview obsolete.
        if (scene.justLockedIn)
        {
            addExplodingRange(notesUpperBoundBeat, scene.nowBeat + kBeatsAhead);
        }
    }

    // Pass mode only: once locked in, this clip's own notes explode
    // immediately (see the nextClipShowing override above) and never draw
    // again for the rest of this section's run - but its audio keeps
    // looping in the background regardless (to build up the arrangement),
    // so give the judge line a synthetic "hit" flash every time one of its
    // pattern's onsets crosses it anyway, so the beat still visibly lands
    // on something instead of the judge line going dark for however long
    // the section has left. Compared against last frame's own nowBeat
    // (m_prevNowBeat) so each onset only fires once, right as it crosses;
    // skipped if nowBeat has gone backwards since last frame (a fresh
    // Start()/Stop() - nothing to catch up on, and a descending range
    // would confuse NotesInRange's own bar-tiling math).
    if (isLearnSection && nowPassing && clip->learnMode == LearnMode::Pass && scene.nowBeat >= m_prevNowBeat)
    {
        double passOriginBeat = session.CurrentClipOriginBeat();
        for (int lane = 0; lane < kLaneCount; ++lane)
        {
            for (const SceneNote& crossed : NotesInRange(lane, passOriginBeat, m_prevNowBeat, scene.nowBeat + 1e-6,
                                                           clip->laneNotes[lane], clip->spanBeats))
            {
                if (crossed.startBeat > m_prevNowBeat + 1e-9 && crossed.startBeat <= scene.nowBeat + 1e-9)
                {
                    scene.passLineHitLanes.push_back(lane);
                }
            }
        }
    }
    m_prevNowBeat = scene.nowBeat;

    m_prevPassing = nowPassing;
    m_prevNotesHandoff = nextClipShowing;

    // m_nextClip may mirror a real GameSession preview, or (see
    // UpdateClipInstances) a synthetic predicted loop repeat of the
    // CURRENT clip that GameSession itself knows nothing about - in the
    // latter case, session.PreviewClipOriginBeat() is meaningless (-1,
    // nothing being previewed as far as GameSession is concerned), and the
    // -1.0 "reveal each lane's own first note" sentinel doesn't apply
    // either (that's for a genuinely fresh debut; a predicted loop is
    // just an ordinary repeat, so every lane reveals together from the
    // predicted loop's own start beat, same as any other repeat).
    bool nextIsRealPreview = session.PreviewClip() != nullptr;
    double nextOriginBeat = nextIsRealPreview ? session.PreviewClipOriginBeat() : session.CurrentClipOriginBeat();
    double nextFromBeat = nextIsRealPreview ? -1.0 : (m_nextClip ? m_nextClip->startBeat : -1.0);

    // While the player can act, draw the live judged pass; otherwise
    // (count-in, break/reset/background, or just past nextClipShowing)
    // draw only the upcoming clip's preview, from each lane's own first
    // required note onward - so lanes reveal one at a time instead of
    // popping in as a batch.
    if (isLiveJudging)
    {
        CollectNotes(session, m_currentClip.get(), session.CurrentClipOriginBeat(), /*judged=*/true,
                     scene.nowBeat - kBeatsBehind, notesUpperBoundBeat, scene);
    }
    else
    {
        CollectNotes(session, m_nextClip.get(), nextOriginBeat, /*judged=*/false, nextFromBeat, notesUpperBoundBeat,
                     scene);
    }

    // The judged pass never tiles past notesUpperBoundBeat, which would
    // otherwise leave a growing gap right before every such boundary. Fill
    // it with whichever is true right now: not locked in, so the clip's
    // own loop will repeat (GameSession::Update) - preview more of its own
    // pattern past the cap; or locked in, so the section is about to
    // advance - preview the real next clip instead. Both are unjudged and
    // layered on top of the still-live judged pass.
    //
    // Only when notesUpperBoundBeat is genuinely capped short of
    // nowBeat+kBeatsAhead is there an actual gap to fill - the common case
    // (nothing capping it) means the judged pass already reached the full
    // kBeatsAhead reach on its own, so this range is empty. Calling
    // CollectNotes anyway with a zero-width [notesUpperBoundBeat,
    // notesUpperBoundBeat] range wouldn't just be a no-op: NotesInRange's
    // own "look one bar back" tail-catch (see its own comment) would
    // re-discover whatever note the judged pass already placed right at
    // that shared boundary and duplicate it into the scene - confirmed
    // real repro, a note whose span straddles nowBeat+kBeatsAhead gets
    // counted twice for as long as that straddle lasts.
    if (notesUpperBoundBeat < scene.nowBeat + kBeatsAhead - 1e-9)
    {
        if (isLiveJudging && !session.IsPassing())
        {
            CollectNotes(session, m_currentClip.get(), session.CurrentClipOriginBeat(), /*judged=*/false,
                         notesUpperBoundBeat, scene.nowBeat + kBeatsAhead, scene);
        }
        else if (isLiveJudging)
        {
            CollectNotes(session, m_nextClip.get(), nextOriginBeat, /*judged=*/false, nextFromBeat,
                         scene.nowBeat + kBeatsAhead, scene);
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
                scene.statusText = clip ? (clip->displayName + L" - Listen...") : L"...";
            }
            else if (clip)
            {
                scene.statusText = clip->displayName;
            }
            break;
        case GamePhase::Complete:
            scene.statusText = L"Song complete!";
            break;
    }

    // Overrides whatever the switch above chose - the phase/clip itself
    // hasn't changed while paused (see GameSession::Pause's own comment),
    // just what the HUD should say about it. scene.nowBeat (and everything
    // derived from it - notes, receptor pulse, beat-synced background glow)
    // is already frozen for free, since it's read from the now-paused
    // session.Clock() same as any other frame.
    if (session.IsPaused())
    {
        scene.statusText = L"Paused";
    }

    return scene;
}
