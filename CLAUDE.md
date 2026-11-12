# CLAUDE.md: WSplit native port

100% vibecoded, I don't give a shit.

## Repo rules (from the owner)

- **Never send anything upstream** (no PRs, issues or pushes to Nitrofski/WSplit). Everything stays on this fork.
- Commit messages end with the attribution trailer lines (Co-Authored-By etc.).
- The owner uses **Wayland only**. X11 support has been removed (GTK4 front-end).

## Layout

```
native/
  core/         portable C11, no UI deps: everything except windows/input
    wsplit.h    the whole API: WsRun, WsStopwatch, WsSettings, WsApp, WsPainter, WsHost, WsMenu
    run.c       run model (line-by-line port of Split.cs/Segment.cs), stopwatch, time format/parse,
                UTF-8 path helpers (ws_fopen / ws_replace_file / ws_remove: wide APIs on Windows)
    runfile.c   .wsplit load/save (same format as Windows WSplit, CRLF) + LiveSplit .lss import
    runtext.c   "Name | Old | Best | BestSeg | Icon" text grid used by the Win/Linux run editors
    settings.c  settings.ini (key=value), colour schemes, hotkey string parse/format, recent files
    app.c       controller = port of the WSplit form logic (split/pause/reset/stop/prompts/layout)
    render.c    the renderer = port of WSplit.Painter (all 4 display modes) via WsPainter callbacks
    menu.c      shared context-menu model; front-ends render it natively, ids >= WS_CMD_CORE_BASE run in core
  linux/        GTK 4 front-end (wsplit-gtk.c) + io.github.lambdan.WSplit.desktop
  macos/        AppKit front-end (main.m, Dialogs.m, WSplitMac.h, Info.plist, make_icns.sh)
  windows/      Win32 + GDI+ front-end: resources only so far (wsplit.rc, resource.h, manifest, .ico)
  tests/        test_core.c: 82 core tests (`make test`)
  assets/       digital-7-mono.ttf (extracted from the original exe), icons/*.png
  docs/         README screenshots
.github/workflows/native.yml   CI: Linux + macOS builds, release on v* tags
```

### Architecture

- The front-end creates a `WsHost` (dialogs, resize, invalidate, hotkey re-register, icon load,
  window props) and calls `ws_app_init`. It forwards input to `ws_app_local_key` /
  `ws_app_hotkey` / `ws_app_timer_control`, calls `ws_app_tick` every `settings.refresh_ms`,
  and renders with `ws_render(app, painter)`.
- `WsPainter` is ~12 primitives (fill, gradient, line, text, metrics, clip, transform, icon).
  The renderer emulates GDI+ text padding (em/6) itself, so painters must draw text **without** padding.
- Layout: the core owns the window size (`a->win_w/h`) and asks the host to resize. User resizes
  go through `ws_app_user_resized(w, h)`.
- Menu: `ws_menu_build(app, platform)` → tree; on click `ws_menu_activate(app, id)` returns
  `WS_CMD_NONE` (handled) or a `WS_CMD_*` the front-end must handle (open dialog etc.).
  Platforms: `WS_MENU_PLATFORM_MAC` (has a settings window), `..._LINUX` (Wayland: hides
  always-on-top + remember-position), `..._WINDOWS`.
- **Timing:** `ws_now_ns()` = CLOCK_MONOTONIC (QPC on Windows). Before invoking an input action,
  front-ends call `ws_app_set_input_latency(app, ns)` computed from the OS event timestamp, so
  splits are back-dated to the real key press. The start delay is back-dated to its exact end.
  The core clears `input_ns` after each action.

## Build & test

```sh
cd native
make test                 # core unit tests (any platform, needs only a C compiler)
make                      # Linux: ./wsplit   macOS: WSplit.app
./wsplit --screenshot out.png --mode 3 --simulate 4 examples/sm64_16star.wsplit
                          # offscreen render, no display needed; mode 0-3, simulate N splits
```

Keep `--screenshot/--mode/--simulate` in the GTK front-end too: it's the quickest way to check
rendering. Compare against `docs/overview_*.png`.

## Status

| Platform | State |
|---|---|
| core | Done, 82 tests pass, ASan/UBSan clean |
| Linux GTK4 | Code complete, screenshot-verified, smoke-tested on macOS GTK. **Not yet tested on Wayland** |
| macOS | Code complete. Owner ran an early build. CI job has been stuck "queued" (macos-latest); check `gh run list`. Window-drag was "finicky": now moves the window manually in `mouseDragged` (DRAG_MOVE) instead of `performWindowDragWithEvent`. **Ask the owner to confirm the fix.** |
| Windows | Resources/manifest/icon done; `windows/main.c` **not written yet** |
| CI | Linux job switched to GTK4 on ubuntu-24.04 (+ screenshot smoke test). Needs: Windows (MSYS2 mingw64) |

## In progress / next steps

