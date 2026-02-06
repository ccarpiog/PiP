# Multi-Window PiP: Implementation Plan

## Overview

This document outlines the plan to enhance the PiP application's multi-window experience, allowing users to have multiple floating windows with different input sources (monitors, windows, cameras, HLS streams) simultaneously.

**Current state:** The app already supports creating multiple windows via Cmd+N. Each `Window` (NSPanel subclass) independently manages its own capture session, renderer, and source selection. However, there is no window management UI, no guidance for new windows, no layout tools, and GPU resources are duplicated per window.

**Goal:** Make multi-window a first-class, polished experience rather than just a side effect of "you can press Cmd+N again."

---

## Technical Feasibility Analysis

### Concurrent Capture Sessions

| Source Type | API | Concurrent Limit | Notes |
|---|---|---|---|
| Display | CGDisplayStream | No hard limit | Bound by GPU bandwidth; practical limit ~4-6 |
| Window | SCStream (ScreenCaptureKit) | No hard limit | Queue depth should stay ≤ 8 per stream to avoid memory pressure |
| Camera | AVCaptureSession | **One session per device** | Same camera cannot be opened by two sessions; must fan out frames |
| HLS | AVPlayer | No hard limit | Network bandwidth is the bottleneck |
| AirPlay | Custom RAOP server | 10 sessions (current soft limit) | Already supports multi-session |

**Key constraint:** A camera device is exclusive to one `AVCaptureSession`. If two windows want the same camera, a source-sharing layer is needed (deferred to Phase 3).

### GPU Resources

- `MTLDevice` is thread-safe and should be created once and shared across all windows.
- Each window can safely create its own `MTLCommandQueue` from the shared device.
- Pipeline states and shader libraries should be cached and reused.
- This reduces VRAM usage and avoids redundant device initialization.

### Realistic Performance Bounds

- **Target use case:** 2-4 simultaneous windows (typical user).
- **Soft limit:** 6 windows (show performance warning).
- **Hard limit:** 8 windows (configurable in preferences).
- This is a lightweight PiP tool, not OBS. Optimize for the common case.

---

## UX Design Decisions

### Window Creation Flow

**Decision:** New windows open blank with an overlay hint. No modal picker.

- Cmd+N creates a new window.
- If preference is "Clone current": new window copies the frontmost window's source.
- If preference is "Blank with hint" (default): new window shows a centered overlay message: _"Clic derecho para seleccionar fuente"_ (Right-click to select source).
- The overlay is a simple `NSTextField` on a semi-transparent `NSVisualEffectView`, centered on the window.
- The overlay disappears when a source is selected.

### Window Identification

- Each window's `title` is already set to the source name (e.g., "Display 1", "Safari - Google", "FaceTime HD Camera").
- AppKit will use these titles in the Window menu automatically.
- When hovering over a borderless window, the title bar appears briefly (existing behavior), showing the source name.
- No color-coded borders for Phase 1 (adds visual noise).

### Source Selection

- **Keep right-click context menu per window.** It's simple, already works, and aligns with user mental model.
- No changes to the source selection mechanism in Phase 1.

### Closing Behavior

- Closing a window closes only that window (existing behavior).
- "Close All Windows" quits the app (matches Preview.app behavior).
- No confirmation dialog (not a destructive app).
- Menu bar persistence is a future consideration.

### Layout Management

- **Arrange in Grid:** Distributes all open PiP windows in a grid on the current screen.
- **Cascade:** Offsets windows diagonally from top-left.
- Both use `screen.visibleFrame` to respect menu bar and dock.
- Grid preserves each window's aspect ratio (scale to fit within cell, center in cell).

### Preferences

Two new preferences for Phase 1:
1. **New window behavior:** "Blank with hint" (default) / "Clone current window"
2. **Maximum simultaneous windows:** Numeric, default 8

---

## Implementation Phases

### Phase 1: Foundation

