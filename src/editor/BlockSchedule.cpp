#include "BlockSchedule.h"

#include <algorithm>
#include <cmath>

#include "LaneConfig.h"

namespace BlockSchedule
{

namespace
{

// One lane's next not-yet-consumed onset, for the k-way (k <= c_LaneCount)
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
double WalkOnsetsForLockIn(double originBeat, const ChartClip& clip, const double anchors[c_LaneCount],
                            int hitsRequired, double afterBeat)
{
    std::vector<LaneFrontier> frontier;
    for (int lane = 0; lane < c_LaneCount; ++lane)
    {
        if (!clip.LaneNotes(lane).empty())
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
        frontier[minIdx].beat = clip.NextOnsetAfter(originBeat, beat, frontier[minIdx].lane);
    }
    return lockInBeat;
}

} // namespace

Schedule Build(const ChartSong& song, const std::unordered_map<const ChartClip*, double>& stemDurationsByClip)
{
    Schedule schedule;
    if (song.Bpm() <= 0.0)
    {
        return schedule;
    }

    double secondsPerBeat = song.SecondsPerBeat();
    double tFallSeconds = c_NoteFallBeats * secondsPerBeat;

    double t = 0.0;
    const ChartClip* queuedBackgroundClip = nullptr;
    int queuedBackgroundSectionIndex = -1;

    std::unordered_map<const ChartClip*, ClipInstance> clipInstances;
    // BlockSchedule-only bookkeeping: which schedule.voices entry is this clip's currently-open one
    // (if any) - kept as a plain index, not a pointer, since that vector is still being push_back'd
    // to while Build() runs, so a pointer into it would be unsafe until construction finishes.
    std::unordered_map<const ChartClip*, int> voiceIndexByClip;

    // The current bar-alignment origin, as a whole number of beats since
    // t=0 - mirrors ChartSong::OriginBeat() exactly (this analytical
    // schedule's own timeline has no separate count-in offset the way
    // GameSession's does, so t=0 already IS beat 0 here - see ChartSong::
    // BeginPlaythrough's own comment for the live game's equivalent). 0 for
    // the song's very first arrangement, and reset to whatever beat t is
    // at, unconditionally, each time a Reset or Break section is reached
    // (see their own cases below) - a Learn or Background section never
    // touches it. Re-anchoring at a Break even when its own clip (or
    // another still-open voice) was already playing is always safe: the
    // alignment invariant (ChartClip's own class comment) guarantees t is
    // already one of its own loop boundaries, so this lands on the exact
    // same phase a preserved origin would have. Integral for the same
    // reason ChartSong::OriginBeat() is - the origin is only ever
    // re-anchored to an already beat-aligned instant, never fractional.
    long long arrangementOriginBeat = 0;

    // Closes every currently-open voice window - called on entering a Break
    // or Reset section (both silence everything before anything else in the
    // real game too, via AudioEngine::StopAll()/StopAllExcept() +
    // GameSession's own ClipInstance::isPlaying fill). Unconditional here
    // even for Break, unlike the real game's StopAllExcept (which spares
    // its own already-playing clip) - schedule-equivalent regardless, since
    // every VoiceWindow this section's own clip had open closes and reopens
    // at the same instant either way.
    auto stopAllVoices = [&](double atSeconds)
    {
        for (VoiceWindow& window : schedule.voices)
        {
            if (window.stopSeconds < 0.0)
            {
                window.stopSeconds = atSeconds;
            }
        }
        for (auto& entry : clipInstances)
        {
            entry.second.MarkStopped();
        }
    };

    // Mirrors GameSession::StartClipLoop's own idempotency guard - a no-op
    // if this clip already has an open voice (from a Learn/Break/
    // Background section reached earlier and never since stopped). Returns
    // the (possibly just-opened) instance either way.
    auto startVoiceIfNeeded = [&](const ChartClip* clip, double atSeconds, double volumeBeforeLockIn,
                                   double volumeAfterLockIn, double lockInSecondsForThisStart,
                                   int sectionIndex) -> ClipInstance&
    {
        ClipInstance& instance = clipInstances[clip];
        if (instance.IsPlaying())
        {
            return instance;
        }
        instance.SetContext(*clip, static_cast<double>(arrangementOriginBeat) * secondsPerBeat, stemDurationsByClip.at(clip));
        instance.MarkStarted(atSeconds);

        VoiceWindow window;
        window.sectionIndex = sectionIndex;
        window.clip = clip;
        window.startSeconds = atSeconds;
        window.stopSeconds = -1.0;
        window.volumeBeforeLockIn = volumeBeforeLockIn;
        window.volumeAfterLockIn = volumeAfterLockIn;
        window.lockInSeconds = lockInSecondsForThisStart;
        window.originSeconds = instance.OriginSeconds();
        schedule.voices.push_back(window);
        voiceIndexByClip[clip] = static_cast<int>(schedule.voices.size()) - 1;
        return instance;
    };

    for (size_t i = 0; i < song.Sections().size(); ++i)
    {
        const ChartSection& section = song.Sections()[i];
        // The one unavoidable index resolution per section: converting the
        // immutable ChartSection's own file-format reference into a real
        // pointer, used for everything from here on instead of the int.
        const ChartClip* clip =
            section.clipIndex >= 0 ? &song.Clips()[static_cast<size_t>(section.clipIndex)] : nullptr;

        // "The next section begins" is exactly this point - realize
        // whatever the previous section (if it was Background) queued,
        // mirroring GameSession::BeginSection's own top-of-function check.
        if (queuedBackgroundClip != nullptr)
        {
            startVoiceIfNeeded(queuedBackgroundClip, t, queuedBackgroundClip->Volume(), queuedBackgroundClip->Volume(),
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
                arrangementOriginBeat = std::llround(t / secondsPerBeat);
                break;

            case SectionKind::Break:
            {
                // Closes every open voice window before starting its own
                // clip - see stopAllVoices' own comment for why this is a
                // plain unconditional close here even though the real
                // game's equivalent (StopAllExcept) skips its own clip.
                stopAllVoices(t);
                arrangementOriginBeat = std::llround(t / secondsPerBeat);

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Break;
                entry.clip = clip;
                entry.sectionStartSeconds = t;
                entry.audioStartSeconds = t;
                entry.lockInSeconds = -1.0;

                // stopAllVoices() just cleared every voice, so this always
                // opens fresh (matches the real StartClipLoop always
                // actually (re)starting a break's clip, phase-seeked at t) -
                // against arrangementOriginBeat, just re-anchored to t
                // above.
                ClipInstance& instance = startVoiceIfNeeded(clip, t, clip->Volume(), clip->Volume(), -1.0,
                                                             static_cast<int>(i));
                entry.originSeconds = instance.OriginSeconds();
                entry.loopSeconds = instance.StemDurationSeconds();

                ChartClip::BreakAdvance advance = instance.ComputeBreakAdvance(t, section.loopCount, tFallSeconds);
                entry.loopCount = advance.loopCount;
                entry.endSeconds = advance.advanceSeconds;

                // Unlike Learn/Background, a break clip self-stops once its
                // own loop_count/c_NoteFallBeats-extended duration elapses
                // (GameSession's finishedSection handling calls
                // StopClipLoop for a finished Break specifically).
                int voiceIdx = voiceIndexByClip.at(clip);
                schedule.voices[static_cast<size_t>(voiceIdx)].stopSeconds = entry.endSeconds;
                instance.MarkStopped();

                t = entry.endSeconds;

                // A Break implicitly ends with a Reset - mirrors
                // GameSession's own finishedSection handling for a Break
                // exactly (see its own comment for why: without this,
                // whatever picks up next only lands on a shared loop
                // boundary if the break's own played duration happens to be
                // a whole multiple of that next clip's own length, which
                // nothing guarantees). Everything is already closed by
                // stopAllVoices() above/instance.MarkStopped() just now, so
                // there's nothing left to stop here, just the re-anchor.
                arrangementOriginBeat = std::llround(t / secondsPerBeat);

                schedule.entries.push_back(entry);
                break;
            }

            case SectionKind::Learn:
            {
                double afterBeat = t / secondsPerBeat - c_FreshJoinEpsilonBeats;

                // Read here, ahead of the anchor computation below, which
                // needs it right away - a Learn never moves the origin
                // (only Reset/Break do), so startVoiceIfNeeded's own read
                // moments later just sees this exact same value.
                ClipInstance& instance = clipInstances[clip];
                instance.SetContext(*clip, static_cast<double>(arrangementOriginBeat) * secondsPerBeat, stemDurationsByClip.at(clip));
                double originBeat = static_cast<double>(arrangementOriginBeat);

                // Covers both a clip's first-ever appearance and a later
                // section reusing one already mid-groove in one formula -
                // see ChartClip::NextOnsetAfter's own comment for why.
                double anchors[c_LaneCount];
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    anchors[lane] = instance.NextOnsetAfter(song.Bpm(), afterBeat, lane);
                }

                // Starts immediately and its own advance is scheduled right
                // away too, exactly like Break - mirrors
                // GameSession::BeginSection's Learn case exactly, which no
                // longer waits for any hits_required-th onset at all.
                double audioStartSeconds = t;
                double lockInBeat = WalkOnsetsForLockIn(originBeat, *clip, anchors, clip->HitsRequired(), afterBeat);
                double lockInSeconds = lockInBeat * secondsPerBeat;

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Learn;
                entry.clip = clip;
                entry.sectionStartSeconds = t;
                entry.audioStartSeconds = audioStartSeconds;
                entry.originSeconds = instance.OriginSeconds();
                entry.lockInSeconds = lockInSeconds;
                entry.loopSeconds = instance.StemDurationSeconds();

                // A Learn section always (re)starts its own clip fresh -
                // ValidateArrangementAlignment rejects any chart where it
                // would instead join a clip still open from an earlier,
                // un-stopped section - so startVoiceIfNeeded here always
                // actually starts a voice, never finds one already playing.
                startVoiceIfNeeded(clip, audioStartSeconds, clip->InitVolume(), clip->Volume(), lockInSeconds,
                                   static_cast<int>(i));

                entry.endSeconds = instance.ComputeLearnAdvanceSeconds(t, section.loopCount, tFallSeconds);

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
                // ChartClip::ExtendAdvanceForFallLeadTime - see its own
                // doc comment.
                entry.endSeconds = instance.ExtendAdvanceForFallLeadTime(entry.endSeconds, lockInSeconds, tFallSeconds);

                // Informational only - the total number of passes spanning
                // [audioStartSeconds, endSeconds), each exactly stemDuration
                // long (see Seek()'s own matching per-pass math). Seek()
                // independently re-derives the true loop/phase from
                // elapsedSeconds directly against the fields above, so this
                // can never desync playback even if it's slightly off at a
                // boundary.
                entry.loopCount = 1;
                if (instance.StemDurationSeconds() > 0.0)
                {
                    double span = entry.endSeconds - audioStartSeconds;
                    if (span > 1e-6)
                    {
                        entry.loopCount = static_cast<int>(std::ceil(span / instance.StemDurationSeconds() - 1e-9));
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
            // started (see ChartClip::ComputeClipPhaseSeconds/
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