### 1. GTK4 front-end (Wayland-native): written, needs a real Wayland test
`linux/wsplit-gtk.c` replaced the X11 files. Requires GTK ≥ 4.12 (`gtk_css_provider_load_from_string`,
`gtk_widget_compute_point`). Builds warning-free; the `--screenshot` renders match `docs/`; the window starts
on macOS with Homebrew gtk4 (`make wsplit` works on macOS for compile/smoke tests, use
`XDG_CONFIG_HOME=<tmp>` so you don't touch real settings). **Not yet run on Wayland.** Ask the owner to try:
window move (`gdk_toplevel_begin_move`), clock-edge resize, right-click `GtkPopoverMenu` (nested submenus),
key latency (`WSPLIT_DEBUG_LATENCY=1` prints it), and the GlobalShortcuts portal flow.

How it's built (keep consistent when changing):
- `GtkApplication` `io.github.lambdan.WSplit` + `G_APPLICATION_HANDLES_OPEN`; core is initialised only in the
  primary instance (after `g_application_register`). Second launch → `open` → prompt-to-save in an idle, then load.
- Undecorated non-resizable window + `GtkDrawingArea`; host resize sets content size + size request + default size.
- Refresh: `g_timeout_add_full(G_PRIORITY_HIGH, refresh_ms)`, re-armed from inside `tick` when `refresh_ms` changes.
- Synchronous dialogs: `GtkAlertDialog`/`GtkFileDialog`/`GtkFontDialog` and small custom modal windows, all
  waited on with a nested `g_main_context_iteration` loop (`wait_run`, `dlg_run`), with `modal_open` set.
- Menu: `WsMenu` → `GMenu` (separators = sections, `_` escaped), one `GSimpleAction` per item in group "ws";
  activation is deferred to an idle (core actions can prompt). Popover unparented in an idle after "closed".
- Keys: controller on the window; keydown[] drops auto-repeat and is cleared when the window loses focus.
  Key names use the keyval with no modifiers (`gdk_display_translate_key`), so Shift+1 = "1".
- Hotkeys: portal on a **private** D-Bus connection so `org.freedesktop.host.portal.Registry.Register` can be the
  first call. Rebinds (new session, since BindShortcuts is once per session) only when the trigger strings
  change; enable/disable is handled by the core. Does nothing if no hotkey is set. One warning if the portal is missing.
  `Activated` timestamp goes through the same 0..1 s latency sanity check (unverified that it's CLOCK_MONOTONIC ms).
- Run editor: Title/Goal/Attempts/Start delay + monospace text grid, "Auto-fill best segments", "Insert icon path...".
  Same update_best / restore_best / close_file + keep path + keep detail size flow as macOS.
- Icons load via `gdk_texture_new_from_filename` (any gdk-pixbuf format). Ctrl+O/S/E/Q shortcuts.

### 2. Windows front-end (`windows/main.c`, Win32 + GDI+ flat API in C)
Plan: `WS_POPUP|WS_MINIMIZEBOX|WS_SYSMENU`, double-buffered GDI+ (`GdipCreateFromHDC` on a memory DC),
`GdipStringFormatGetGenericTypographic` (no padding; the core adds it), `PixelOffsetModeHalf`,
per-monitor DPI with a world-transform scale. Move = `ReleaseCapture` + `WM_NCLBUTTONDOWN/HTCAPTION`
(what the original did). `RegisterHotKey` with `MOD_NOREPEAT`. Menus via `TrackPopupMenu(TPM_RETURNCMD)`.
`MessageBoxW`, `GetOpen/SaveFileNameW`, `ChooseFontW`. Dialogs come from `wsplit.rc` (prompt, hotkey
with HOTKEY_CLASS, run editor using the text-grid format). Digital-7 from RCDATA via `GdipPrivateAddMemoryFont`.
Latency from `GetMessageTime()` only if > ~32 ms (tick resolution). Settings: `%APPDATA%\WSplit\settings.ini`.
Link: `-mwindows -static -lgdiplus -lcomdlg32 -lcomctl32 -lshell32 -lole32`. Build in CI with MSYS2 mingw64; windres run from `native/`.
Use `WS_MENU_PLATFORM_WINDOWS`. The owner asked whether this should **replace** the C# app: don't delete `WSplit/` without asking.

### 3. CI
Add GTK4 Linux + Windows jobs, keep releases on `v*` tags (artifacts: Linux tar.gz, macOS zip of
an ad-hoc-signed unsigned app (users allow it in Privacy & Security), Windows zip). Read failures with
`gh run view --log-failed`.

### 4. Timing write-up (`native/docs/TIMING.md`)
Measured here:
- **CLOCK_MONOTONIC:** 1 ns resolution, ~25 ns per read, 0 ppm drift vs CLOCK_REALTIME/RAW over 20 s.
- **Start delay (fixed):** used to start up to 15 ms late (avg 7.7 ms); now 0.02 ms.
- **Key latency:** now back-dated using event timestamps.

The original WSplit's "fallback timer" existed because QueryPerformanceCounter drifted on some
XP-era multi-core/power-saving CPUs. It compared QPC against DateTime.UtcNow/TickCount (15.6 ms
resolution, and DateTime jumps with NTP), so the fallback was arguably worse. That's not an issue on
modern invariant-TSC hardware. Splits are truncated to 1/100 s, like the original.

## Gotchas

- `.wsplit` segment lines are `Name,Old,Best,BestSeg` split on exactly 3 commas, so **names can't
  contain commas** (same as the original). Editors replace them with spaces.
- Don't add the original's "missing icon" (Nintendo artwork); a neutral "?" placeholder is drawn instead.
- Digital-7 Mono is freeware for personal use; it's the font embedded in the original exe.
- Two bugs deliberately fixed vs the original renderer are commented in `render.c` (segment
  background covering the status bar; goal text overlapping deltas).
