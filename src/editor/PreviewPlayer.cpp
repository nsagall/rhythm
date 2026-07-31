#include "PreviewPlayer.h"

namespace
{

const wchar_t* SectionKindDisplayName(SectionKind kind)
{
    switch (kind)
    {
        case SectionKind::Learn:
            return L"Learn";
        case SectionKind::Break:
            return L"Break";
        case SectionKind::Reset:
            return L"Reset";
        case SectionKind::Background:
            return L"Background";
    }
    return L"Learn";
}

} // namespace

PreviewPlayer::PreviewPlayer(AudioEngine& audioEngine) : m_audioEngine(audioEngine)
{
}

bool PreviewPlayer::PrepareForDocument(const EditorDocument& doc, std::wstring& outError)
{
    if (m_state != State::Stopped)
    {
        outError = L"Cannot reload clips while the preview is playing or paused - stop it first.";
        return false;
    }

    m_doc = &doc;
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
        m_stems.push_back({clip.id, handle});
    }
    return true;
}

void PreviewPlayer::Play()
{
    if (m_doc == nullptr || m_doc->sections.empty() || m_state == State::Playing)
    {
        return;
    }

    m_state = State::Playing;
    BeginSection(m_currentSectionIndex < 0 ? 0 : m_currentSectionIndex);
}

void PreviewPlayer::Pause()
{
    if (m_state != State::Playing)
    {
        return;
    }
    m_audioEngine.StopAll();
    m_waitingStem = StemHandle{};
    m_activeBackgroundClipIds.clear();
    m_state = State::Paused;
}

void PreviewPlayer::Stop()
{
    m_audioEngine.StopAll();
    m_waitingStem = StemHandle{};
    m_activeBackgroundClipIds.clear();
    m_queuedBackgroundClipId = -1;
    m_currentSectionIndex = -1;
    m_currentClipId = -1;
    m_state = State::Stopped;
}

void PreviewPlayer::SeekToSection(int sectionIndex)
{
    if (m_doc == nullptr || sectionIndex < 0 || static_cast<size_t>(sectionIndex) >= m_doc->sections.size())
    {
        return;
    }
    if (m_state == State::Stopped)
    {
        return;
    }

    m_queuedBackgroundClipId = -1;
    m_state = State::Playing;
    BeginSection(sectionIndex);
}

void PreviewPlayer::SetLoopWholeSong(bool loop)
{
    m_loopWholeSong = loop;
}

bool PreviewPlayer::LoopWholeSong() const
{
    return m_loopWholeSong;
}

void PreviewPlayer::Update()
{
    if (m_state != State::Playing || !m_waitingStem.IsValid())
    {
        return;
    }
    if (!m_audioEngine.IsPlaying(m_waitingStem))
    {
        int next = m_currentSectionIndex + 1;
        m_waitingStem = StemHandle{};
        BeginSection(next);
    }
}

bool PreviewPlayer::IsPlaying() const
{
    return m_state == State::Playing;
}

bool PreviewPlayer::IsPaused() const
{
    return m_state == State::Paused;
}

int PreviewPlayer::CurrentSectionIndex() const
{
    return m_currentSectionIndex;
}

int PreviewPlayer::CurrentLoopNumber() const
{
    if (!m_waitingStem.IsValid())
    {
        return 0;
    }
    double duration = m_audioEngine.GetStemDurationSeconds(m_waitingStem);
    if (duration <= 0.0)
    {
        return 1;
    }
    double position = m_audioEngine.GetPositionSeconds(m_waitingStem);
    int loopNumber = static_cast<int>(position / duration) + 1;
    if (loopNumber > m_currentSectionLoopCount)
    {
        loopNumber = m_currentSectionLoopCount;
    }
    if (loopNumber < 1)
    {
        loopNumber = 1;
    }
    return loopNumber;
}

int PreviewPlayer::TotalLoopsForCurrentSection() const
{
    return m_waitingStem.IsValid() ? m_currentSectionLoopCount : 0;
}

