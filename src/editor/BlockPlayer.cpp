#include "BlockPlayer.h"

#include <algorithm>
#include <cmath>

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

    // Validate and expand on the local song first (not m_song), matching doc.clips by position, so
    // a failure below leaves m_song/m_schedule untouched. These position-indexed vectors hand off
    // to the pointer-keyed maps below once m_song.clips has its final addresses.
    std::vector<double> durationsByPosition(song.Clips().size(), 0.0);
    std::vector<StemHandle> handlesByPosition(song.Clips().size());
    // Captured before ExpandLaneNotesToFillClip can widen any clip's spanBeats - see
    // GameSession::LoadChart.
    std::unordered_map<const ChartClip*, ChartClip::ClipAlignmentInfo> clipAlignmentInfo;
    for (size_t i = 0; i < song.Clips().size() && i < doc.clips.size(); ++i)
    {
        StemHandle handle = GetStemForEditorClipId(doc.clips[i].id);
        handlesByPosition[i] = handle;
        double duration = handle.IsValid() ? m_audioEngine.GetStemDurationSeconds(handle) : 0.0;
        durationsByPosition[i] = duration;

        ChartClip& clip = song.MutableClips()[i];
        clipAlignmentInfo[&clip] = {clip.SpanBeats(), duration};
        if (clip.HasMidi())
        {
            // Mirrors GameSession::LoadChart's rejection check - a chart failing this wouldn't
            // load in the real game.
            if (!clip.ClipFitsOneLoop(duration, song.Bpm()))
            {
                outErrors.clear();
                outErrors.push_back(L"clip '" + clip.Name() +
                                     L"': its MIDI pattern is longer than one loop of its audio - trim the "
                                     L"MIDI pattern or use a longer audio stem (the real game would refuse to "
                                     L"load this chart).");
                return false;
            }
            clip.ExpandLaneNotesToFillClip(duration, song.Bpm());
        }
    }

    // The same whole-chart bar-alignment invariant GameSession::LoadChart checks.
    if (!ChartClip::ValidateArrangementAlignment(song, clipAlignmentInfo, outErrors))
    {
        return false;
    }

    m_song = std::move(song);

    // Build the pointer-keyed maps everything else uses, from m_song.clips' final addresses.
    m_stemHandlesByClip.clear();
    std::unordered_map<const ChartClip*, double> stemDurationsByClip;
    for (size_t i = 0; i < m_song.Clips().size() && i < handlesByPosition.size(); ++i)
    {
        const ChartClip* clip = &m_song.Clips()[i];
        m_stemHandlesByClip[clip] = handlesByPosition[i];
        stemDurationsByClip[clip] = durationsByPosition[i];
    }

    m_schedule = BlockSchedule::Build(m_song, stemDurationsByClip);
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
    m_activeVoiceClips.clear();
    m_state = State::Paused;
}

void BlockPlayer::Stop()
{
    m_audioEngine.StopAll();
    m_activeVoiceClips.clear();
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

    // No entry for this section (Background/Reset are zero-duration) - fall back to where the next
    // real entry begins, this section's conceptual instant. Shared with
    // BlockTimeline::LayoutXToSeconds's marker-click fallback.
    SeekToSeconds(BlockSchedule::FirstEntrySecondsAtOrAfter(m_schedule, sectionIndex));
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
    for (const ChartClip* clip : m_activeVoiceClips)
    {
        bool stillActive = std::any_of(result.activeVoices.begin(), result.activeVoices.end(),
                                        [clip](const BlockSchedule::ActiveVoice& v) { return v.clip == clip; });
        if (!stillActive)
        {
            m_audioEngine.Stop(m_stemHandlesByClip.at(clip));
        }
    }

    // Start newly-active voices phase-aligned to the current position (mirrors StartClipLoop);
    // already-active voices just get their volume kept in sync, for a Learn voice's
    // init_volume -> volume switch at lock-in.
    std::vector<const ChartClip*> newActive;
    newActive.reserve(result.activeVoices.size());
    for (const BlockSchedule::ActiveVoice& voice : result.activeVoices)
    {
        newActive.push_back(voice.clip);
        bool wasActive =
            std::find(m_activeVoiceClips.begin(), m_activeVoiceClips.end(), voice.clip) != m_activeVoiceClips.end();
        StemHandle stem = m_stemHandlesByClip.at(voice.clip);
        if (!wasActive)
        {
            double stemDuration = m_audioEngine.GetStemDurationSeconds(stem);
            // ComputeClipPhaseSeconds, matching GameSession's phase-seek. voice.originSeconds (not
            // 0) is this clip's persistent phase reference.
            double phase = voice.clip->ComputeClipPhaseSeconds(voice.originSeconds, m_positionSeconds, stemDuration,
                                                                m_song.Bpm());
            m_audioEngine.StartLooping(stem, phase, static_cast<float>(voice.volume), 0);
        }
        else
        {
            m_audioEngine.SetVolume(stem, static_cast<float>(voice.volume));
        }
    }

    m_activeVoiceClips = std::move(newActive);
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
    const ChartClip& clip = *entry.clip;

    std::wstring text = clip.Name() + L", " + kindName + L" (#" + std::to_wstring(result.entryIndex + 1) +
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
            if (voice.clip != entry.clip)
            {
                text += L" " + voice.clip->Name();
            }
        }
    }

    return text;
}
