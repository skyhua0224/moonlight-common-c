# Protocol extension allocation registry

This document is the allocation contract for protocol extensions maintained
in this `moonlight-common-c` fork. It is the source of truth for identifiers
shared by the host and client projects. Keep the client flags, host flags, and
control opcodes in their own registries.

## Client feature flags

These values are sent by the client in `x-ml-general.featureFlags`.

### Upstream assignments

| Identifier | Value | Used for |
| --- | ---: | --- |
| `ML_FF_FEC_STATUS` | `0x01` | Tells the host that the client sends frame-loss FEC status feedback. |
| `ML_FF_SESSION_ID_V1` | `0x02` | Enables the session ID, ping payload, and connection-data extensions. |

### Frozen fork assignments

These values are already deployed in the fork ecosystem. They must not be
reused or renumbered.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `ML_FF_DS5_HAPTICS_PCM` | `0x04` | Tells the host that the client accepts authored DualSense PCM haptics. |
| `ML_FF_DS5_HAPTICS_IR_V2` | `0x08` | Tells the host that the client selected simulated DualSense and accepts IR v2. |
| `ML_FF_REMOTE_TEXT_CONTEXT` | `0x10` | Tells the host that the client accepts remote editable-text focus context updates. |
| `ML_FF_PYROWAVE` | `0x20` | Tells the host that the client accepts the experimental PyroWave video contract. |
| `ML_FF_DS5_HAPTICS_STATE` | `0x40` | Tells the host that the client accepts absolute DualSense actuator state updates on opcode `0x550D`. |
| `ML_FF_DS5_HAPTICS_IR_EXTENDED` | `0x80` | Tells the host that the client accepts continuous-state and source-event DualSense IR v2 updates on opcode `0x550B`. |
| `ML_FF_HOST_SESSION_STATE` | `0x100` | Tells the host that the client accepts host lock-state notifications on opcode `0x550E`. |

### New fork assignments

New fork-only client flags are allocated from `0x8000` downward. Reserve the
next unused high bit before implementation and add it to this table.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `ML_FF_ENDPOINT_AUTO_REFRESH` | `0x8000` | Tells the host that the client supports authenticated UDP endpoint migration v1. Activation also requires encrypted RTSP and `x-ml-general.endpointAutoRefreshVersion:1`. |

## Host feature flags

These values are advertised by the host in `x-ss-general.featureFlags`.

### Upstream assignments

| Identifier | Value | Used for |
| --- | ---: | --- |
| `LI_FF_PEN_TOUCH_EVENTS` | `0x01` | Advertises pen and touch event injection support. |
| `LI_FF_CONTROLLER_TOUCH_EVENTS` | `0x02` | Advertises controller touch event injection support. |

### Frozen fork assignments

These values are already deployed in the fork ecosystem. They must not be
reused or renumbered.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `LI_FF_TOUCHPAD_EVENTS` | `0x10` | Advertises native precision touchpad event support. |
| `LI_FF_TOUCHPAD_FRAME_EVENTS` | `0x20` | Advertises precision touchpad frame event support. |
| `LI_FF_CURSOR_SHAPE` | `0x40` | Advertises client-rendered cursor shape updates. |
| `LI_FF_DS5_HAPTICS_PCM` | `0x80` | Advertises authored DualSense stereo PCM haptics from the host. |
| `LI_FF_DYNAMIC_SDR_WHITE` | `0x100` | Advertises runtime client SDR reference-white updates. |
| `LI_FF_REMOTE_TEXT_CONTEXT` | `0x200` | Advertises host support for remote editable-text focus context updates. |
| `LI_FF_HOST_SESSION_STATE` | `0x800` | Advertises host lock-state notifications so the client can react to a locked desktop. |

### New fork assignments

New fork-only host flags are allocated from `0x8000` downward. Reserve the
next unused high bit before implementation and add it to this table.

| Identifier | Value | Used for |
| --- | ---: | --- |
| `LI_FF_ENDPOINT_AUTO_REFRESH` | `0x8000` | Advertises authenticated UDP endpoint migration v1. The host enables it only after the client requests version 1 over encrypted RTSP. |

## Assigned control opcodes

These opcodes are already assigned in the current `mic` protocol baseline.
The values are separate from both feature-flag namespaces.

| Opcode | Used for |
| --- | --- |
| `0x5500` | Rumble triggers. |
| `0x5501` | Controller motion events. |
| `0x5502` | RGB LED updates. |
| `0x5503` | Adaptive trigger updates. |
| `0x5504` | Microphone data. |
| `0x5505` | Microphone configuration. |
| `0x5506` | Dynamic parameter changes. |
| `0x5507` | Stream resolution change notifications. |
| `0x5508` | Opaque clipboard synchronization payloads. |
| `0x5509` | Local cursor mode and cursor shape updates. |
| `0x550A` | Authored DualSense PCM haptics. |
| `0x550B` | Device-independent DualSense IR v2 haptics. |
| `0x550C` | Remote text context updates. |
| `0x550D` | Absolute DualSense actuator state updates. |
| `0x550E` | Host interactive session lock-state notifications. |

## Reserved private opcodes

The range `0x55F0`–`0x55FF` is reserved for future fork-private control or
protocol opcodes. Reserve one value per opcode and document its payload,
direction, version, and lifecycle in the common-c headers and this registry.
These values must not be used for feature flags.

## Compatibility contract

Feature bits are hints and must not be the only activation condition for a new
protocol. New features must carry an explicit version or capability attribute,
validate the complete wire payload, and reject unsupported versions without
breaking the base stream.

Unknown optional flags and opcodes must be ignored safely. Once an identifier
is published, its meaning and wire format are immutable; incompatible changes
require a new identifier or protocol version.
