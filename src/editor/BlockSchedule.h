#pragma once

#include <unordered_map>
#include <vector>

#include "ChartSong.h"

// An analytical, seekable schedule of what a chart's blocks would do if a
// player hit every note correctly - built once from a fully-resolved
// ChartSong (see Build()'s contract below), then queried at any
// elapsed-seconds position via Seek(), which is exact and cheap regardless
// of direction: seeking backwards to an arbitrary point is exactly as
// correct as playing forward to it, since Seek() is a pure function of the
// precomputed Schedule and the requested position. All timing math here
// delegates to ChartClip's own timing methods (src/ChartClip.h), the same
// functions GameSession itself uses for live play, so the two can never compute
// different answers for the same chart.
//
// Every ChartClip* below (Entry::clip, VoiceWindow::clip, ActiveVoice::clip)
// points directly into the song passed to Build() - see Build()'s own
// comment for the lifetime contract that makes this safe.
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
    const ChartClip* clip = nullptr; // points into the ChartSong passed to Build()

    double sectionStartSeconds = 0.0; // == previous entry's endSeconds (0 for the first)
    // Learn and Break alike: == sectionStartSeconds. Both start their clip
    // immediately when the section begins - no press gate, no lock-in wait
    // - even on the rare chart where the same clip was already playing
    // from an earlier, still-open section (see Build()'s own
    // ClipBuildState::loopStartSeconds comment), where it does not
    // necessarily mean the underlying audio voice re-seeks its phase at
    // this instant.
    double audioStartSeconds = 0.0;
    // The current arrangement's shared phase reference - the wall-clock
    // second its first clip began at, whether that was this entry or an
    // earlier one (see Build()'s own arrangementOriginSeconds) - not
    // necessarily == audioStartSeconds. Every phase/loop-boundary
    // computation for this entry's clip is measured relative to this, not
    // absolute second 0, so Seek() can correctly reproduce where the real, phase-seeked audio
    // voice actually is.
    double originSeconds = 0.0;
    // Learn only: the hits_required-th onset in chronological order across
    // all lanes, for a perfect player - i.e. the instant IsLockedIn() would
    // flip true (see GameSession::RegisterHit). Always reachable (and
    // therefore always a real, non-negative value) for a perfect player:
    // if hits_required exceeds however many onsets a single loop of the
    // pattern offers, entry.endSeconds itself just extends by as many
    // further loops as it takes (see Build()'s own Learn case) - a learn
    // clip never gets abandoned before locking in, live game or here
    // alike. Drives nothing about entry.endSeconds itself beyond that
    // extension, only the matching VoiceWindow's own volume-switch timing.
    // Always -1 for Break (no lock-in event - loop_count is already known
    // up front).
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
    const ChartClip* clip = nullptr; // points into the ChartSong passed to Build()
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
    // The arrangement's shared phase reference - see Entry::originSeconds
    // for what this is and why it's not necessarily == startSeconds.
    double originSeconds = 0.0;
};

// Build()'s full result for one song: every judging Entry and every
// VoiceWindow, in chronological order, plus the song's total duration -
// everything Seek() needs to answer "what's true at second X" for any X.
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
// stemDurationsByClip (keyed by each clip's own address in song.clips) and
// must already be ChartClip::ExpandLaneNotesToFillClip'd using those
// durations - Build() does no audio I/O and no expansion itself, only the
// timing arithmetic (see src/editor/BlockPlayer.cpp for how a
// fully-resolved song is obtained).
//
// Lifetime contract: every pointer in the returned Schedule (Entry::clip,
// VoiceWindow::clip, and every ActiveVoice::clip a later Seek() call
// produces) points directly into song.clips. song must outlive the
// Schedule, and must not be reassigned or resized while the Schedule is
// still in use - callers that rebuild their ChartSong (e.g. after an edit)
// must rebuild the Schedule from it at the same time, before anything reads
// the old one again. See BlockPlayer::RebuildSchedule for how that's kept
// true in practice (song and its Schedule are sibling members, replaced
// together in one function, never independently).
Schedule Build(const ChartSong& song, const std::unordered_map<const ChartClip*, double>& stemDurationsByClip);

// One clip that should currently be audible, as reported by SeekResult -
// clip plus everything BlockPlayer::ApplyAudioForPosition needs to
// actually reproduce that voice (volume, phase).
struct ActiveVoice
{
    const ChartClip* clip = nullptr; // points into the ChartSong passed to Build()
    double volume = 1.0;
    // The arrangement's shared phase reference - see
    // VoiceWindow::originSeconds - needed by BlockPlayer::ApplyAudioForPosition
    // to phase-seek a newly-active voice exactly like the real game would.
    double originSeconds = 0.0;
};

// Seek()'s answer to "what's true at this elapsed-seconds position": which
// entry/pass/phase the timeline/playhead should show, and every clip that
// should actually be audible right then.
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

// Pass 1's real duration for a Learn/Break entry, given where its clip's
// audio actually started (audioStartSeconds), its persistent phase
// reference (originSeconds), and one full loop's duration (loopSeconds) -
// see SeekResult::loopIndex's own comment for why pass 1 is a special case.
// Returns 0.0 (never loopSeconds itself, never dividing/fmod-ing by it)
// when loopSeconds <= 0 - "nothing to compute," matching Seek()'s own
// top-level treatment of that case (loopIndex == 0, phaseSeconds == 0.0).
// Shared by Seek() and BlockTimeline::LayoutXToSeconds's own ruler-click
// mapping, which needs the identical formula to convert a click position
// back into elapsed seconds - previously two independent copies of the
// same 6 lines, one of which (LayoutXToSeconds) lacked this function's
// loopSeconds<=0 guard and would collapse every click across a degenerate
// block's width to audioStartSeconds instead of tracking the click.
double ComputeFirstPassSeconds(double audioStartSeconds, double originSeconds, double loopSeconds);

// The instant a zero-duration (Background/Reset) section actually falls
// at - such a section never gets its own Entry (see Entry's own comment),
// so there's nothing to look up directly for it - the first Learn/Break
// entry at or after sectionIndex (entries are always in section order), or
// schedule.totalSeconds if sectionIndex is trailing past every entry.
// Shared by BlockPlayer::SeekToBlockStart's own fallback and
// BlockTimeline::LayoutXToSeconds's Background/Reset marker-click
// fallback - both need the same answer for the same block.
double FirstEntrySecondsAtOrAfter(const Schedule& schedule, int sectionIndex);

} // namespace BlockSchedule
