#include "LaneBindings.h"

#include <string>

#include "Settings.h"

namespace
{

// Serializes binding as ""/"K:<code>"/"M:<code>"/"G:<code>" - see
// LaneBindings.h's own comment on the persisted format. Kept private to this
// file; Settings itself never needs to know this shape, only that it
// round-trips.
std::wstring SerializeBinding(const InputBinding& binding)
{
    if (binding.kind == InputKind::Keyboard)
    {
        return L"K:" + std::to_wstring(binding.code);
    }
    if (binding.kind == InputKind::MidiNote)
    {
        return L"M:" + std::to_wstring(binding.code);
    }
    if (binding.kind == InputKind::Gamepad)
    {
        return L"G:" + std::to_wstring(binding.code);
    }
    return L"";
}

// Inverse of SerializeBinding - a string that doesn't match either prefix
// (including empty, or anything corrupted) parses as InputKind::None, same
// as "never saved" - there's no invalid-but-distinguishable state to report.
InputBinding DeserializeBinding(const std::wstring& serialized)
{
    if (serialized.size() > 2 && serialized[1] == L':')
    {
        try
        {
            int code = std::stoi(serialized.substr(2));
            if (serialized[0] == L'K')
            {
                return InputBinding{InputKind::Keyboard, code};
            }
            if (serialized[0] == L'M')
            {
                return InputBinding{InputKind::MidiNote, code};
            }
            if (serialized[0] == L'G')
            {
                return InputBinding{InputKind::Gamepad, code};
            }
        }
        catch (const std::exception&)
        {
            // A hand-edited or corrupted settings.ini falls back to "no
            // custom binding" rather than crashing on startup.
        }
    }
    return InputBinding{};
}

} // namespace

// Loads every lane's saved custom binding from settings.
void LaneBindings::Load(Settings& settings)
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        m_custom[lane] = DeserializeBinding(settings.LoadLaneBinding(lane));
    }
}

// Whichever lane has vkCode as its default, or as its custom Keyboard binding.
int LaneBindings::LaneForKey(int vkCode) const
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (kLaneDefaultKeys[lane] == vkCode)
        {
            return lane;
        }
    }
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (m_custom[lane].kind == InputKind::Keyboard && m_custom[lane].code == vkCode)
        {
            return lane;
        }
    }
    return -1;
}

// Whichever lane has note as its custom MidiNote binding.
int LaneBindings::LaneForMidiNote(int note) const
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (m_custom[lane].kind == InputKind::MidiNote && m_custom[lane].code == note)
        {
            return lane;
        }
    }
    return -1;
}

// Whichever lane has button as its custom Gamepad binding.
int LaneBindings::LaneForGamepadButton(int button) const
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (m_custom[lane].kind == InputKind::Gamepad && m_custom[lane].code == button)
        {
            return lane;
        }
    }
    return -1;
}

bool LaneBindings::IsDefaultKey(int vkCode) const
{
    for (int lane = 0; lane < kLaneCount; ++lane)
    {
        if (kLaneDefaultKeys[lane] == vkCode)
        {
            return true;
        }
    }
    return false;
}

InputBinding LaneBindings::GetCustom(int lane) const
{
    return m_custom[lane];
}

// Clears binding's (kind, code) from any other lane that already has it as
// its own custom binding (so no physical input maps to two lanes at once),
// then sets and persists it on lane.
void LaneBindings::SetCustom(int lane, InputBinding binding, Settings& settings)
{
    for (int other = 0; other < kLaneCount; ++other)
    {
        if (other != lane && m_custom[other].kind == binding.kind && m_custom[other].code == binding.code)
        {
            m_custom[other] = InputBinding{};
            SaveLane(other, settings);
        }
    }

    m_custom[lane] = binding;
    SaveLane(lane, settings);
}

void LaneBindings::SaveLane(int lane, Settings& settings) const
{
    settings.SaveLaneBinding(lane, SerializeBinding(m_custom[lane]));
}