std::wstring PreviewPlayer::NowPlayingText() const
{
    if (m_doc == nullptr || m_currentSectionIndex < 0 ||
        static_cast<size_t>(m_currentSectionIndex) >= m_doc->sections.size())
    {
        return m_state == State::Paused ? L"Paused" : L"Stopped";
    }

    const EditorSection& section = m_doc->sections[static_cast<size_t>(m_currentSectionIndex)];
    std::wstring text = std::wstring(SectionKindDisplayName(section.kind)) + L" (#" +
                         std::to_wstring(m_currentSectionIndex + 1) + L" of " +
                         std::to_wstring(m_doc->sections.size()) + L")";

    const EditorClip* clip = FindClipById(*m_doc, m_currentClipId);
    if (clip != nullptr && m_waitingStem.IsValid())
    {
        text = clip->displayName + L", " + text + L", loop " + std::to_wstring(CurrentLoopNumber()) + L"/" +
               std::to_wstring(m_currentSectionLoopCount);
    }
    else
    {
        text += L" (no clip)";
    }

    if (!m_activeBackgroundClipIds.empty())
    {
        text += L" | background:";
        for (int clipId : m_activeBackgroundClipIds)
        {
            const EditorClip* bgClip = FindClipById(*m_doc, clipId);
            if (bgClip != nullptr)
            {
                text += L" " + bgClip->displayName;
            }
        }
    }

    return text;
}

void PreviewPlayer::BeginSection(int sectionIndex)
{
    // Realize any clip queued by a previous Background section - "the next
    // section begins" is exactly this call.
    if (m_queuedBackgroundClipId >= 0)
    {
        const EditorClip* bgClip = FindClipById(*m_doc, m_queuedBackgroundClipId);
        StemHandle bgStem = GetStemForClip(m_queuedBackgroundClipId);
        if (bgClip != nullptr && bgStem.IsValid())
        {
            // loop_count has no effect on Background sections, matching
            // GameSession's own documented behavior - always loops forever
            // until a later Break/Reset's StopAll() silences it.
            m_audioEngine.StartLooping(bgStem, 0.0, static_cast<float>(bgClip->volume), 0);
            m_activeBackgroundClipIds.push_back(m_queuedBackgroundClipId);
        }
        m_queuedBackgroundClipId = -1;
    }

    if (m_doc == nullptr || sectionIndex < 0 || static_cast<size_t>(sectionIndex) >= m_doc->sections.size())
    {
        if (m_loopWholeSong && m_doc != nullptr && !m_doc->sections.empty())
        {
            BeginSection(0);
            return;
        }
        m_audioEngine.StopAll();
        m_activeBackgroundClipIds.clear();
        m_currentSectionIndex = -1;
        m_currentClipId = -1;
        m_waitingStem = StemHandle{};
        m_state = State::Stopped;
        return;
    }

    m_currentSectionIndex = sectionIndex;
    const EditorSection& section = m_doc->sections[static_cast<size_t>(sectionIndex)];

    switch (section.kind)
    {
        case SectionKind::Learn:
        case SectionKind::Break:
        {
            m_audioEngine.StopAll();
            m_activeBackgroundClipIds.clear();
            const EditorClip* clip = FindClipById(*m_doc, section.clipId);
            StemHandle stem = (clip != nullptr) ? GetStemForClip(clip->id) : StemHandle{};
            if (clip != nullptr && stem.IsValid())
            {
                m_audioEngine.StartLooping(stem, 0.0, static_cast<float>(clip->volume), section.loopCount);
                m_currentClipId = clip->id;
                m_currentSectionLoopCount = section.loopCount;
                m_waitingStem = stem;
            }
            else
            {
                // No clip picked yet, or its .wav isn't imported - nothing
                // to play; don't get stuck waiting forever, just move on.
                m_currentClipId = -1;
                m_waitingStem = StemHandle{};
                BeginSection(sectionIndex + 1);
            }
            break;
        }
        case SectionKind::Reset:
            m_audioEngine.StopAll();
            m_activeBackgroundClipIds.clear();
            m_currentClipId = -1;
            m_waitingStem = StemHandle{};
            BeginSection(sectionIndex + 1);
            break;
        case SectionKind::Background:
            m_queuedBackgroundClipId = section.clipId;
            BeginSection(sectionIndex + 1);
            break;
    }
}

StemHandle PreviewPlayer::GetStemForClip(int clipId) const
{
    for (const ClipStem& entry : m_stems)
    {
        if (entry.clipId == clipId)
        {
            return entry.handle;
        }
    }
    return StemHandle{};
}
