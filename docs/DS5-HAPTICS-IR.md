# DualSense authored IR: v2 base with optional enhancement

The protocol library owns capability advertisement, message dispatch, decoding,
validation and synchronous callbacks. The SDK owns PCM analysis. Sunshine owns
controller association, encoding and sending. Clients own playback clocks,
deduplication, expiry, calibration and actuator allocation.

## Compatibility and negotiation

The original v2 capability `ML_FF_DS5_HAPTICS_IR_V2 = 0x08`, control message
`0x550B`, 72-byte payload and feature semantics remain unchanged. Existing v2
clients continue receiving exactly that path. No reserved v2 field is reused.

A client can additionally register `ds5HapticsIrExtended` alongside its required
`ds5HapticsIrV2` callback to advertise `ML_FF_DS5_HAPTICS_IR_EXTENDED = 0x80`.
This accepts an enhanced payload on the existing message (`0x550B`) consisting of the original 72-byte
v2 base followed by an optional continuous/event extension. The host sends one
representation per connection: enhanced when both capabilities are present,
v2 for v2-only clients, ordinary rumble when neither is present. An old host
sees the base capability and sends v2. Raw authored PCM and IR remain exclusive.
The enhanced callback alone is not a complete v2 receiver and is rejected.

The control receiver uses the existing v2 message route: a 72-byte payload
goes to the original callback; a longer payload is decoded only when the
enhancement callback was negotiated. No extra message type or channel is added.

The enhanced decoder reuses the existing v2 decoder on the first 72 bytes.
It delivers the base and enhancement together in one callback, without also
issuing a legacy callback or sending a duplicate base message. This avoids
joining separately lost/reordered base and enhancement packets.

Both callbacks execute on the control receive thread and borrow their frame
until return. Copy before deferring work; do not block this thread. C callback
structs are native values, not packed wire layouts. As with the v2 source API,
rebuild callers, headers and library together when the callback struct grows.
Old applications cannot replace only their shared library across that change;
wire fallback and source recompilation compatibility are separate from binary ABI.

## Original v2 base

All integers and IEEE-754 binary32 floats are little-endian. The v2 base always
has its own version 2 and length 72, even inside an enhanced message.

| Base offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Version, 2 |
| 1 | u8 | Discontinuity 1, partial 2, END 4, SILENT 8 |
| 2 | u16 | Base length, 72 |
| 4 | u16 | Client controller number |
| 6 | u16 | Reserved, zero |
| 8 | u32 | Source sequence, shared by base and enhancement |
| 12 | u64 | End of the represented v2 source window, microseconds |
| 20 | u32 | Number of stereo source frames represented |
| 24, 44 | 5 x f32 | Each lane: tactile RMS, peak, relative attack, low-band ratio, sign-transition rate |
| 64 | f32 | Lane correlation |
| 68 | u32 | Reserved, zero |

RMS/peak describe the 50-400 Hz tactile band, with `0 <= rms <= peak <= 1`.
Attack and low-band energy ratio are in [0, 1]; the split is approximately
160 Hz. Sign transitions are a trailing estimate in [0, 48000], not a pitch.
Correlation is in [-1, 1]. All values must be finite. SILENT describes the
current tactile input. These features retain their original v2 meaning and are
not repurposed as continuous envelope values, raw PCM statistics or event IDs.

## Optional enhancement version 1

The outer control message length is `104 + 20 * eventCount`, maximum 264 bytes.
The extension begins immediately after the base. Its length is `32 + 20*N`;
its version is independent of the unchanged base version.

| Extension offset | Type | Meaning |
| --- | --- | --- |
| 0 | u8 | Extension version, 1 |
| 1 | u8 | Event count, 0-8 |
| 2 | u16 | Complete extension length |
| 4 | u32 | Enhancement epoch |
| 8 | u64 | End of the continuous/event analysis state, microseconds |
| 16, 24 | 2 x f32 | Each lane: continuous low, continuous high |
| 32 onward | 20-byte records | Identified source-time events |

Extension version 1 defines 50-180 Hz / 180-400 Hz continuous levels in [0, 1], with
release history. The enhancement window end can follow the last completed v2
hop, but cannot precede the base timestamp. It shares the same 48 kHz source
clock. The source sequence, controller identity, lifecycle flags, source count
and original signal features are reused from the base rather than duplicated.
A source count is metadata, not a payload length or an allocation request;
positive u32 counts are independent of the SDK's current 240-frame input limit.
A zero count is accepted in an enhanced message only with END.

Each event contains `eventId:u32` at offset 0, `sourceTimeUs:u64` at offset 4,
then `leftStrength:f32` and `rightStrength:f32` at offsets 12 and 16, in [0, 1].
Wire records have no native alignment padding. Event time must not exceed the
enhancement window end. Decoder validation covers the complete v2 base,
extension sizes/version and finite ranges. Analysis profile, confidence and
sharpness are not transported; the extension version fixes the field semantics.
No device model, preset, gain or output-rate policy is carried here.

## Playback semantics

Newer valid frames replace continuous state. Events use
`(base.controllerNumber, epoch, eventId)` identity and immutable published values.
A producer can repeat recent events as bounded transport redundancy. Clients
can recover an unseen event from a reordered packet without replacing newer
continuous state. The base source sequence and epoch use unsigned 32-bit serial
ordering with rollover; zero is valid. Restart/discontinuity advances the epoch.

Base END terminates its epoch and clears pending playback; old-epoch END must
not stop a new source. Base SILENT must not erase an enhanced continuous release
tail. When enhancement is absent, the client retains its v2 renderer. When it
is present, it consumes the supplied continuous state and source events instead
of also estimating and playing the base attack a second time. Queues, freshness
limits, missed-event cleanup and phone/controller routing belong to the client.
Source time is not audio/video PTS and does not promise cross-media synchronization.
