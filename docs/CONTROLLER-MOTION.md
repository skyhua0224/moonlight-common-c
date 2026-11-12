# Shared controller motion forwarding

The input stream owns remote demand, queued motion and explicit gyro stops.
Platform code owns acquisition, coordinates, source selection and local assistance.
Local consumers must receive source samples before remote forwarding decisions;
host disablement must not disable a sensor still needed by a local consumer.

## Contract

- The control stream calls the internal `setControllerMotionEnabled` before notifying the
  platform. Positive host rates enable that remote sensor; zero disables it. Platforms
  apply acquisition rates where supported, without decimating callback samples.
- `LiSendControllerMotionEvent` stores the supplied vector in an immutable packet.
  Ordinary zero readings remain ordinary samples. Its return value reports enqueue
  acceptance (or a no-op for a disabled sensor), not host acknowledgement. Rejected
  movement is never replayed later.
- `LiSendControllerMotionFrame` atomically queues gyro then acceleration when both
  are requested. Use it only for vectors from the same physical report. Independent
  sensor callbacks must not be paired by arrival time. Rejection leaves prior motion
  intact; the platform owning this source must explicitly stop on a rejected final zero.
- `LiStopControllerMotionEvent` invalidates older queued gyro samples and requests
  a reliable zero. The input sender processes this fixed slot even when the queue
  is full. New accepted gyro samples supersede an unsent stop; an already dequeued
  old stop cannot stop newer queued motion. Platforms decide which physical source
  may request a stop; the core does not infer ownership from report format.
- `LiResetControllerMotionEvents` retires queued source data. Preserve demand when
  replacing a source in a surviving host slot; clear it when the host slot is removed.
  Platform callbacks must still reject data from retired physical sources.
- Motion retains the former maximum queue footprint of 32 entries, leaving room for
  buttons and releases. A frame is accepted completely or rejected completely. A
  rejected individually submitted final gyro zero requests a stop through the fixed slot.
- Input shutdown cancels pending work. There is no extra motion worker or retry timer.

The gyro unit is degrees/second; acceleration includes gravity in meters/second².
Packet format, ENet channels and zero reliability are unchanged. No source timestamps
are added to the wire, so network and dispatch timing can still affect integration.

`controller-motion-queue-tests` exercises the production input sender and real queue:
sample identity/integral, partial frames, saturation with button capacity, failed
zeros, source replacement, host disablement and invalidated stop snapshots.
