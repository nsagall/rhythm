#include "BlockPlayer.h"

#include <algorithm>
#include <cmath>

#include "ChartTiming.h"
#include "EditorChartIO.h"

BlockPlayer::BlockPlayer(AudioEngine& audioEngine) : m_audioEngine(audioEngine)
{
}

bool BlockPlayer::PrepareStems(const EditorDocument& doc, std::wstring& outError)
{
    if (m_state != State::Stopped)
    {
        outError = L"Cannot reload clips while the preview is playing or paused - stop it first.";
        return false;
    }

    m_stems.clear();
    for (const EditorClip& clip : doc.clips)
    {
        if (!clip.hasWav)
        {
            continue;
        }
        std::wstring wavPath = doc.folderPath + clip.name + L".wav";
        StemHandle handle = m_audioEngine.LoadStem(wavPath);
        if (!handle.IsValid())
        {
            outError = L"Could not load " + wavPath;
            return false;
        }
        ClipStem stem;
        stem.editorClipId = clip.id;
        stem.handle = handle;
        stem.durationSeconds = m_audioEngine.GetStemDurationSeconds(handle);
        m_stems.push_back(stem);
    }
    return true;
}

StemHandle BlockPlayer::GetStemForEditorClipId(int editorClipId) const
{
    for (const ClipStem& stem : m_stems)
    {
        if (stem.editorClipId == editorClipId)
        {
            return stem.handle;
        }
    }
    return StemHandle{};
}

bool BlockPlayer::EnsureStemsLoaded(const EditorDocument& doc, std::wstring& outError)
{
    for (const EditorClip& clip : doc.clips)
    {
        if (!clip.hasWav || GetStemForEditorClipId(clip.id).IsValid())
        {
            continue;
        }
        std::wstring wavPath = doc.folderPath + clip.name + L".wav";
        StemHandle handle = m_audioEngine.LoadStem(wavPath);
        if (!handle.IsValid())
        {
            outError = L"Could not load " + wavPath;
            return false;
        }
        ClipStem stem;
        stem.editorClipId = clip.id;
        stem.handle = handle;
        stem.durationSeconds = m_audioEngine.GetStemDurationSeconds(handle);
        m_stems.push_back(stem);
    }
    return true;
}

bool BlockPlayer::RebuildSchedule(const EditorDocument& doc, std::vector<std::wstring>& outErrors)
{
    std::wstring stemError;
    if (!EnsureStemsLoaded(doc, stemError))
    {
        outErrors.clear();
        outErrors.push_back(stemError);
        return false;
    }

    ChartSong song;
    if (!EditorChartIO::ValidateDocument(doc, outErrors, &song))
    {
        return false;
    }

    std::vector<double> stemDurations(song.clips.size(), 0.0);
    std::vector<StemHandle> stemHandles(song.clips.size());
    for (size_t i = 0; i < song.clips.size() && i < doc.clips.size(); ++i)
    {
        StemHandle handle = GetStemForEditorClipId(doc.clips[i].id);
        stemHandles[i] = handle;
        double duration = handle.IsValid() ? m_audioEngine.GetStemDurationSeconds(handle) : 0.0;
        stemDurations[i] = duration;

        ChartClip& clip = song.clips[i];
        if (clip.hasMidi)
        {
            // Mirrors GameSession::LoadChart's own rejection check - a
            // chart that fails this would refuse to even load in the real
            // game, so the editor shouldn't produce a schedule for it
            // either.
            if (!ChartTiming::ClipFitsOneLoop(clip, duration, song.bpm))
            {
                outErrors.clear();
                outErrors.push_back(L"clip '" + clip.name +
                                     L"': its MIDI pattern is longer than one loop of its audio - trim the "
                                     L"MIDI pattern or use a longer audio stem (the real game would refuse to "
                                     L"load this chart).");
                return false;
            }
            ChartTiming::ExpandLaneNotesToFillClip(clip, duration, song.bpm);
        }
    }

    m_song = std::move(song);
    m_stemHandlesBySongClipIndex = std::move(stemHandles);
    m_schedule = BlockSchedule::Build(m_song, stemDurations);
    return true;
}

const BlockSchedule::Schedule* BlockPlayer::CurrentSchedule() const
{
    return &m_schedule;
}

void BlockPlayer::Play()
{
    if (m_schedule.entries.empty())
    {
        return;
    }
    m_state = State::Playing;
    ApplyAudioForPosition();
}

void BlockPlayer::Pause()
{
    if (m_state != State::Playing)
    {
        return;
    }
    m_audioEngine.StopAll();
    m_activeVoiceClipIndices.clear();
    m_state = State::Paused;
}

void BlockPlayer::Stop()
{
    m_audioEngine.StopAll();
    m_activeVoiceClipIndices.clear();
    m_positionSeconds = 0.0;
    m_state = State::Stopped;
}

void BlockPlayer::SeekToSeconds(double seconds)
{
    if (seconds < 0.0)
    {
        seconds = 0.0;
    }
    m_positionSeconds = seconds;
    if (m_state == State::Playing)
    {
        ApplyAudioForPosition();
    }
}

