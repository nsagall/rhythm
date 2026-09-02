#pragma once

#include <string>
#include <vector>

#include "ChartClip.h"  // ChartClip stored by value in m_clips.

// Which of the four block kinds a section is.
enum class SectionKind
{
    Learn,      // [learn]: judge presses/releases against the clip.
    Break,      // [break]: stop everything else, play this clip, block until loop_count loops finish.
    Reset,      // [reset]: stop everything else looping, then advance immediately (no clip, no pause).
    Background, // [background]: queue this clip to start (without stopping anything) when the *next* section begins.
};

// One step of gameplay, processed in declared order. clipIndex indexes ChartSong::Clips(), or -1
// for a Reset section (which has no clip).
struct ChartSection
{
    int clipIndex = -1;
    SectionKind kind = SectionKind::Learn;

    // Minimum number of times the clip must loop; exact meaning is SectionKind-specific.
    int loopCount = 1;
};

// A full song: tempo/time signature, the pool of reusable clips, and the ordered list of sections
// that drives gameplay.
class ChartSong
{
public:
    const std::wstring& Title() const { return m_title; }
    double Bpm() const { return m_bpm; }
    int BeatsPerBar() const { return m_beatsPerBar; } // The time_signature numerator (N in "N/D").
    int TimeSignatureDenominator() const { return m_timeSignatureDenominator; } // The time_signature denominator (D in "N/D").
    // Default press/release judging tolerance for a clip that declares no
    // start_tolerance_ms/release_tolerance_ms override of its own.
    double StartToleranceMs() const { return m_startToleranceMs; }
    double ReleaseToleranceMs() const { return m_releaseToleranceMs; }
    const std::vector<ChartClip>& Clips() const { return m_clips; }
    const std::vector<ChartSection>& Sections() const { return m_sections; }
    std::vector<ChartClip>& MutableClips() { return m_clips; }

    // Parses and validates a .chart text file into *this.
    //   chartFilePath - path to the .chart text file.
    //   outErrors     - on failure, one human-readable message per problem found (unsupported
    //                   fields, wrong-typed or out-of-range values, malformed time signature,
    //                   missing required fields, a missing/unparsable stem or MIDI file, a
    //                   duplicate clip name, a section referencing an unknown clip, an unrecognized
    //                   block header, or no [learn]/[break]/[reset]/[background] blocks at all).
    // Returns false if the file can't be opened or fails validation, leaving *this exactly as it
    // was before the call. On success outErrors is empty and *this holds the parsed/validated song.
    bool Load(const std::wstring& chartFilePath, std::vector<std::wstring>& outErrors);

    // ---------------------------------------------------------------
    // Per-playthrough timing state - not chart content, and untouched by Load(). (Re)set by
    // BeginPlaythrough().
    // ---------------------------------------------------------------

    // (Re)anchors a playthrough.
    //   wallClockSecondsAtBeatZero - becomes this playthrough's own beat 0; the bar-alignment
    //                                origin resets to that same beat.
    void BeginPlaythrough(double wallClockSecondsAtBeatZero)
    {
        m_songStartSeconds = wallClockSecondsAtBeatZero;
        m_originBeat = 0;
    }

    // The current bar-alignment origin, in whole beats since this playthrough's beat 0 (negative
    // before it, e.g. during the count-in). Re-anchored to the current beat by each Reset or Break
    // section; a Learn or Background section leaves it alone.
    long long OriginBeat() const { return m_originBeat; }
    void SetOriginBeat(long long beat) { m_originBeat = beat; }

    // This playthrough's beat<->seconds conversions, measured from BeginPlaythrough's
    // wallClockSecondsAtBeatZero.
    double SecondsPerBeat() const { return 60.0 / m_bpm; }
    double AbsoluteBeatToSeconds(double absoluteBeat) const
    {
        return m_songStartSeconds + absoluteBeat * SecondsPerBeat();
    }
    double SecondsToAbsoluteBeat(double wallClockSeconds) const
    {
        return (wallClockSeconds - m_songStartSeconds) / SecondsPerBeat();
    }

    // OriginBeat() converted to a wall-clock second.
    double OriginSeconds() const { return AbsoluteBeatToSeconds(static_cast<double>(m_originBeat)); }

private:
    std::wstring m_title;
    double m_bpm = 120.0;
    int m_beatsPerBar = 4;
    int m_timeSignatureDenominator = 4;
    double m_startToleranceMs = 120.0;
    double m_releaseToleranceMs = 120.0;
    std::vector<ChartClip> m_clips;
    std::vector<ChartSection> m_sections;

    // Per-playthrough timing state; see above.
    double m_songStartSeconds = 0.0;
    long long m_originBeat = 0;
};
