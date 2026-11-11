# Broadcast protocols and credentials
- RTMPS: encrypted, certificate and host name are verified. There is no option to skip verification.
- RTMP: unencrypted; use only on trusted networks.
- RTP: unencrypted contribution (MJPEG + L16) for a local network; public destinations need explicit opt-in.
- SRT: not available in this build (no SRT stack is bundled). The Record / Broadcast tab shows the reason.
- H.264 uses the Windows Media Foundation encoder when present and is marked not qualified until verified on this machine.
The stream key is stored only in the Windows secret store (DPAPI), is shown masked, and never appears in logs, exports or support bundles.
Broadcasting starts only when you press Start and confirm the summary. Network failures reconnect for up to two minutes; Stop cancels reconnecting at once. Local recording continues.
