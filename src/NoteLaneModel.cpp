#include "NoteLaneModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// When true, a section hands its dots off early to the next clip's
// preview as soon as that clip's own notes are due to appear. False (the
// current setting) keeps a section's dots/judging live for its whole
// duration, right up to the scheduled advance. See nextClipShowing below.
constexpr bool kPreviewNextClipBeforeHandoff = false;

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

std::unique_ptr<ClipInstance> NoteLaneModel::MakeClipInstance(const GameSession& session, const ChartClip* chartClip)
{
    if (!chartClip)
    {
        return nullptr;
    }
    auto instance = std::make_unique<ClipInstance>();
    instance->chartClip = chartClip;
    const ChartClip* base = session.Song().clips.data();
    instance->color = ClipColor::ForIndex(static_cast<int>(chartClip - base));
    return instance;
}

void NoteLaneModel::UpdateClipInstances(const GameSession& session)
{
    const ChartClip* currentChartClip = session.CurrentClip();
    const ChartClip* trackedCurrent = m_currentClip ? m_currentClip->chartClip : nullptr;
    if (trackedCurrent != currentChartClip)
    {
        // A new playthrough is starting (or none is, anymore) - whatever
        // m_currentClip was a moment ago becomes m_previousClip, exactly
        // once, right here.
        m_previousClip = std::move(m_currentClip);

        const ChartClip* trackedNext = m_nextClip ? m_nextClip->chartClip : nullptr;
        if (currentChartClip && trackedNext == currentChartClip)
        {
            // The clip we were already previewing just went live - reuse
            // that same instance (same identity/color) rather than
            // building a redundant new one.
            m_currentClip = std::move(m_nextClip);
        }
        else
        {
            m_currentClip = MakeClipInstance(session, currentChartClip);
        }
    }
    if (m_currentClip)
    {
        // A fresh (or freshly-promoted) instance already starts at false;
        // this also carries a same-instance run's lock-in forward as it
        // happens.
        m_currentClip->lockedIn = session.IsLockedIn();
    }

    const ChartClip* previewChartClip = session.PreviewClip();
    const ChartClip* trackedNextNow = m_nextClip ? m_nextClip->chartClip : nullptr;
    if (trackedNextNow != previewChartClip)
    {
        m_nextClip = MakeClipInstance(session, previewChartClip);
    }
}

NoteLaneScene NoteLaneModel::BuildScene(const GameSession& session)
{
    NoteLaneScene scene;

    scene.clockRunning = session.Phase() != GamePhase::Idle;
    scene.nowBeat = scene.clockRunning ? session.Clock().BeatPosition() : 0.0;
    scene.beatsPerBar = session.Song().beatsPerBar;

    UpdateClipInstances(session);

    const ChartClip* clip = session.CurrentClip();

    // Whichever clip is most relevant right now - the actively-playing one
    // if there is one (Learn or Break alike; CurrentClip() is non-null for
    // both), otherwise whatever's about to start (the count-in, or a
    // Reset's own zero-time gap).
    scene.primaryClip = m_currentClip ? m_currentClip.get() : m_nextClip.get();

    // A learn section's dots keep coming (and being judged) until
    // nextClipShowing flips true - either at the scheduled advance itself,
    // or (only while kPreviewNextClipBeforeHandoff is true) earlier, once
    // the next clip's first note comes within kBeatsAhead of now.
    bool isLearnSection =
        clip && session.Phase() == GamePhase::Learning && session.CurrentSectionKind() == SectionKind::Learn;
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

        if (!kPreviewNextClipBeforeHandoff || session.PreviewClip() == nullptr)
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

    bool isLiveJudging = isLearnSection && !nextClipShowing;

    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        scene.receptors[lane].held = isLiveJudging && session.IsLaneHeld(lane);
    }

    // Edge-triggered flags: true only on the exact frame each condition
    // first becomes true, so a renderer can react once (a celebration
    // burst, an explosion) instead of every frame the condition holds.
    bool nowLockedIn = isLearnSection && session.IsLockedIn();
    scene.justLockedIn = nowLockedIn && !m_prevLockedIn;
    scene.justHandedOff = nextClipShowing && !m_prevNotesHandoff;

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

    m_prevLockedIn = nowLockedIn;
    m_prevNotesHandoff = nextClipShowing;

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
        CollectNotes(session, m_nextClip.get(), session.PreviewClipOriginBeat(), /*judged=*/false, -1.0,
                     notesUpperBoundBeat, scene);
    }

    // The judged pass never tiles past notesUpperBoundBeat, which would
    // otherwise leave a growing gap right before every such boundary. Fill
    // it with whichever is true right now: not locked in, so the clip's
    // own loop will repeat (GameSession::Update) - preview more of its own
    // pattern past the cap; or locked in, so the section is about to
    // advance - preview the real next clip instead. Both are unjudged and
    // layered on top of the still-live judged pass.
    if (isLiveJudging && !session.IsLockedIn())
    {
        CollectNotes(session, m_currentClip.get(), session.CurrentClipOriginBeat(), /*judged=*/false,
                     notesUpperBoundBeat, scene.nowBeat + kBeatsAhead, scene);
    }
    else if (isLiveJudging)
    {
        CollectNotes(session, m_nextClip.get(), session.PreviewClipOriginBeat(), /*judged=*/false, -1.0,
                     scene.nowBeat + kBeatsAhead, scene);
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

    return scene;
}
