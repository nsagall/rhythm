#include "BlockSchedule.h"

#include <algorithm>
#include <cmath>

#include "ChartTiming.h"
#include "LaneConfig.h"

namespace BlockSchedule
{

namespace
{

// One lane's next not-yet-consumed onset, for the k-way (k <= kLaneCount)
// merge walk below - only lanes with at least one note participate, since
// a lane with none can never be judged/pressed at all (matches
// GameSession::IsLaneJudgeable's own "nothing to press" case).
struct LaneFrontier
{
    int lane = 0;
    double beat = 0.0;
};

// Walks the merged, chronologically-sorted onsets across every lane with
// notes, starting from each lane's own anchor, for exactly hitsRequired
// pops - mirrors GameSession::RegisterHit's shared streak incrementing once
// per judged hit, in chronological order across all lanes, until it meets
// hits_required. Returns the beat of the hitsRequired-th pop for a perfect
// player (== the instant IsLockedIn() flips true, purely for the voice's
// own volume-switch timing - it no longer affects the section's own
// advance timing at all, see Build()'s own Learn case).
double WalkOnsetsForLockIn(double originBeat, const ChartClip& clip, const double anchors[kLaneCount],
                            int hitsRequired, double afterBeat)
{
    std::vector<LaneFrontier> frontier;
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (!clip.laneNotes[lane].empty())
        {
            frontier.push_back({lane, anchors[lane]});
        }
    }

    double lockInBeat = afterBeat;

    int hits = std::max(hitsRequired, 1);
    for (int hit = 0; hit < hits && !frontier.empty(); ++hit)
    {
        size_t minIdx = 0;
        for (size_t f = 1; f < frontier.size(); ++f)
        {
            if (frontier[f].beat < frontier[minIdx].beat)
            {
                minIdx = f;
            }
        }
        double beat = frontier[minIdx].beat;
        lockInBeat = beat;
        frontier[minIdx].beat = ChartTiming::NextOnsetAfter(originBeat, beat, clip, frontier[minIdx].lane);
    }
    return lockInBeat;
}

// Everything Build() needs to track per clip while walking the song's
// sections in order, mirroring GameSession::ClipInstance exactly - a clip's
// own phase origin is no longer part of this (see arrangementOriginSeconds
// in Build() itself - shared by every clip in the same continuously-
// sounding group, per ChartTiming.h's own namespace comment), only its
// playback state: a clip reused by a later section while still open from an
// earlier one (nothing has stopped it since) does NOT restart or re-seek; it
// just keeps going, and any lock-in-floor math that section computes uses
// the ORIGINAL start time, not its own. voiceIndex == -1 means "not
// currently open" - kept as a plain index into schedule.voices (not a
// pointer) since that vector is still being push_back'd to while Build()
// runs, so a pointer into it would be unsafe until construction finishes;
// this never escapes Build(), unlike every ChartClip* elsewhere here.
struct ClipBuildState
{
    int voiceIndex = -1;
    double loopStartSeconds = -1.0;
};

} // namespace