void BlockPlayer::SeekToBlockStart(int sectionIndex)
{
    for (const BlockSchedule::Entry& entry : m_schedule.entries)
    {
        if (entry.sectionIndex == sectionIndex)
        {
            SeekToSeconds(entry.sectionStartSeconds);
            return;
        }
    }

    // No entry for this exact section - Background/Reset never get one
    // (both are zero-duration, see BlockSchedule::Entry's own comment).
    // Fall back to wherever the next real (Learn/Break) entry actually
    // begins, which is exactly this section's own conceptual instant in
    // time - entries are already sorted by sectionIndex, so the first one
    // at or past sectionIndex is the answer.
    for (const BlockSchedule::Entry& entry : m_schedule.entries)
    {
        if (entry.sectionIndex >= sectionIndex)
        {
            SeekToSeconds(entry.sectionStartSeconds);
            return;
        }
    }

    // Nothing at or after this section either - it's trailing, past the
    // last real entry - land at the very end.
    SeekToSeconds(m_schedule.totalSeconds);
}

void BlockPlayer::SetLoopWholeSong(bool loop)
{
    m_loopWholeSong = loop;
}

bool BlockPlayer::LoopWholeSong() const
{
    return m_loopWholeSong;
}

void BlockPlayer::Update(double deltaSeconds)
{
    if (m_state != State::Playing)
    {
        return;
    }

    m_positionSeconds += deltaSeconds;
    if (m_positionSeconds >= m_schedule.totalSeconds)
    {
        if (m_loopWholeSong && m_schedule.totalSeconds > 0.0)
        {
            m_positionSeconds = std::fmod(m_positionSeconds, m_schedule.totalSeconds);
        }
        else
        {
            Stop();
            return;
        }
    }

    ApplyAudioForPosition();
}

void BlockPlayer::ApplyAudioForPosition()
{
    BlockSchedule::SeekResult result = BlockSchedule::Seek(m_schedule, m_positionSeconds);

    // Stop voices that are no longer supposed to be active.
    for (int clipIndex : m_activeVoiceClipIndices)
    {
        bool stillActive = std::any_of(result.activeVoices.begin(), result.activeVoices.end(),
                                        [clipIndex](const BlockSchedule::ActiveVoice& v)
                                        { return v.clipIndex == clipIndex; });
        if (!stillActive)
        {
            m_audioEngine.Stop(m_stemHandlesBySongClipIndex[static_cast<size_t>(clipIndex)]);
        }
    }

    // Start newly-active voices, phase-aligned to the current position -
    // mirrors StartClipLoop's own absolute-wall-clock phase seek exactly
    // (phase = fmod(position, stemDuration), independent of when this
    // particular voice window happened to open). Already-active voices
    // just get their volume kept in sync, for a Learn voice's own
    // init_volume -> volume switch at its lock-in instant.
    std::vector<int> newActive;
    newActive.reserve(result.activeVoices.size());
    for (const BlockSchedule::ActiveVoice& voice : result.activeVoices)
    {
        newActive.push_back(voice.clipIndex);
        bool wasActive = std::find(m_activeVoiceClipIndices.begin(), m_activeVoiceClipIndices.end(),
                                    voice.clipIndex) != m_activeVoiceClipIndices.end();
        StemHandle stem = m_stemHandlesBySongClipIndex[static_cast<size_t>(voice.clipIndex)];
        if (!wasActive)
        {
            double stemDuration = m_audioEngine.GetStemDurationSeconds(stem);
            const ChartClip& clip = m_song.clips[static_cast<size_t>(voice.clipIndex)];
            // ChartTiming::ComputeClipPhaseSeconds, matching GameSession's
            // own real-game phase-seek exactly - see its doc comment for
            // why this must use the clip's beat-based pattern length
            // (spanBeats), not the audio file's own raw measured duration.
            // voice.originSeconds (not 0) is this clip's own persistent
            // phase reference - see BlockSchedule::VoiceWindow::originSeconds.
            double phase = ChartTiming::ComputeClipPhaseSeconds(voice.originSeconds, m_positionSeconds, clip,
                                                                  stemDuration, m_song.bpm);
            m_audioEngine.StartLooping(stem, phase, static_cast<float>(voice.volume), 0);
        }
        else
        {
            m_audioEngine.SetVolume(stem, static_cast<float>(voice.volume));
        }
    }

    m_activeVoiceClipIndices = std::move(newActive);
}

bool BlockPlayer::IsPlaying() const
{
    return m_state == State::Playing;
}

bool BlockPlayer::IsPaused() const
{
    return m_state == State::Paused;
}

double BlockPlayer::PositionSeconds() const
{
    return m_positionSeconds;
}

std::wstring BlockPlayer::NowPlayingText() const
{
    BlockSchedule::SeekResult result = BlockSchedule::Seek(m_schedule, m_positionSeconds);
    if (result.entryIndex < 0 || static_cast<size_t>(result.entryIndex) >= m_schedule.entries.size())
    {
        return m_state == State::Paused ? L"Paused" : L"Stopped";
    }

    const BlockSchedule::Entry& entry = m_schedule.entries[static_cast<size_t>(result.entryIndex)];
    const wchar_t* kindName = entry.kind == SectionKind::Break ? L"Break" : L"Learn";
    const ChartClip& clip = m_song.clips[static_cast<size_t>(entry.clipIndex)];

    std::wstring text = clip.displayName + L", " + kindName + L" (#" + std::to_wstring(result.entryIndex + 1) +
                         L" of " + std::to_wstring(m_schedule.entries.size()) + L")";
    if (result.loopIndex >= 1)
    {
        text += L", loop " + std::to_wstring(result.loopIndex) + L"/" + std::to_wstring(entry.loopCount);
    }

    if (result.activeVoices.size() > 1)
    {
        text += L" | also playing:";
        for (const BlockSchedule::ActiveVoice& voice : result.activeVoices)
        {
            if (voice.clipIndex != entry.clipIndex)
            {
                text += L" " + m_song.clips[static_cast<size_t>(voice.clipIndex)].displayName;
            }
        }
    }

    return text;
}
