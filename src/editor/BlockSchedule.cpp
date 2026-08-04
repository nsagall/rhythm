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

} // namespace

Schedule Build(const ChartSong& song, const std::vector<double>& stemDurationsByClipIndex)
{
    Schedule schedule;
    if (song.bpm <= 0.0)
    {
        return schedule;
    }

    double secondsPerBeat = 60.0 / song.bpm;
    double tFallSeconds = kNoteFallBeats * secondsPerBeat;

    double t = 0.0;
    int queuedBackgroundClipIndex = -1;
    int queuedBackgroundSectionIndex = -1;

    // Per-clip, mirroring GameSession's own ClipVoice::originEstablished/
    // originSeconds exactly - NOT a single whole-schedule flag. A clip's
    // first-ever start establishes clipOriginSecondsByClipIndex to that
    // exact instant, its own persistent phase reference from then on (see
    // FreshOnsetForAllLanes) - every later reuse of that same clip anchors
    // independently per lane instead, continuing its groove from wherever
    // ITS OWN beat grid (not any other clip's, and not absolute beat 0)
    // says it'd be. See GameSession.h's own comment on ClipVoice for why
    // this must be keyed per clip.
    std::vector<bool> clipOriginEstablished(song.clips.size(), false);
    std::vector<double> clipOriginSecondsByClipIndex(song.clips.size(), 0.0);

    // Mirrors GameSession::EnsureClipOriginEstablished exactly - see its own
    // comment. Returns true only when this call just established it.
    auto ensureOriginEstablished = [&](int clipIndex, double atSeconds)
    {
        if (clipOriginEstablished[static_cast<size_t>(clipIndex)])
        {
            return false;
        }
        clipOriginEstablished[static_cast<size_t>(clipIndex)] = true;
        clipOriginSecondsByClipIndex[static_cast<size_t>(clipIndex)] = atSeconds;
        return true;
    };

    // Per-clip playback state, mirroring GameSession's own ClipVoice::
    // isPlaying/loopStartSeconds exactly - a clip reused by a later
    // section while still open from an earlier one
    // (nothing has stopped it since) does NOT restart or re-seek; it just
    // keeps going, and any lock-in-floor math that section computes uses
    // the ORIGINAL start time, not its own. -1 means "not currently open".
    std::vector<int> voiceIndexByClipIndex(song.clips.size(), -1);
    std::vector<double> loopStartSecondsByClipIndex(song.clips.size(), -1.0);

    // Mirrors AudioEngine::StopAll() + GameSession's ClipVoice::isPlaying
    // fill - called on entering a Break or Reset section (both call StopAll()
    // before anything else, silencing every currently-open voice
    // regardless of how it started).
    auto stopAllVoices = [&](double atSeconds)
    {
        for (VoiceWindow& window : schedule.voices)
        {
            if (window.stopSeconds < 0.0)
            {
                window.stopSeconds = atSeconds;
            }
        }
        std::fill(voiceIndexByClipIndex.begin(), voiceIndexByClipIndex.end(), -1);
        std::fill(loopStartSecondsByClipIndex.begin(), loopStartSecondsByClipIndex.end(), -1.0);
    };

    // Mirrors GameSession::StartClipLoop's own idempotency guard - a no-op
    // if this clip already has an open voice (from a Learn/Break/
    // Background section reached earlier and never since stopped). Also
    // establishes this clip's origin if it hasn't been already - mirrors
    // StartClipLoop's own EnsureClipOriginEstablished safety-net call, so a
    // Background voice (which never goes through Learn/Break's own explicit
    // pre-establishment) still gets one. Returns true only when a new
    // VoiceWindow was actually opened.
    auto startVoiceIfNeeded = [&](int clipIndex, double atSeconds, double volumeBeforeLockIn,
                                   double volumeAfterLockIn, double lockInSecondsForThisStart, int sectionIndex)
    {
        if (voiceIndexByClipIndex[static_cast<size_t>(clipIndex)] >= 0)
        {
            return false;
        }
        ensureOriginEstablished(clipIndex, atSeconds);
        VoiceWindow window;
        window.sectionIndex = sectionIndex;
        window.clipIndex = clipIndex;
        window.startSeconds = atSeconds;
        window.stopSeconds = -1.0;
        window.volumeBeforeLockIn = volumeBeforeLockIn;
        window.volumeAfterLockIn = volumeAfterLockIn;
        window.lockInSeconds = lockInSecondsForThisStart;
        window.originSeconds = clipOriginSecondsByClipIndex[static_cast<size_t>(clipIndex)];
        schedule.voices.push_back(window);
        voiceIndexByClipIndex[static_cast<size_t>(clipIndex)] = static_cast<int>(schedule.voices.size()) - 1;
        loopStartSecondsByClipIndex[static_cast<size_t>(clipIndex)] = atSeconds;
        return true;
    };

    for (size_t i = 0; i < song.sections.size(); ++i)
    {
        const ChartSection& section = song.sections[i];

        // "The next section begins" is exactly this point - realize
        // whatever the previous section (if it was Background) queued,
        // mirroring GameSession::BeginSection's own top-of-function check.
        if (queuedBackgroundClipIndex >= 0)
        {
            const ChartClip& bgClip = song.clips[static_cast<size_t>(queuedBackgroundClipIndex)];
            startVoiceIfNeeded(queuedBackgroundClipIndex, t, bgClip.volume, bgClip.volume, -1.0,
                                queuedBackgroundSectionIndex);
            queuedBackgroundClipIndex = -1;
        }

        switch (section.kind)
        {
            case SectionKind::Background:
                queuedBackgroundClipIndex = section.clipIndex;
                queuedBackgroundSectionIndex = static_cast<int>(i);
                break;

            case SectionKind::Reset:
                stopAllVoices(t);
                break;

            case SectionKind::Break:
            {
                // Mirrors GameSession::BeginSection's Break case calling
                // m_audioEngine.StopAll() before starting its own clip -
                // that silences any currently-playing voice, background or
                // locked-in Learn alike, not just a Reset's own gate.
                stopAllVoices(t);

                const ChartClip& clip = song.clips[static_cast<size_t>(section.clipIndex)];
                double stemDuration = stemDurationsByClipIndex[static_cast<size_t>(section.clipIndex)];

                // Established here, ahead of ComputeBreakAdvance below,
                // which needs it right away - startVoiceIfNeeded's own
                // establishment (moments later) is then just a no-op
                // confirming the same value.
                ensureOriginEstablished(section.clipIndex, t);
                double originSeconds = clipOriginSecondsByClipIndex[static_cast<size_t>(section.clipIndex)];

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Break;
                entry.clipIndex = section.clipIndex;
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
                startVoiceIfNeeded(section.clipIndex, t, clip.volume, clip.volume, -1.0, static_cast<int>(i));
                // Unlike Learn/Background, a break clip self-stops once its
                // own loop_count/kNoteFallBeats-extended duration elapses
                // (GameSession's finishedSection handling calls
                // StopClipLoop for a finished Break specifically).
                int voiceIdx = voiceIndexByClipIndex[static_cast<size_t>(section.clipIndex)];
                schedule.voices[static_cast<size_t>(voiceIdx)].stopSeconds = entry.endSeconds;
                voiceIndexByClipIndex[static_cast<size_t>(section.clipIndex)] = -1;

                t = entry.endSeconds;
                schedule.entries.push_back(entry);
                break;
            }

            case SectionKind::Learn:
            {
                const ChartClip& clip = song.clips[static_cast<size_t>(section.clipIndex)];
                double stemDuration = stemDurationsByClipIndex[static_cast<size_t>(section.clipIndex)];
                double afterBeat = t / secondsPerBeat - 1e-6;

                // Established here, ahead of the anchor computation below,
                // which needs it right away - startVoiceIfNeeded's own
                // establishment (moments later) is then just a no-op
                // confirming the same value.
                bool freshOrigin = ensureOriginEstablished(section.clipIndex, t);
                double originSeconds = clipOriginSecondsByClipIndex[static_cast<size_t>(section.clipIndex)];
                double originBeat = originSeconds / secondsPerBeat;

                double anchors[kLaneCount];
                if (freshOrigin)
                {
                    ChartTiming::FreshOnsetForAllLanes(originBeat, clip, anchors);
                }
                else
                {
                    for (int lane = 0; lane < kLaneCount; ++lane)
                    {
                        anchors[lane] = ChartTiming::NextOnsetAfter(originBeat, afterBeat, clip, lane);
                    }
                }

                // Starts immediately and its own advance is scheduled right
                // away too, exactly like Break - mirrors
                // GameSession::BeginSection's Learn case exactly, which no
                // longer waits for any hits_required-th onset at all.
                double audioStartSeconds = t;
                double lockInBeat = WalkOnsetsForLockIn(originBeat, clip, anchors, clip.hitsRequired, afterBeat);
                double lockInSeconds = lockInBeat * secondsPerBeat;

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Learn;
                entry.clipIndex = section.clipIndex;
                entry.sectionStartSeconds = t;
                entry.audioStartSeconds = audioStartSeconds;
                entry.originSeconds = originSeconds;
                entry.lockInSeconds = lockInSeconds;
                entry.loopSeconds = stemDuration;

                // If this clip is already playing (still open from an
                // earlier, un-stopped section), startVoiceIfNeeded is a
                // no-op and loopStartSecondsByClipIndex keeps its
                // ORIGINAL value - exactly mirroring how the real
                // StartClipLoop's guard leaves ClipVoice::loopStartSeconds
                // untouched in that case, so this section's own advance
                // floor is computed relative to when the clip truly
                // started, not this section's own start.
                startVoiceIfNeeded(section.clipIndex, audioStartSeconds, clip.initVolume, clip.volume, lockInSeconds,
                                   static_cast<int>(i));
                double effectiveLoopStartSeconds = loopStartSecondsByClipIndex[static_cast<size_t>(section.clipIndex)];

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
                if (stemDuration > 0.0 && lockInSeconds > entry.endSeconds)
                {
                    double loopsNeeded = std::ceil((lockInSeconds - entry.endSeconds) / stemDuration - 1e-9);
                    entry.endSeconds += std::max(0.0, loopsNeeded) * stemDuration;
                }

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
            double startPhase = std::fmod(entry.audioStartSeconds - entry.originSeconds, entry.loopSeconds);
            double firstPassSeconds = entry.loopSeconds - startPhase;
            if (firstPassSeconds <= 1e-9)
            {
                firstPassSeconds = entry.loopSeconds;
            }

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
            result.activeVoices.push_back({window.clipIndex, volume, window.originSeconds});
        }
    }

    return result;
}

} // namespace BlockSchedule
