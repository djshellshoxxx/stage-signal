# Audio gain staging
StageSignal never changes hardware gain. Set it on the audio interface or mixer.

- Aim for peaks below -6 dBFS. The dotted line on the meter marks -6 dBFS.
- The raw input meter and the processed (recording/broadcast) meter are separate. An upstream clip warns you even when the limiter hides it on the output.
- [NEAR FULL SCALE] means a peak within 0.5 dB of full scale. [CLIPPING] means consecutive full-scale samples: lower the gain.
- If the device disappears the top bar shows DISCONNECTED within a second. StageSignal never switches to another microphone by itself; choose the device and press Open, or use Reopen same device.
