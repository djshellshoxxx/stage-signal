# Recorder and recovery
- Choose a folder in Setup, then Start recording. The state must read Recording before you rely on it.
- Audio-only (WAV) needs no video encoder. Video formats use the built-in MJPEG encoder.
- Low disk space warns early and stops gracefully, leaving completed segments playable. Lost intervals are reported.
- After a crash, StageSignal asks to restore the autosaved show as a new copy. The original is never overwritten and everything starts disarmed.
- Recordings can be repaired from their journal if the program stopped abruptly.
