# CLAUDE.md: WSplit native port

100% vibecoded, I don't give a shit.

## Repo rules (from the owner)

- **Never send anything upstream** (no PRs, issues or pushes to Nitrofski/WSplit). Everything stays on this fork.
- Commit messages end with the attribution trailer lines (Co-Authored-By etc.).
- The owner uses **Wayland only**. X11 support is being removed (see "In progress").

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
  linux/        CURRENT: X11 + cairo front-end (main.c, ui.c, x11.h). TO BE REPLACED by GTK4, see below
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
| Linux X11 | Done and tested under Xvfb, **but being dropped** (owner is Wayland-only) |
| macOS | Code complete. Owner ran an early build. CI job has been stuck "queued" (macos-latest); check `gh run list`. Window-drag was "finicky": now moves the window manually in `mouseDragged` (DRAG_MOVE) instead of `performWindowDragWithEvent`. **Ask the owner to confirm the fix.** |
| Windows | Resources/manifest/icon done; `windows/main.c` **not written yet** |
| CI | Linux job green. Needs: switch Linux job to GTK4 (ubuntu-24.04), add Windows (MSYS2 mingw64) |

## In progress / next steps

### 1. Replace X11 with a GTK4 front-end (Wayland-native)  ← top priority
Delete `linux/main.c`, `linux/ui.c`, `linux/x11.h`; write e.g. `linux/wsplit-gtk.c`. Requires GTK ≥ 4.10.
Reuse from the old `linux/main.c`: the whole cairo painter (`p_*` functions, `use_font`, FreeType
Digital-7 loading via cairo-ft, PNG icon loader with greyscale copy), asset-dir discovery, and the
`--screenshot` mode.

Design decided so far:
- `GtkApplication` id `io.github.lambdan.WSplit`, `G_APPLICATION_HANDLES_OPEN` (second launch
  opens the file in the running instance). Rename `linux/wsplit.desktop` → `io.github.lambdan.WSplit.desktop`
  (portals identify apps by this).
- Undecorated, non-resizable `GtkWindow` holding a `GtkDrawingArea`. Host resize →
  `gtk_drawing_area_set_content_width/height` (+ `gtk_window_set_default_size`).
- Refresh: `g_timeout_add_full(G_PRIORITY_HIGH, refresh_ms, ...)`, re-armed when `refresh_ms` changes.
- Button 1: `GtkGestureDrag`. On drag-begin, hit-test (same logic as the old `hit_test`).
  MOVE → `gdk_toplevel_begin_move(...)` + `gtk_event_controller_reset`. Clock-edge/segment drags →
  handle in drag-update. Left/top edges can't move the window on Wayland, so they just resize.
- Cursors: `GtkEventControllerMotion` + `gtk_widget_set_cursor_from_name` (ew-resize, ns-resize, nwse-resize, nesw-resize).
- Right-click menu: `GtkPopoverMenu` from a `GMenu` built from `WsMenu`. Separators start new
  sections. One `GSimpleAction` per item in a `GSimpleActionGroup` inserted as "ws"; stateful
  boolean for check/radio items; `g_simple_action_set_enabled` for disabled ones. **Escape `_` as
  `__` in labels** (mnemonics; recent-file names have underscores). Unparent the popover in an idle after "closed".
- Keys: `GtkEventControllerKey`. Keep a keydown[] set to drop auto-repeat (GTK4 has no repeat flag).
  Event time from `gtk_event_controller_get_current_event_time` (compositor ms ≈ CLOCK_MONOTONIC) →
  latency with the same 0..1000 ms sanity check as the old `x_event_latency_ns`.
- Synchronous host dialogs (core expects `ask()` to return): `GtkAlertDialog` / `GtkFileDialog` /
  `GtkFontDialog` with a nested `g_main_context_iteration` loop until the callback fires. The timer
  keeps ticking because the timeout runs in that loop.
- Run editor: own modal window with Title/Goal/Attempts/Delay entries + monospace `GtkTextView`
  using `ws_run_segments_to_text` / `ws_run_segments_from_text`, plus an "Auto-fill best segments"
  button (`ws_run_autofill_best_segments`). Before opening: `ws_run_update_best`. On cancel:
  `ws_run_restore_best`. On OK: `ws_app_run_edited`. See macOS `WSShowRunEditor` for the exact flow.
- Global hotkeys: **xdg-desktop-portal GlobalShortcuts** over GDBus (no extra deps). CreateSession
  → Response(session_handle) → BindShortcuts(session, a(sa{sv}) [(id, {description, preferred_trigger})], "", {handle_token}).
  Subscribe to `Activated(o session, s id, t timestamp, a{sv})` → map id → `ws_app_hotkey`.
  Request object path = `/org/freedesktop/portal/desktop/request/<unique name without ':' and '.'→'_'>/<token>`;
  subscribe to its Response **before** calling. Bind all 8 ids (the core ignores non-toggle hotkeys
  while disabled). On settings change: close the session (`org.freedesktop.portal.Session.Close`)
  and rebind. Trigger format is the XDG shortcuts spec: `CTRL+SHIFT+F1`, mods CTRL/ALT/SHIFT/LOGO,
  key = xkb keysym name (Space→space, Enter→Return, Numpad1→KP_1, PageUp→Page_Up,
  Backspace→BackSpace, Minus→minus, Quote→apostrophe, letters lowercase). If the portal is
  missing, show one message: needs GNOME 48+, KDE Plasma 6 or Hyprland.
- Opacity: `gtk_widget_set_opacity(window)`. Always-on-top and remember-position are impossible
  on Wayland (hidden in the menu). Drag & drop: `GtkDropTarget` for `G_TYPE_FILE`.
- Settings path: `g_get_user_config_dir()/wsplit/settings.ini`.
- Makefile: `pkg-config gtk4 freetype2`. CI Linux job: ubuntu-24.04, `libgtk-4-dev libfreetype-dev`.

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
