#include "BlockSchedule.h"

#include <algorithm>
#include <cmath>

#include "LaneConfig.h"

namespace BlockSchedule
{

namespace
{

// One lane's next not-yet-consumed onset, for the merge walk below. Only lanes with at least one
// note participate.
struct LaneFrontier
{
    int lane = 0;
    double beat = 0.0;
};

// Walks the chronologically-merged onsets across every lane with notes for exactly hitsRequired
// pops, mirroring GameSession::RegisterHit's shared streak.
//   originBeat   - the clip's arrangement origin, in beats.
//   clip         - the section's clip.
//   anchors      - each lane's first onset to walk from.
//   hitsRequired - the clip's hits_required.
//   afterBeat    - fallback returned if no lane has notes.
// Returns the beat of the hitsRequired-th pop for a perfect player (the instant IsLockedIn() flips
// true; only drives the voice's volume-switch timing).
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
    // Which schedule.voices entry is this clip's currently-open one. A plain index, not a pointer,
    // since schedule.voices is still being push_back'd to while Build() runs.
    std::unordered_map<const ChartClip*, int> voiceIndexByClip;

    // The current bar-alignment origin, in whole beats since t=0 - the editor's equivalent of
    // ChartSong::OriginBeat() (this timeline has no count-in offset, so t=0 is beat 0). Re-anchored
    // to the current beat unconditionally on every Reset or Break; a Learn or Background never
    // touches it. Integral for the same reason ChartSong::OriginBeat() is.
    long long arrangementOriginBeat = 0;

    // Closes every currently-open voice window - called on entering a Break or Reset (both silence
    // everything in the real game too). Unconditional even for Break, unlike the game's
    // StopAllExcept, but schedule-equivalent since the clip's voice closes and reopens at the same
    // instant either way.
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

    // Mirrors GameSession::StartClipLoop's idempotency guard - a no-op if this clip already has an
    // open voice. Returns the (possibly just-opened) instance either way.
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
        const ChartClip* clip =
            section.clipIndex >= 0 ? &song.Clips()[static_cast<size_t>(section.clipIndex)] : nullptr;

        // Realize whatever the previous section queued, if it was Background - mirrors
        // GameSession::BeginSection.
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
                // Closes every open voice window before starting its own clip (see stopAllVoices).
                stopAllVoices(t);
                arrangementOriginBeat = std::llround(t / secondsPerBeat);

                Entry entry;
                entry.sectionIndex = static_cast<int>(i);
                entry.kind = SectionKind::Break;
                entry.clip = clip;
                entry.sectionStartSeconds = t;
                entry.audioStartSeconds = t;
                entry.lockInSeconds = -1.0;

                // stopAllVoices() just cleared every voice, so this always opens fresh against the
                // just-re-anchored arrangementOriginBeat.
                ClipInstance& instance = startVoiceIfNeeded(clip, t, clip->Volume(), clip->Volume(), -1.0,
                                                             static_cast<int>(i));
                entry.originSeconds = instance.OriginSeconds();
                entry.loopSeconds = instance.StemDurationSeconds();

                ChartClip::BreakAdvance advance = instance.ComputeBreakAdvance(t, section.loopCount, tFallSeconds);
                entry.loopCount = advance.loopCount;
                entry.endSeconds = advance.advanceSeconds;

                // Unlike Learn/Background, a break clip self-stops once its
                // loop_count/c_NoteFallBeats-extended duration elapses.
                int voiceIdx = voiceIndexByClip.at(clip);
                schedule.voices[static_cast<size_t>(voiceIdx)].stopSeconds = entry.endSeconds;
                instance.MarkStopped();

                t = entry.endSeconds;

                // A Break implicitly ends with a Reset - mirrors GameSession's finishedSection
                // handling. Everything is already closed, so this is just the re-anchor.
                arrangementOriginBeat = std::llround(t / secondsPerBeat);

                schedule.entries.push_back(entry);
                break;
            }

            case SectionKind::Learn:
            {
                double afterBeat = t / secondsPerBeat - c_FreshJoinEpsilonBeats;

                // A Learn never moves the origin, so startVoiceIfNeeded's read later sees this same value.
                ClipInstance& instance = clipInstances[clip];
                instance.SetContext(*clip, static_cast<double>(arrangementOriginBeat) * secondsPerBeat, stemDurationsByClip.at(clip));
                double originBeat = static_cast<double>(arrangementOriginBeat);

                // One formula covers both a clip's first appearance and a later reuse mid-groove
                // (see ChartClip::NextOnsetAfter).
                double anchors[c_LaneCount];
                for (int lane = 0; lane < c_LaneCount; ++lane)
                {
                    anchors[lane] = instance.NextOnsetAfter(song.Bpm(), afterBeat, lane);
                }

                // Starts and schedules its advance immediately, like Break - mirrors
                // GameSession::BeginSection's Learn case.
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

                // A Learn always restarts its clip fresh (ValidateArrangementAlignment enforces
                // this), so startVoiceIfNeeded here always actually starts a voice.
                startVoiceIfNeeded(clip, audioStartSeconds, clip->InitVolume(), clip->Volume(), lockInSeconds,
                                   static_cast<int>(i));

                entry.endSeconds = instance.ComputeLearnAdvanceSeconds(t, section.loopCount, tFallSeconds);

                // Mirrors GameSession::Update's finishedSection handling: the boundary must land at
                // least tFallSeconds after lockInSeconds, so the section repeats by whole loops
                // until a perfect player's walk falls within it AND the next clip's preview gets a
                // full tFallSeconds to show. Shared with GameSession::RegisterHit via
                // ChartClip::ExtendAdvanceForFallLeadTime.
                entry.endSeconds = instance.ExtendAdvanceForFallLeadTime(entry.endSeconds, lockInSeconds, tFallSeconds);

                // Informational only - Seek() re-derives the true loop/phase from elapsedSeconds
                // directly, so a slight error here can't desync playback.
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

    // Any Background still queued here was never realized (acceptable, not an error) and is simply
    // never added to schedule.voices. Any realized voice still open is genuinely still playing at
    // the song's end - left as-is per VoiceWindow's meaning of stopSeconds == -1.
    schedule.totalSeconds = t;
    return schedule;
}

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
            // Pass 2 onward spans exactly loopSeconds; pass 1 is the exception (see
            // SeekResult::loopIndex). The real voice was phase-seeked when it started, so pass 1
            // ends loopSeconds - startPhase later, not a full loopSeconds later. Rescaling below
            // keeps pass 1's rendered sweep running edge-to-edge instead of jumping to the block's
            // end early.
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
    // entryIndex stays -1 if elapsedSeconds is at/after schedule.totalSeconds or there are no entries.

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
