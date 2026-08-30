# Microsoft XAudio 2.9 Redistributable

`xaudio2_9redist.dll` is the redistributable build of XAudio 2.9. `AudioEngine`
(`src/AudioEngine.cpp`) loads the in-box `xaudio2_9.dll` on Windows 10/11 and
falls back to this DLL on Windows 7 / 8.1, where XAudio 2.9 is not part of the
OS. The installer ships it next to `Rhythm.exe` / `RhythmEditor.exe`.

- **Package:** [`Microsoft.XAudio2.Redist`](http://aka.ms/XAudio2Redist) (NuGet)
- **Version:** 1.2.11
- **File:** `build/native/release/bin/x64/xaudio2_9redist.dll` from that package
- **License:** `LICENSE.txt` (Microsoft XAudio 2.9 Redistributable license -
  distribution is permitted with an application that adds primary functionality)

Only the x64 Release DLL is vendored; the game has no 32-bit or debug-CRT build.
To update, download a newer package version, replace the DLL and `LICENSE.txt`,
and bump the version above.
