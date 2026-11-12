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
  windows/      Win32 + GDI+ front-end (main.c, wsplit.rc dialogs, resource.h, manifest, .ico)
  tests/        test_core.c: 83 core tests (`make test`)
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
| core | Done, 83 tests pass, ASan/UBSan clean |
| Linux GTK4 | Code complete, screenshot-verified, smoke-tested on macOS GTK. **Not yet tested on Wayland** |
| macOS | Code complete. Owner ran an early build. CI job has been stuck "queued" (macos-latest); check `gh run list`. Window-drag was "finicky": now moves the window manually in `mouseDragged` (DRAG_MOVE) instead of `performWindowDragWithEvent`. **Ask the owner to confirm the fix.** |
| Windows | Code complete; cross-compiles warning-free with Homebrew mingw-w64 (`make windows`). **Never run yet** (no Wine here): check the CI `windows-screenshots` artifact and ask the owner to try it |
| CI | Linux (GTK4, ubuntu-24.04), macOS, Windows (MSYS2 MINGW64) jobs; Linux + Windows render screenshots as a smoke test |

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

### 2. Windows front-end (`windows/main.c`): written, never run
Win32 + GDI+ flat API in C (mingw-w64's `<gdiplus.h>` works from C; don't define `WIN32_LEAN_AND_MEAN`, it breaks it).
- `WS_POPUP|WS_MINIMIZEBOX|WS_SYSMENU` + `WS_EX_LAYERED` (opacity) + `WS_EX_ACCEPTFILES`. Core units are 96 dpi;
  window pixels = core size * `ui.scale` (from `GetDpiForWindow`, updated on `WM_DPICHANGED`); paint scales the world transform.
- Painter: memory DC + `GdipCreateFromHDC`, generic-typographic string format (+ MeasureTrailingSpaces, NoWrap),
  `PixelOffsetModeHalf`, font metrics from cell ascent/descent. Icons: GDI+ bitmaps copied to memory; grey via ColorMatrix.
- Move = `ReleaseCapture` + `WM_NCLBUTTONDOWN/HTCAPTION`. Edge drags use capture + screen coords; left/top edges move the window too.
- Menu: `TrackPopupMenu(TPM_RETURNCMD)`, `&` escaped, radio items via `MFT_RADIOCHECK`, shortcut after `\t`.
- Keys: `WM_KEYDOWN`/`WM_SYSKEYDOWN`, bit 30 = repeat. Latency from `GetMessageTime()` only when 32..1000 ms.
  Hotkeys: `RegisterHotKey` + `MOD_NOREPEAT`, id = action + 1.
- Dialogs from `wsplit.rc` (prompt, hotkey with `HOTKEY_CLASS`, run editor using the text grid; CRLF conversion
  for the edit control). `MessageBoxW`, `GetOpen/SaveFileNameW`, `ChooseFontW`. Digital-7 from RCDATA via `GdipPrivateAddMemoryFont`.
- Settings: `%APPDATA%\WSplit\settings.ini`; remember-position + always-on-top supported (`WS_MENU_PLATFORM_WINDOWS`).
- `--screenshot/--mode/--simulate` work (PNG via GDI+ encoder); used by CI.
The owner asked whether this should **replace** the C# app: don't delete `WSplit/` without asking.

### 3. CI
Done: Linux (GTK4), macOS, Windows jobs; releases on `v*` tags (Linux tar.gz, macOS zip of an ad-hoc-signed
app (users allow it in Privacy & Security), Windows zip). `gh` isn't installed on the owner's Mac; the public API
works without auth: `curl -s https://api.github.com/repos/lambdan/WSplit/actions/runs?per_page=3`, then
`.../runs/<id>/jobs` and `.../check-runs/<job id>/annotations`. Full logs need auth.
macOS jobs queue for a long time (runner capacity). The macOS unit-test failure at 632a5cc was most likely
the timing test assuming a 300 ms sleep is exact; fixed to not depend on sleep accuracy.

### 4. Timing write-up: done (`native/docs/TIMING.md`); notes it was based on:
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
