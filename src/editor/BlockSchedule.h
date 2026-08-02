#pragma once

#include <vector>

#include "ChartFile.h"

// An analytical, seekable schedule of what a chart's blocks would do if a
// player hit every note correctly - built once from a fully-resolved
// ChartSong (see Build()'s contract below), then queried at any
// elapsed-seconds position via Seek(), which is exact and cheap regardless
// of direction: seeking backwards to an arbitrary point is exactly as
// correct as playing forward to it, since Seek() is a pure function of the
// precomputed Schedule and the requested position. All timing math here
// delegates to ChartTiming (src/ChartTiming.h), the same functions
// GameSession itself uses for live play, so the two can never compute
// different answers for the same chart.
namespace BlockSchedule
{

// One playable judging entry - Learn or Break sections only. Background/
// Reset never get their own entry (mirrors how neither ever persists as
// "current" in the live game - see GameSession::NextPersistentSectionAtOrAfter)
// - they contribute zero elapsed time between the entries around them.
// This drives the timeline's block widths and playhead position; it is
// NOT the same thing as "what audio is playing" - see VoiceWindow for that
// (a locked-in Learn clip keeps sounding long after its own Entry ends, to
// build up the arrangement, exactly like the live game).
struct Entry
{
    int sectionIndex = 0; // index into ChartSong::sections
    SectionKind kind = SectionKind::Learn;
    int clipIndex = -1; // index into ChartSong::clips

    double sectionStartSeconds = 0.0; // == previous entry's endSeconds (0 for the first)
    // Break: == sectionStartSeconds (starts immediately, no press gate).
    // Learn: the first note onset across every lane with notes (mirrors
    // GameSession::StartClipLoop only acting on a Learn section's first
    // hit) - this is when THIS section's own judging conceptually begins,
    // even on the rare chart where the same clip was already playing from
    // an earlier, still-open Learn section (see Build()'s comment on
    // clipLoopStartSeconds) - it does not necessarily mean the underlying
    // audio voice re-seeks its phase at this instant.
    double audioStartSeconds = 0.0;
    // Learn only: the hits_required-th onset in chronological order across
    // all lanes (== GameSession::SchedulePendingAdvance's nowSeconds for a
    // perfect player). -1 for Break (no lock-in event - loop_count is
    // already known up front).
    double lockInSeconds = -1.0;
    double loopSeconds = 0.0; // one stem loop's duration - the timeline "block width" unit
    int loopCount = 1;        // informational: total passes spanning [audioStartSeconds, endSeconds) - see Build()
    double endSeconds = 0.0;  // == next entry's sectionStartSeconds
};

// One clip's actual audible window - covers a Learn clip (once locked in
// via its first onset), a Break clip, and a Background clip alike, since
// all three behave identically once actually sounding: they keep playing
// until a later Break/Reset's StopAll() (Break additionally self-stops
// after its own loop_count/kNoteFallBeats-extended duration; Learn and
// Background never self-stop - "build up the arrangement" is the live
// game's own phrase for this). This is the source of truth for what audio
// should actually be playing at any instant - Entry above is a separate,
// judging/timeline-only concept.
struct VoiceWindow
{
    int sectionIndex = 0; // which section started this voice
    int clipIndex = -1;
    double startSeconds = 0.0;
    double stopSeconds = -1.0; // -1 = still playing at the end of the schedule
    // Volume before/from lockInSeconds - equal to each other (both ==
    // clip.volume) and lockInSeconds == -1 for Break/Background (no split
    // point; always volumeAfterLockIn, which just happens to equal
    // volumeBeforeLockIn too). For Learn: volumeBeforeLockIn == initVolume,
    // volumeAfterLockIn == volume, split at this window's own lockInSeconds
    // (the matching Entry's lockInSeconds, when this window was opened
    // fresh rather than continuing an already-open one).
    double volumeBeforeLockIn = 1.0;
    double volumeAfterLockIn = 1.0;
    double lockInSeconds = -1.0;
};

struct Schedule
{
    std::vector<Entry> entries;
    std::vector<VoiceWindow> voices;
    double totalSeconds = 0.0;
};

// Builds a schedule for song, assuming a perfect player (hits every note
// exactly on time - mathematically exact for that assumption, not an
// approximation, since a shared hit-streak advancing exactly once per note
// in chronological order means "lock-in happens at the hits_required-th
// onset" is a precise restatement of the real rule). song's clips must
// already carry real, AudioEngine-measured stem durations in
// stemDurationsByClipIndex (parallel to song.clips) and must already be
// ChartTiming::ExpandLaneNotesToFillClip'd using those durations - Build()
// does no audio I/O and no expansion itself, only the timing arithmetic
// (see src/editor/BlockPlayer.cpp for how a fully-resolved song is
// obtained).
Schedule Build(const ChartSong& song, const std::vector<double>& stemDurationsByClipIndex);

struct ActiveVoice
{
    int clipIndex = -1;
    double volume = 1.0;
};

struct SeekResult
{
    int entryIndex = -1; // -1 if elapsedSeconds is before/after every entry, or the schedule is empty
    // 1-based pass number within the entry; 0 if entryIndex >= 0 but
    // elapsedSeconds is still in the anchor-to-first-press gap (before
    // audioStartSeconds) - notes are anchored but nothing is audible yet.
    // Pass 2 onward each span exactly loopSeconds of real elapsed time -
    // this is a timeline/playhead-only notion of "which pass" and
    // deliberately does NOT track the underlying audio voice's actual
    // absolute-time-aligned sample phase (see
    // BlockPlayer::ApplyAudioForPosition, which computes that separately
    // and correctly starts a clip's audio mid-loop if it begins partway
    // through the song - the "beat grid" phase-alignment a real player
    // would hear). Pass 1 is the one exception, and deliberately so: its
    // REAL duration is loopSeconds - fmod(audioStartSeconds, loopSeconds)
    // - shorter than a full loopSeconds whenever audioStartSeconds doesn't
    // land exactly on a loop boundary (the real, phase-seeked audio voice
    // genuinely wraps that much sooner) - but phaseSeconds is still
    // reported rescaled into the full 0..loopSeconds range so pass 1's
    // rendered sweep still runs edge-to-edge across the block's full
    // width, just compressed into that shorter real time span, rather
    // than stopping partway across it. Getting this wrong (assuming pass 1
    // also spans a full loopSeconds) was a confirmed bug: a block would
    // visually stop advancing at some fraction short of its own right edge
    // and jump straight to the next block instead, since the section's
    // real end can land before a naively-assumed full-loopSeconds pass 1
    // would have finished.
    int loopIndex = 0;
    double phaseSeconds = 0.0; // rescaled into 0..entries[entryIndex].loopSeconds - see loopIndex's own comment
    // Every clip that should currently be audible, each with its current
    // (post- or pre-lock-in) volume - not just background layers, per
    // VoiceWindow's own comment.
    std::vector<ActiveVoice> activeVoices;
};

// Pure function of schedule and elapsedSeconds - see the namespace comment.
SeekResult Seek(const Schedule& schedule, double elapsedSeconds);

} // namespace BlockSchedule
