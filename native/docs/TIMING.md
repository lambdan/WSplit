# How the native port keeps time

Short version: one monotonic clock, splits back-dated to the moment the key
was pressed, and a start delay that ends exactly when it should. These are
small fixes, a few milliseconds each, but a split timer should get them right.

## The clock

`ws_now_ns()` (core/run.c) is the only time source:

| Platform | Source |
|---|---|
| Linux, macOS | `clock_gettime(CLOCK_MONOTONIC)` |
| Windows | `QueryPerformanceCounter` |

Measured on Linux: 1 ns resolution, about 25 ns per read, and 0 ppm drift
against `CLOCK_REALTIME` and `CLOCK_MONOTONIC_RAW` over 20 s. A monotonic clock
never jumps when NTP adjusts the wall clock or the user changes the time.

The stopwatch (`WsStopwatch`) stores the start timestamp and the time
accumulated across pauses, all in nanoseconds. Displayed and saved split times
are truncated to 1/100 s, like the original.

### The original's "fallback timer"

WSplit 1.5 had a `DualStopwatch` that ran .NET's `Stopwatch`
(QueryPerformanceCounter) alongside `DateTime.UtcNow` / `Environment.TickCount`,
and could switch to the latter. It existed because QPC drifted or jumped on some
XP-era multi-core and power-saving CPUs, where each core had its own unsynchronised
TSC. The fallback was arguably worse: `TickCount` has about 15.6 ms resolution, and
`DateTime.UtcNow` jumps whenever the system clock is corrected. Every CPU from
roughly 2008 on has an invariant TSC, and Windows Vista+ picks a safe QPC source
anyway, so the port has no fallback.

## Splits are back-dated to the key press

Between the key going down and the app's handler running there is a delay:
the OS delivers the event, the compositor forwards it, and the app may be busy
drawing a frame. The original timed the split when the handler ran.

Each front-end now works out how long ago the event happened, from the event's
own timestamp, and calls `ws_app_set_input_latency()` right before the action.
The core subtracts that from "now" and uses it for the split, start, pause or
reset (`input_time()` in core/app.c). Anything outside 0 to 1 s is ignored, and
the value is cleared after every action, so a bad timestamp costs nothing.

| Front-end | Event time | Compared with |
|---|---|---|
| macOS, focused key | `NSEvent.timestamp` | `NSProcessInfo.systemUptime` (same clock) |
| macOS, global hotkey | Carbon `GetEventTime` | `GetCurrentEventTime` |
| Linux GTK 4, focused key | `gtk_event_controller_get_current_event_time` (ms) | `CLOCK_MONOTONIC` ms |
| Linux, portal hotkey | `Activated` timestamp (ms) | `CLOCK_MONOTONIC` ms, if it looks like the same clock |
| Windows | `GetMessageTime` | `GetTickCount` |

Wayland and X11 event times are normally `CLOCK_MONOTONIC` milliseconds (Xorg and
Xwayland use it; Wayland compositors generally do too). If a compositor uses some
other clock, the 0 to 1 s sanity check throws the value away. Windows message times only have `GetTickCount` resolution
(10 to 16 ms), so the Windows build only applies a latency above 32 ms, where
it clearly beats the rounding error.

Set `WSPLIT_DEBUG_LATENCY=1` to print the measured latency for each event
(Linux: stderr; Windows: `OutputDebugString`, visible in DebugView).

## Exact start delay

With a start delay (the negative countdown), the original started the
stopwatch on the first refresh tick after the delay had run out. With the
default 15 ms refresh that's up to 15 ms late; measured here it averaged 7.7 ms.
The port starts the stopwatch at the exact end of the delay
(`delay_start + delay`), whenever the tick notices it. The measured error is now
0.02 ms.

## What's left

- **Display refresh** (`RefreshRate`, default 15 ms) only affects how often the
  clock is redrawn, not the recorded times.
- **Double-tap guard** compares event times too, so a slow frame can't make two
  presses look closer together than they were.