Minimum viable multi-window enhancement. Low risk, high impact.

#### 1.1 Automatic Window List in Window Menu

**Files:** `main.m`

- Call `[NSApp setWindowsMenu:windowMenu]` after creating the Window menu.
- AppKit will automatically list all open `Window` instances by their `title`.
- Ensure each window has a meaningful title (already the case: source name is set in `-changeWindow:`).
- Add custom items above the auto-managed list: "Close All Windows", "Arrange in Grid", "Cascade".

#### 1.2 Close All Windows

**Files:** `main.m`

- Add menu item "Cerrar todas las ventanas" (Close All Windows) with shortcut Opt+Cmd+W.
- Action: iterate `[NSApp windows]`, close each `Window` instance.
- App terminates when last window closes (existing behavior via `applicationShouldTerminateAfterLastWindowClosed:`).

#### 1.3 Overlay Hint for Blank Windows

**Files:** `window.m`

- When a new window is created (non-AirPlay) and no source is selected:
  - Add a `NSVisualEffectView` overlay (blending mode: behind window, material: dark) centered on the window.
  - Inside it, a centered `NSTextField` with text: _"Clic derecho para seleccionar fuente"_.
  - Style: white text, medium system font, non-editable, non-selectable.
- When `-changeWindow:` is called and a source is selected, remove (hide) the overlay.
- The overlay should resize with the window (auto-resizing mask or constraints).

#### 1.4 Arrange in Grid

**Files:** `main.m` or a new utility function in `window.m`

Algorithm:
1. Collect all open `Window` instances from `[NSApp windows]`.
2. Get `visibleFrame` of the screen where the frontmost window resides.
3. Calculate grid: `cols = ceil(sqrt(N))`, `rows = ceil(N / cols)`.
4. Cell size: `cellW = visibleFrame.width / cols`, `cellH = visibleFrame.height / rows`.
5. For each window:
   - Calculate target size: scale to fit within cell while preserving aspect ratio.
   - Clamp to window's `minSize`.
   - Center within the cell.
   - Animate with `setFrame:display:animate:`.

Edge cases:
- Single window: center it on screen, don't resize.
- Respect each window's minimum size.

#### 1.5 Cascade

**Files:** `main.m` or utility function

Algorithm:
1. Start at top-left of `visibleFrame`.
2. Each window offset by (25, -25) from the previous.
3. Don't resize windows.
4. Wrap back to top-left if cascade goes off-screen.

#### 1.6 Shared MTLDevice

**Files:** `main.m` (or app delegate), `window.m`, `metalRenderer.m`

- Create `MTLCreateSystemDefaultDevice()` once in the app delegate and store it.
- Pass the shared device to each `Window` at creation time.
- `Window` passes it to `MetalRenderer` init.
- `MetalRenderer` uses the shared device but creates its own `MTLCommandQueue`.
- The device is retained by the app delegate for the app's lifetime.

#### 1.7 New Preferences

**Files:** `preferences.m`, `preferences.h`

Add two new rows to the preferences table:
1. **"Comportamiento de nueva ventana"** (New window behavior): Popup with options "En blanco con pista" / "Clonar ventana actual".
2. **"Máximo de ventanas simultáneas"** (Max simultaneous windows): Popup with options 2, 4, 6, 8, 10.

#### 1.8 Max Windows Enforcement

**Files:** `main.m` (in the Cmd+N handler)

- Before creating a new window, check count of open `Window` instances.
- If at limit:
  - First time: show `NSAlert` with message _"Se alcanzó el máximo de ventanas. Puedes cambiar el límite en Preferencias."_ and "OK" + "Preferencias..." buttons.
  - Subsequent times: just `NSBeep()`.
- Track "has shown alert" with a simple static boolean.

#### 1.9 Keyboard Shortcut to Cycle Windows

**Files:** `main.m`

