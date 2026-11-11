# Broadcast / recording transport status (MOD-12 / MOD-11)

Honest status as built. "Implemented" means code exists and passes the listed tests; it does NOT mean qualified against live receivers or hardware.

| Item | Status | Verified by |
|---|---|---|
| RTMP (rtmp://, port 1935) | Implemented, portable client `src/broadcast/rtmp.{h,cpp}` over `IByteStream`. **Unqualified against live receivers.** Visibly unencrypted. | `tests/test_cast_rtmp.cpp` (in-process mock server with an independent chunk/AMF parser) |
| RTMPS (rtmps://, port 443) | Implemented as RTMP over a verifying TLS `IByteStream` (Windows: Schannel, `win::makeTcpStream`). **Unqualified against live receivers; TLS path not run against a real server in this environment** (wine's Schannel only proves "fails on non-TLS peer"). Refused (`CertificateRequired`, terminal) when the stream cannot verify chain + host name; never downgraded, no override. Revocation is not checked. | factory/transport/session tests; `tests/win/smoke_platform.cpp` under wine |
| RTP/RTCP (MJPEG + L16) | Implemented, LAN contribution, unencrypted and labelled so; Windows UDP adapter `win::makeBroadcastUdpSocket`. | `tests/test_cast_rtp.cpp` |
| SRT | **Absent.** `TransportFactory` returns `UnsupportedCapability`: "no SRT stack; requires qualified media worker". Only a registered media worker (`registerMediaWorker`) can enable it. Not claimed anywhere. | `test_cast_session`, `test_cast_rtmp` |

## RTMP client scope
Adobe RTMP 1.0 simple handshake (C0/C1/C2; S2 echo mismatch is tolerated but recorded), chunk types 0-3, extended timestamps, Set Chunk Size (both directions), Window Ack Size / Acknowledgement / Set Peer Bandwidth, Ping request/response, AMF0 (no AMF3 objects), command flow `connect` -> `releaseStream` -> `FCPublish` -> `createStream` -> `publish(key,'live')` with `_result` / `_error` / `onStatus` checks, `@setDataFrame onMetaData`, FLV-style AVC (sequence header + length-prefixed NALU) and AAC (AudioSpecificConfig + raw) messages. Timestamps come from packet PTS (relative to the first packet, never backwards). Not supported: RTMPE, complex/digest handshake, AMF3 command bodies, enhanced RTMP (HEVC/AV1), server-side listen.

Never blocks callers: `send()` copies into a bounded queue (256 packets / 4 MiB); when full the oldest non-keyframe packet is dropped (headers and video key frames are kept) and counted. One worker thread owns the stream; `close()` aborts a blocked connect/read within one poll slice (Stop within 1 s).

Failure classes: certificate, rejected credentials (`Rejected`/`Denied`/`Unauthorized`...), invalid app -> terminal (no retry). `NetStream.Publish.BadName` (name in use after a drop) and everything else -> retryable under the session policy (1/2/4/8/15 s, 120 s).

Secrets: the stream key is read from `ISecretStore` by `BroadcastSession` at connect time, held only inside the transport worker (commands `releaseStream`, `FCPublish`, `publish`), wiped when the attempt ends, and scrubbed from every string the transport reports (including receiver text that echoes it). `tcUrl` never contains it. Test: secret-leak scans over logs/status/errors.

Packet conventions: `Video/H264` = Annex B access unit (SPS/PPS/AUD stripped on the wire), `Audio/Aac` = raw AAC-LC, `Headers` = avcC / AudioSpecificConfig or `rtmp::packHeaders()` bundle. MJPEG and PCM packets are counted as unsupported and not sent over RTMP.

## Encoders and storage (Windows Media Foundation, loaded at run time)
* `h264-mf` (`win::registerWindowsEncoders`): Microsoft software H.264 MFT, baseline, CBR, 2 s GOP, low latency, RGB32->NV12 in scalar code. `qualified=false`. Registered only when the MFT can be created, otherwise the registry reports `EncoderUnavailable`.
* `win::makeAacEncoder()` (`IAudioEncoder`): Microsoft AAC-LC MFT, raw payload, AudioSpecificConfig computed from rate/channels; bit rate snaps to 96/128/160/192 kb/s.
* **Neither MFT exists under wine; the MF encode paths have not been executed anywhere in this environment.** Only the pure helpers are tested. Codec patent/redistribution decisions remain open (spec §12/§16).
* MKV can store H.264 (`V_MPEG4/ISO/AVC`, avcC CodecPrivate from `IVideoEncoder::codecPrivate()`, Annex B converted to length-prefixed blocks); AVI refuses H.264. Tested with a fake encoder (`tests/test_rec_h264.cpp`) and the independent re-parser.

## Other Windows adapters
`win::makeDiskInfo()` (GetDiskFreeSpaceExW), `win::makeUsbSerialLink()` (overlapped COM port, 250000 baud 8N2, `COMn` names only) and `win::namedUsbAdapters()` (protocol implemented, **not hardware-qualified**; the qualified list stays empty). Serial I/O has not been tried on hardware.
