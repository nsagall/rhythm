#pragma once

#include <unordered_map>
#include <vector>

#include "ChartSong.h"  // SectionKind enum used by value in Entry.

// An analytical, seekable schedule of what a chart's blocks would do if a player hit every note
// correctly. Built once from a fully-resolved ChartSong via Build(), then queried at any
// elapsed-seconds position via Seek(), which is a pure function of the precomputed Schedule and the
// position - so seeking backwards is exactly as correct as playing forward. All timing math
// delegates to ChartClip's timing methods, the same ones GameSession uses for live play, so the
// two can never disagree about the same chart.
//
// Every ChartClip* below points directly into the song passed to Build(); see Build()'s lifetime
// contract.
namespace BlockSchedule
{

// One playable judging entry - Learn or Break sections only. Background/Reset never get an entry
// (they contribute zero elapsed time). This drives the timeline's block widths and playhead, which
// is not the same as "what audio is playing" (a locked-in Learn clip keeps sounding after its
// Entry ends) - see VoiceWindow.
struct Entry
{
    // Index into ChartSong::sections.
    int sectionIndex = 0;

    SectionKind kind = SectionKind::Learn;

    // Points into the ChartSong passed to Build().
    const ChartClip* clip = nullptr;

    // == previous entry's endSeconds (0 for the first).
    double sectionStartSeconds = 0.0;

    // == sectionStartSeconds for Learn and Break alike (both restart their clip fresh the instant
    // the section begins).
    double audioStartSeconds = 0.0;

    // The current arrangement's shared phase reference - the wall-clock second its first clip began
    // at, whether this entry or an earlier one. Not necessarily == audioStartSeconds; every
    // phase/loop-boundary computation for this clip is measured relative to it.
    double originSeconds = 0.0;

    // Learn only: the instant IsLockedIn() would flip true for a perfect player (the
    // hits_required-th onset across all lanes). Always a real non-negative value. -1 for Break.
    double lockInSeconds = -1.0;

    // One stem loop's duration - the timeline "block width" unit.
    double loopSeconds = 0.0;

    // Informational: total passes spanning [audioStartSeconds, endSeconds).
    int loopCount = 1;

    // == next entry's sectionStartSeconds.
    double endSeconds = 0.0;
};

// One clip's actual audible window - covers a locked-in Learn clip, a Break clip, and a Background
// clip alike, since all three keep playing once sounding until a later Break/Reset's StopAll()
// (Break also self-stops after its loop_count/c_NoteFallBeats-extended duration). This is the
// source of truth for what audio should be playing at any instant; Entry is judging/timeline-only.
struct VoiceWindow
{
    // Which section started this voice.
    int sectionIndex = 0;

    // Points into the ChartSong passed to Build().
    const ChartClip* clip = nullptr;

    double startSeconds = 0.0;

    // -1 = still playing at the end of the schedule.
    double stopSeconds = -1.0;

    // Learn: volumeBeforeLockIn == initVolume, volumeAfterLockIn == volume, split at lockInSeconds.
    // Break/Background: both equal clip.volume and lockInSeconds == -1 (no split).
    double volumeBeforeLockIn = 1.0;
    double volumeAfterLockIn = 1.0;
    double lockInSeconds = -1.0;

    // Shared phase reference - see Entry::originSeconds.
    double originSeconds = 0.0;
};

// Build()'s full result for one song: every judging Entry and every VoiceWindow in chronological
// order, plus the song's total duration - everything Seek() needs.
struct Schedule
{
    std::vector<Entry> entries;
    std::vector<VoiceWindow> voices;
    double totalSeconds = 0.0;
};

// Builds a schedule for song, assuming a perfect player (mathematically exact for that assumption,
// not an approximation).
//   song                - a fully-resolved chart. Its clips must already carry real stem durations
//                         and be ExpandLaneNotesToFillClip'd; Build() does no audio I/O or expansion.
//   stemDurationsByClip - each clip's measured stem duration, keyed by the clip's address in song.clips.
// Lifetime contract: every pointer in the returned Schedule (and in any ActiveVoice a later Seek()
// produces) points into song.clips. song must outlive the Schedule and must not be reassigned or
// resized while it's in use - rebuild both together (see BlockPlayer::RebuildSchedule).
Schedule Build(const ChartSong& song, const std::unordered_map<const ChartClip*, double>& stemDurationsByClip);

// One clip that should currently be audible, as reported by SeekResult - everything
// BlockPlayer::ApplyAudioForPosition needs to reproduce the voice.
struct ActiveVoice
{
    // Points into the ChartSong passed to Build().
    const ChartClip* clip = nullptr;

    double volume = 1.0;

    // Shared phase reference - see VoiceWindow::originSeconds.
    double originSeconds = 0.0;
};

// Seek()'s answer to "what's true at this elapsed-seconds position".
struct SeekResult
{
    // -1 if elapsedSeconds is before/after every entry, or the schedule is empty.
    int entryIndex = -1;

    // 1-based pass number within the entry; 0 if elapsedSeconds is still in the anchor-to-first-press
    // gap (notes anchored, nothing audible yet). Pass 2 onward each span exactly loopSeconds of
    // elapsed time - a timeline/playhead notion of "which pass", NOT the audio voice's absolute
    // sample phase (BlockPlayer::ApplyAudioForPosition computes that separately). Pass 1 is the
    // exception: its real duration is loopSeconds - fmod(audioStartSeconds, loopSeconds), shorter
    // whenever audioStartSeconds isn't on a loop boundary, but phaseSeconds is still rescaled into
    // the full 0..loopSeconds range so pass 1's rendered sweep runs edge-to-edge.
    int loopIndex = 0;

    // Rescaled into 0..entries[entryIndex].loopSeconds - see loopIndex.
    double phaseSeconds = 0.0;

    // Every clip that should currently be audible, with its current volume.
    std::vector<ActiveVoice> activeVoices;
};

// Pure function of schedule and elapsedSeconds - see the namespace comment.
SeekResult Seek(const Schedule& schedule, double elapsedSeconds);

// Returns pass 1's real duration for a Learn/Break entry - see SeekResult::loopIndex for why pass 1
// is a special case.
//   audioStartSeconds - where the clip's audio actually started.
//   originSeconds     - the clip's persistent phase reference.
//   loopSeconds       - one full loop's duration.
// Returns 0.0 when loopSeconds <= 0. Shared by Seek() and BlockTimeline::LayoutXToSeconds so a
// ruler click maps back to elapsed seconds via the identical formula.
double ComputeFirstPassSeconds(double audioStartSeconds, double originSeconds, double loopSeconds);

// Returns the instant a zero-duration (Background/Reset) section falls at: the first Learn/Break
// entry at or after sectionIndex, or schedule.totalSeconds if sectionIndex is past every entry.
// Shared by BlockPlayer::SeekToBlockStart and BlockTimeline::LayoutXToSeconds.
double FirstEntrySecondsAtOrAfter(const Schedule& schedule, int sectionIndex);

} // namespace BlockSchedule