Schedule Build(const ChartSong& song, const std::unordered_map<const ChartClip*, double>& stemDurationsByClip)
{
    Schedule schedule;
    if (song.bpm <= 0.0)
    {
        return schedule;
    }

    double secondsPerBeat = 60.0 / song.bpm;
    double tFallSeconds = kNoteFallBeats * secondsPerBeat;

    double t = 0.0;
    const ChartClip* queuedBackgroundClip = nullptr;
    int queuedBackgroundSectionIndex = -1;

    std::unordered_map<const ChartClip*, ClipBuildState> clipStates;

    // The current arrangement's shared phase reference - mirrors
    // GameSession::m_arrangementOriginSeconds exactly, including when it's
    // (re)established: valid whenever at least one voice is open, cleared by
    // stopAllVoices below (a Reset, or a Break - see its own comment for why
    // Break can invalidate unconditionally here even though the real game's
    // StopAllExcept sometimes doesn't).
    bool arrangementOriginValid = false;
    double arrangementOriginSeconds = 0.0;

    // Mirrors GameSession::EnsureArrangementOrigin exactly - see its own
    // comment.
    auto establishArrangementOrigin = [&](double atSeconds)
    {
        if (!arrangementOriginValid)
        {
            arrangementOriginValid = true;
            arrangementOriginSeconds = atSeconds;
        }
        return arrangementOriginSeconds;
    };

    // Closes every currently-open voice window and invalidates the
    // arrangement origin - called on entering a Break or Reset section (both
    // silence everything before anything else in the real game too, via
    // AudioEngine::StopAll()/StopAllExcept() + GameSession's own
    // ClipInstance::isPlaying fill). Unconditional here even for Break,
    // unlike the real game's StopAllExcept (which spares its own
    // already-playing clip) - schedule-equivalent regardless, since every
    // VoiceWindow this section's own clip had open closes and reopens at the
    // same instant either way, and the alignment invariant (ChartTiming.h's
    // namespace comment) guarantees re-establishing its origin right here
    // lands on the exact same phase a preserved one would have.
    auto stopAllVoices = [&](double atSeconds)
    {
        for (VoiceWindow& window : schedule.voices)
        {
            if (window.stopSeconds < 0.0)
            {
                window.stopSeconds = atSeconds;
            }
        }
        for (auto& entry : clipStates)
        {
            entry.second.voiceIndex = -1;
            entry.second.loopStartSeconds = -1.0;
        }
        arrangementOriginValid = false;
    };

    // Mirrors GameSession::StartClipLoop's own idempotency guard - a no-op
    // if this clip already has an open voice (from a Learn/Break/
    // Background section reached earlier and never since stopped).
    // Establishes the arrangement origin if nothing else has already,
    // mirroring StartClipLoop's own safety-net call, so a Background voice
    // (which never goes through Learn/Break's own explicit
    // pre-establishment) still gets one. Returns true only when a new
    // VoiceWindow was actually opened.
    auto startVoiceIfNeeded = [&](const ChartClip* clip, double atSeconds, double volumeBeforeLockIn,
                                   double volumeAfterLockIn, double lockInSecondsForThisStart, int sectionIndex)
    {
        ClipBuildState& state = clipStates[clip];
        if (state.voiceIndex >= 0)
        {
            return false;
        }
        double originSeconds = establishArrangementOrigin(atSeconds);
        VoiceWindow window;
        window.sectionIndex = sectionIndex;
        window.clip = clip;
        window.startSeconds = atSeconds;
        window.stopSeconds = -1.0;
        window.volumeBeforeLockIn = volumeBeforeLockIn;
        window.volumeAfterLockIn = volumeAfterLockIn;
        window.lockInSeconds = lockInSecondsForThisStart;
        window.originSeconds = originSeconds;
        schedule.voices.push_back(window);
        state.voiceIndex = static_cast<int>(schedule.voices.size()) - 1;
        state.loopStartSeconds = atSeconds;
        return true;
    };

    for (size_t i = 0; i < song.sections.size(); ++i)
    {
        const ChartSection& section = song.sections[i];
        // The one unavoidable index resolution per section: converting the
        // immutable ChartSection's own file-format reference into a real
        // pointer, used for everything from here on instead of the int.
        const ChartClip* clip =
            section.clipIndex >= 0 ? &song.clips[static_cast<size_t>(section.clipIndex)] : nullptr;

        // "The next section begins" is exactly this point - realize
        // whatever the previous section (if it was Background) queued,
        // mirroring GameSession::BeginSection's own top-of-function check.
        if (queuedBackgroundClip != nullptr)
        {
            startVoiceIfNeeded(queuedBackgroundClip, t, queuedBackgroundClip->volume, queuedBackgroundClip->volume,
                                -1.0, queuedBackgroundSectionIndex);
            queuedBackgroundClip = nullptr;
        }

        switch (section.kind)
        {
            case SectionKind::Background:
                queuedBackgroundClip = clip;
                queuedBackgroundSectionIndex = static_cast<int>(i);
                break;

            case SectionKind::Reset:
                stopAllVoices(t);
                break;

            case SectionKind::Break:
            {
                // Closes every open voice window before starting its own
                // clip - see stopAllVoices' own comment for why this is a
                // plain unconditional close here even though the real
                // game's equivalent (StopAllExcept) skips its own clip.
                stopAllVoices(t);

                double stemDuration = stemDurationsByClip.at(clip);

                // stopAllVoices() just invalidated the arrangement origin
                // unconditionally, so this always (re)establishes it fresh,
                // right at t - startVoiceIfNeeded's own establishment
                // (moments later) is then just a no-op confirming the same
                // value.
                double originSeconds = establishArrangementOrigin(t);

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Break;
                entry.clip = clip;
                entry.sectionStartSeconds = t;
                entry.audioStartSeconds = t;
                entry.originSeconds = originSeconds;
                entry.lockInSeconds = -1.0;
                entry.loopSeconds = stemDuration;

                ChartTiming::BreakAdvance advance =
                    ChartTiming::ComputeBreakAdvance(originSeconds, t, stemDuration, section.loopCount, tFallSeconds);
                entry.loopCount = advance.loopCount;
                entry.endSeconds = advance.advanceSeconds;

                // stopAllVoices() just cleared every voice, so this always
                // opens fresh (matches the real StartClipLoop always
                // actually (re)starting a break's clip, phase-seeked at t).
                startVoiceIfNeeded(clip, t, clip->volume, clip->volume, -1.0, static_cast<int>(i));
                // Unlike Learn/Background, a break clip self-stops once its
                // own loop_count/kNoteFallBeats-extended duration elapses
                // (GameSession's finishedSection handling calls
                // StopClipLoop for a finished Break specifically).
                int voiceIdx = clipStates[clip].voiceIndex;
                schedule.voices[static_cast<size_t>(voiceIdx)].stopSeconds = entry.endSeconds;
                clipStates[clip].voiceIndex = -1;

                t = entry.endSeconds;
                schedule.entries.push_back(entry);
                break;
            }

            case SectionKind::Learn:
            {
                double stemDuration = stemDurationsByClip.at(clip);
                double afterBeat = t / secondsPerBeat - 1e-6;

                // Established here, ahead of the anchor computation below,
                // which needs it right away - startVoiceIfNeeded's own
                // establishment (moments later) is then just a no-op
                // confirming the same value.
                double originSeconds = establishArrangementOrigin(t);
                double originBeat = originSeconds / secondsPerBeat;

                // Covers both a clip's first-ever appearance and a later
                // section reusing one already mid-groove in one formula -
                // see ChartTiming::NextOnsetAfter's own comment for why.
                double anchors[kLaneCount];
                for (int lane = 0; lane < kLaneCount; ++lane)
                {
                    anchors[lane] = ChartTiming::NextOnsetAfter(originBeat, afterBeat, *clip, lane);
                }

                // Starts immediately and its own advance is scheduled right
                // away too, exactly like Break - mirrors
                // GameSession::BeginSection's Learn case exactly, which no
                // longer waits for any hits_required-th onset at all.
                double audioStartSeconds = t;
                double lockInBeat = WalkOnsetsForLockIn(originBeat, *clip, anchors, clip->hitsRequired, afterBeat);
                double lockInSeconds = lockInBeat * secondsPerBeat;

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Learn;
                entry.clip = clip;
                entry.sectionStartSeconds = t;
                entry.audioStartSeconds = audioStartSeconds;
                entry.originSeconds = originSeconds;
                entry.lockInSeconds = lockInSeconds;
                entry.loopSeconds = stemDuration;

                // If this clip is already playing (still open from an
                // earlier, un-stopped section), startVoiceIfNeeded is a
                // no-op and loopStartSeconds keeps its ORIGINAL value -
                // exactly mirroring how the real StartClipLoop's guard
                // leaves ClipInstance::loopStartSeconds untouched in that
                // case, so this section's own advance floor is computed
                // relative to when the clip truly started, not this
                // section's own start.
                startVoiceIfNeeded(clip, audioStartSeconds, clip->initVolume, clip->volume, lockInSeconds,
                                   static_cast<int>(i));
                double effectiveLoopStartSeconds = clipStates[clip].loopStartSeconds;

                entry.endSeconds = ChartTiming::ComputeLearnAdvanceSeconds(
                    originSeconds, t, effectiveLoopStartSeconds, stemDuration, section.loopCount, tFallSeconds);

                // Mirrors GameSession::Update's own finishedSection
                // handling: a learn section that hasn't locked in by its
                // own current candidate advance doesn't get abandoned - it
                // just repeats, extended by one more full loop, however
                // many times it takes until a perfect player's own walk
                // (lockInSeconds, computed above) actually falls within it.
                // hits_required exceeding however many onsets one loop of
                // the pattern offers (rare) just means more loops, never
                // "never locks in" - a perfect player pressing every note
                // always eventually reaches hits_required given enough
                // repeats, exactly like the live game's clip just keeps
                // looping rather than being stopped.
                //
                // Beyond merely locking in somewhere before the boundary,
                // this entry also can't end until the *next* clip's own
                // preview (PreviewClip(), gated on IsPassing() in the real
                // game) would have had a full tFallSeconds to actually show
                // before the hand-off - locking in with less than that left
                // gives the player little or no real on-screen warning
                // about what's coming, even though this section technically
                // passed. Both conditions collapse into the same single
                // floor below: the boundary must land at least tFallSeconds
                // after lockInSeconds - which is a strict superset of the
                // "doesn't fit in the natural window at all" case this
                // replaced, not a separate check. Shared with
                // GameSession::RegisterHit's own equivalent, reactive
                // fix-up (there, referenceSeconds is "now" instead of this
                // perfect player's own lockInSeconds) via
                // ChartTiming::ExtendAdvanceForFallLeadTime - see its own
                // doc comment.
                entry.endSeconds =
                    ChartTiming::ExtendAdvanceForFallLeadTime(entry.endSeconds, lockInSeconds, stemDuration, tFallSeconds);

                // Informational only - the total number of passes spanning
                // [audioStartSeconds, endSeconds), each exactly stemDuration
                // long (see Seek()'s own matching per-pass math). Seek()
                // independently re-derives the true loop/phase from
                // elapsedSeconds directly against the fields above, so this
                // can never desync playback even if it's slightly off at a
                // boundary.
                entry.loopCount = 1;
                if (stemDuration > 0.0)
                {
                    double span = entry.endSeconds - audioStartSeconds;
                    if (span > 1e-6)
                    {
                        entry.loopCount = static_cast<int>(std::ceil(span / stemDuration - 1e-9));
                        if (entry.loopCount < 1)
                        {
                            entry.loopCount = 1;
                        }
                    }
                }

                t = entry.endSeconds;
                schedule.entries.push_back(entry);
                break;
            }
        }
    }

    // Any Background still queued here was never realized (its clip was
    // the last section, or came right before one) - GameSession's own doc
    // comment calls this "acceptable, not an error", so it's simply never
    // added to schedule.voices at all. Any *realized* voice still open
    // (stopSeconds == -1) is genuinely still playing at the song's end -
    // left as-is, not force-closed here, per VoiceWindow's own documented
    // meaning of -1.
    schedule.totalSeconds = t;
    return schedule;
}