- Add Cmd+` (backtick) to cycle through open PiP windows (this is standard macOS behavior and may already work via AppKit if the Window menu is properly configured).
- Verify it works; if not, add a manual implementation that calls `[NSApp windows]` and `makeKeyAndOrderFront:` on the next window.

---

### Phase 2: Management

Better visibility and control over multiple windows.

#### 2.1 Window Manager Panel

- New `NSPanel` with an `NSTableView`.
- Columns: source icon + name, type (Display/Window/Camera/HLS), status (Active/Paused/Disconnected).
- Click a row to focus that window.
- Context menu on row: Close, Reassign Source, Clone.
- Opened via Window menu item "Administrador de ventanas" or keyboard shortcut.

#### 2.2 Clone Current Window

- Menu item "Clonar ventana" (Clone Window) with shortcut Shift+Cmd+N.
- Creates a new window and copies the frontmost window's source (`WindowSel`).
- Calls `-changeWindow:` on the new window with the same selection.
- For cameras: warn that the same camera can't be opened twice; offer to pick a different source.

#### 2.3 Disconnect / Error Handling

Uniform handling across all windows:
- **Display disconnected:** Stop stream, show overlay _"Pantalla desconectada"_ with option to pick a new source.
- **Camera unplugged:** Stop session, show overlay _"Cámara no disponible"_.
- **Captured window closed:** Stop stream, show overlay _"Ventana cerrada"_.
- All overlays include a "Select new source" button.

#### 2.4 Performance Warnings

- When opening the 6th+ window, show a brief non-modal notification: _"Muchas ventanas abiertas. El rendimiento puede verse afectado."_
- Optional: show FPS indicator per window (toggle in preferences).

---

### Phase 3: Power Features

Architectural changes for advanced users.

#### 3.1 CaptureSourceController Abstraction

- Create per-source-type controllers: `DisplaySourceController`, `WindowSourceController`, `CameraSourceController`, `HLSSourceController`.
- Each has `-start`, `-stop`, and a delegate callback delivering native frame data.
- Window becomes a consumer that receives frames from a controller.
- This decouples capture logic from window management.

#### 3.2 Source Sharing

- When multiple windows select the same source, they attach as consumers to one `CaptureSourceController`.
- Especially important for cameras (AVCaptureSession exclusivity).
- One capture session fans out frames to multiple renderers.
- Source-specific controls (resolution, audio) live on the controller; any window can present them but changes affect all attached windows.

#### 3.3 Adaptive Throttling

- Monitor total capture load (aggregate FPS, frame times).
- When load exceeds threshold, automatically reduce FPS or resolution on lower-priority windows.
- Priority can be based on: frontmost > visible > minimized.
- ScreenCaptureKit `queueDepth` kept ≤ 8 per stream.

#### 3.4 Per-Window Quality Settings

- Move renderer type, FPS cap, resolution, and crop settings to per-window preferences.
- Access via right-click menu > "Window Settings" submenu.
- Global preferences become defaults for new windows.

#### 3.5 Session Persistence / Restore

- On quit, save the list of open windows with their source identifiers, positions, and sizes to `NSUserDefaults`.
- On launch, attempt to restore the session:
  - For displays: match by EDID identifier (already used for custom names).
  - For windows: match by app name + window title (best effort).
  - For cameras: match by `uniqueID`.
  - For HLS: match by URL.
- If a source is unavailable, open the window blank with a "Source unavailable" overlay.

---

## Summary

| Phase | Effort | Impact | Risk |
|---|---|---|---|
| Phase 1 | Low-Medium | High (usable multi-window) | Low |
| Phase 2 | Medium | Medium (better management) | Low |
| Phase 3 | High | Medium (power users) | Medium (architectural changes) |

Phase 1 can be implemented without major refactoring — it builds on the existing architecture. Phases 2 and 3 introduce new abstractions but can be done incrementally.

The key insight is that the app already supports multiple independent windows. The work is primarily about **UX polish** (guiding users, managing windows, arranging layouts) and **resource optimization** (shared GPU device, performance limits), not about fundamental architectural changes.

---

## Phase 1 Implementation Status

**All Phase 1 items implemented.** Files modified:

| File | Changes |
|---|---|
| `main.m` | Shared MTLDevice (dispatch_once), allPipWindows/visiblePipWindows helpers, closeAllWindows (closes prefs first), arrangeInGrid, arrangeInCascade, max windows enforcement with alert, clone window behavior via preference, Window menu auto-population via setWindowsMenu, applicationShouldTerminateAfterLastWindowClosed→YES |
| `window.m` | PassthroughView class (hitTest→nil), sourceHintOverlay ivar + setup in init (PassthroughView with centered label), hide/show in changeWindow, cleanup in close, cloneSourceToWindow: method |
| `window.h` | Added cloneSourceToWindow: declaration |
| `metalRenderer.m` | Uses getSharedMTLDevice() instead of MTLCreateSystemDefaultDevice() |
| `imageRenderer.h` | Added Metal import and getSharedMTLDevice() declaration |
| `preferences.m` | Two new prefs (new_window_behavior, max_windows), panel height 230→290, auto-quit when prefs close and no PiP windows remain (dispatch_async for safety) |

### Issues found during Codex review and fixed:
1. Overlay blocked right-click events → used PassthroughView with hitTest:→nil
2. pipWindows excluded minimized windows → split into allPipWindows/visiblePipWindows
3. Grid/cascade used keyWindow's screen (could be Preferences) → use first PiP window's screen
4. Overlay was destroyed instead of hidden → changed to hide/show so it reappears
5. getSharedMTLDevice wasn't thread-safe → added dispatch_once
6. closeAllWindows didn't close preferences panel → added explicit close
7. Prefs close could leave app running with no windows → added auto-quit check
8. new_window_behavior pref was unused → implemented clone via cloneSourceToWindow:

### Note on 1.9 (Cmd+` cycling):
Standard macOS Cmd+` window cycling should work automatically now that the Window menu is properly configured with `setWindowsMenu:`. AppKit handles this natively for all windows registered in the windows menu.

---

## Phase 2 Implementation Status

**All Phase 2 items implemented.** Files modified:

| File | Changes |
|---|---|
| `main.m` | "Clonar ventana" menu item (Shift+Cmd+N) with cloneCurrentWindow method, performance warning on 6th+ window (shown once), WindowManagerPanel class (NSPanel with 3-column NSTableView: source name, type, status), "Administrador de ventanas" menu item (Opt+Cmd+M), auto-refresh timer with weakSelf pattern |
| `window.m` | Promoted hintLabel to ivar for dynamic text, showDisconnectOverlay: method (stops captures, shows overlay with disconnect message), handleDisplayDisconnected: for display removal, cameraSessionError: for camera unplug/error, CGDisplayReconfigurationCallback registration/unregistration, AVCaptureSession/Device notification observers in startCameraCapture/stopCameraCapture, SCStream didStopWithError: now shows disconnect overlay, CGDisplayStream callback handles kCGDisplayStreamFrameStatusStopped, sourceType/sourceStatus public methods, cloneSourceToWindow: returns BOOL, forward declaration category for C callback |
| `window.h` | Added sourceType, sourceStatus declarations, changed cloneSourceToWindow: return type to BOOL |

### Issues found during Codex review and fixed:
1. WindowManagerPanel refresh timer created retain cycle → used weakSelf pattern in block
2. CGDisplayReconfigurationCallback uses __bridge (no retain) → safe because close always unregisters before dealloc (NSWindow lifecycle guarantees close before dealloc)
3. Camera notification handler reads camera_session on arbitrary thread → benign pointer check, all state mutation dispatched to main queue
4. showDisconnectOverlay: verified as idempotent — safe to call from multiple error paths simultaneously