// See the header's own comment.
double ComputeFirstPassSeconds(double audioStartSeconds, double originSeconds, double loopSeconds)
{
    if (loopSeconds <= 0.0)
    {
        return 0.0;
    }
    double startPhase = std::fmod(audioStartSeconds - originSeconds, loopSeconds);
    double firstPassSeconds = loopSeconds - startPhase;
    if (firstPassSeconds <= 1e-9)
    {
        firstPassSeconds = loopSeconds;
    }
    return firstPassSeconds;
}

SeekResult Seek(const Schedule& schedule, double elapsedSeconds)
{
    SeekResult result;

    for (size_t i = 0; i < schedule.entries.size(); ++i)
    {
        const Entry& entry = schedule.entries[i];
        if (elapsedSeconds < entry.sectionStartSeconds || elapsedSeconds >= entry.endSeconds)
        {
            continue;
        }

        result.entryIndex = static_cast<int>(i);
        if (elapsedSeconds < entry.audioStartSeconds || entry.loopSeconds <= 0.0)
        {
            result.loopIndex = 0;
            result.phaseSeconds = 0.0;
        }
        else
        {
            // Pass 2 onward always spans exactly loopSeconds of real time -
            // see SeekResult::loopIndex's own comment for why the RENDERED
            // phaseSeconds is deliberately independent of the underlying
            // audio voice's absolute-time-aligned phase. But pass 1 is the
            // one exception: the real voice was phase-seeked to
            // fmod(audioStartSeconds - originSeconds, loopSeconds) when it
            // started (see ChartTiming::ComputeClipPhaseSeconds/
            // GameSession::StartClipLoop), so its own first buffer-wrap -
            // and therefore the real end of pass 1 - lands
            // loopSeconds - startPhase real seconds later, not a full
            // loopSeconds later. (ComputeLearnAdvanceSeconds/
            // ComputeBreakAdvance's advance targets are themselves always
            // exact multiples of loopSeconds measured from originSeconds -
            // i.e. exactly where the real, phase-seeked voice actually
            // wraps - so this is not an approximation: passes 2+ always land
            // perfectly full-length once pass 1's own real length is
            // accounted for.) Skipping this would make pass 1's rendered
            // sweep run at the wrong rate: reaching only
            // (loopSeconds-startPhase)/loopSeconds of the block's width
            // before the entry actually ends, instead of the full width -
            // visually "jumping to the end" of the block early, especially
            // stark for a clip whose first-ever appearance starts well
            // after a loop boundary (see BlockSchedule.cpp's own diagnostic
            // history for a confirmed real repro: a single-pass block that
            // only ever reached ~73% of its own width).
            double firstPassSeconds =
                ComputeFirstPassSeconds(entry.audioStartSeconds, entry.originSeconds, entry.loopSeconds);

            double loopOffset = elapsedSeconds - entry.audioStartSeconds;
            if (loopOffset < firstPassSeconds)
            {
                result.loopIndex = 1;
                result.phaseSeconds = (loopOffset / firstPassSeconds) * entry.loopSeconds;
            }
            else
            {
                double afterFirstPass = loopOffset - firstPassSeconds;
                result.loopIndex = 2 + static_cast<int>(std::floor(afterFirstPass / entry.loopSeconds));
                result.phaseSeconds = std::fmod(afterFirstPass, entry.loopSeconds);
            }
        }
        break;
    }
    // If elapsedSeconds is at/after schedule.totalSeconds (or the schedule
    // has no entries at all), entryIndex stays -1 - nothing audible.

    for (const VoiceWindow& window : schedule.voices)
    {
        bool stillOpenForever = window.stopSeconds < 0.0;
        if (elapsedSeconds >= window.startSeconds && (stillOpenForever || elapsedSeconds < window.stopSeconds))
        {
            double volume = (window.lockInSeconds >= 0.0 && elapsedSeconds < window.lockInSeconds)
                                 ? window.volumeBeforeLockIn
                                 : window.volumeAfterLockIn;
            result.activeVoices.push_back({window.clip, volume, window.originSeconds});
        }
    }

    return result;
}

// See the header's own comment.
double FirstEntrySecondsAtOrAfter(const Schedule& schedule, int sectionIndex)
{
    for (const Entry& entry : schedule.entries)
    {
        if (entry.sectionIndex >= sectionIndex)
        {
            return entry.sectionStartSeconds;
        }
    }
    return schedule.totalSeconds;
}

} // namespace BlockSchedule
