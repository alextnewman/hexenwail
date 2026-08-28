# Hexenwail PWA / GitHub Pages build

This repository now includes a GitHub-Pages-deployable, installable PWA shell for the WebAssembly build of Hexenwail, including fullscreen play and a rudimentary phone mode for iPhone/iOS Safari.

## Goals

- same-origin deployment (no CDN/runtime fetches)
- installable on iPadOS/iOS via **Add to Home Screen**
- fully offline play after the first online load plus local asset import
- no cross-origin isolation requirement (single-threaded WASM; no pthreads / SharedArrayBuffer)

## Current architecture

- **Engine build:** Emscripten + SDL3 + WebGL2 / ES 3.0
- **PWA shell:** `web/index.html`, `web/app.js`, `web/sw.js`, `web/manifest.webmanifest`
- **Persistent asset storage:** OPFS first, IndexedDB fallback
- **Service worker:** caches launcher/runtime assets only; never caches user-imported game data
- **Base runtime path:** engine launches with `-basedir /persistent`

User-imported assets are mirrored into the runtime filesystem before `main()` is called.

## Local build

The build requires the Emscripten SDK. CI pins `4.0.23` — see
`.github/workflows/ci.yml`; do not use `latest`, since the pin is what keeps
local and CI builds comparable.

### Option A: Nix dev shell

```bash
nix develop          # web toolchain: emscripten, cmake, node
./scripts/wasm-build.sh software
```

Note that the nixpkgs Emscripten is not the pinned emsdk CI uses.

### Option B: direct emsdk install (what CI does)

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 4.0.23
./emsdk activate 4.0.23
source ./emsdk_env.sh

cd /path/to/hexenwail
./scripts/wasm-build.sh nitro engine/build-nitro   # primary renderer
./scripts/wasm-build.sh software                    # parked reference
./scripts/wasm-build.sh webgl2 engine/build-webgl2  # retained GPU renderer
```

`scripts/wasm-build.sh` is the same script CI runs, so the two cannot drift.
Primary build output lands in `engine/build-nitro/bin/`.

## Assemble a local PWA site

Copy the PWA shell plus engine artifacts into one directory:

```bash
mkdir -p dist
cp -r web/. dist/
cp engine/build/bin/hexenwail.js dist/
cp engine/build/bin/hexenwail.wasm dist/
cp engine/build/bin/hexenwail.html dist/engine-shell-debug.html
```

If Emscripten emits optional extra files such as `hexenwail.data` or `hexenwail.worker.js`, copy them too.

## Local testing

Service workers require HTTPS or `localhost`. `localhost` is allowed, so a local static server is sufficient:

```bash
cd dist
python3 -m http.server 8000
```

Then open <http://localhost:8000/>.

Recommended checks:

1. open once while online
2. import `pak0.pak` and `pak1.pak`
3. reload
4. disconnect networking / use airplane mode
5. reload again and verify the shell plus imported assets still work

## GitHub Pages deployment

A workflow is provided at `.github/workflows/pages.yml`.

### One-time repository setting

GitHub Pages **must** be configured manually in the repository settings:

- **Settings → Pages → Source → GitHub Actions**

The workflow YAML cannot flip that repository setting by itself.

### Workflow behavior

The workflow:

1. checks out the repo
2. installs emsdk directly on the Ubuntu runner
3. runs the lightweight Node-based PWA tests
4. builds the WASM client with `emcmake` / `emmake`
5. assembles a `dist/` site
6. deploys it with the supported Pages actions

All launcher URLs are relative (`./...`) so the site works under project Pages paths such as:

- `https://<user>.github.io/hexenwail/`

## Installing on iPadOS / iOS

1. Open the deployed site in Safari.
2. Wait for the first online load to finish.
3. Use **Share → Add to Home Screen**.
4. Launch the installed app.
5. Import your legally acquired Hexen II assets:
   - loose `.pak` files and loose music (`.ogg`, `.mp3`, `.flac`, `.wav`)
   - a directory selected with the folder picker (where supported)
   - or a `.zip` archive

Use assets from your own copy of Hexen II / Portal of Praevus, for example from GOG or Steam. This project does **not** include game data.

## Fullscreen play

Starting the engine always switches the page from the launcher into a
game-focused surface: the header and import panels disappear, the canvas fills
the window, page scrolling is disabled while playing, and
resize/rotation/fullscreen events are forwarded to the renderer
through `Web_ResizeCanvas`.

Play is **edge to edge**. An installed iOS PWA cannot hide the status bar, so
padding the window with the safe-area insets would only trade game pixels for
black bars; instead the canvas takes the whole window and the system glyphs
sit over the outer edge of the picture — a game authored for a 320 × 200-ish
screen with a border can afford that, wasted screen space it cannot. The
renderer completes the job from its side: the software backend takes the
canvas aspect for its render resolution rather than pillarboxing 4:3 (see
[`web/SOFTWARE_RENDERER.md`](web/SOFTWARE_RENDERER.md#aspect-policy)).

Everything interactive keeps its own safe-area inset, so the ☰ button, the
touch controls and the in-game overlay never hide under the status bar, the
home indicator or a rounded corner.

Two layers are involved, and they are deliberately independent:

- **Immersive layout** (`<body data-immersive="true">`) is pure launcher CSS. It
  always works, including in an installed iOS PWA where the Fullscreen API is
  missing or refused.
- **Native fullscreen** is requested on top of it, from the same click that
  starts the game, so the browser sees a valid user gesture. The fullscreen
  element is the game *surface* (`.viewport`), not the bare canvas, so the touch
  controls and the in-game overlay come along with it.

Because both layers are driven by the launch action, the launcher can no longer
end up fullscreen without a game (the old **Request fullscreen** button did
exactly that: an empty black fullscreen canvas). The **Fullscreen play** button
is enabled only while the engine is running and toggles both layers together;
leaving fullscreen through the browser (desktop Esc, iPad system gesture) drops
the immersive layout too, so the launcher chrome comes back (except in phone
mode, below, where there is no room for it).

The in-game **☰** button opens and closes the engine menu directly, just like
the on-screen **Menu** control. The menu replaces the gameplay controls with
its own navigation pad.

**☰ hides while a controller is connected.** A gamepad already has a menu
button — Start always reaches the engine menu — so leaving a launcher
glyph floating over the game would be pure clutter. It comes back on
disconnect. The button is driven by `<body data-gamepad>`, which is set from
the `gamepadconnected` / `gamepaddisconnected` events (Safari only lists a pad
through `navigator.getGamepads()` once it has sent input, so the events are
the authoritative signal and the poll is the fallback for a page loaded with
one already attached).

Phone mode is only about panel size: it is detected from the viewport's *short*
side (500 CSS px in either orientation), so a phone is in phone mode in both
orientations and an iPad never is. The previous 820px rule matched iPad
landscape and left iPads permanently in phone mode. Pointer capability is
deliberately not part of this test — an attached mouse does not make a phone
panel any bigger, and a narrow iPad Split View column is as cramped as a phone
whether or not a trackpad is present.

Phone mode pins the immersive layout and hides **Show launcher**, because a
panel that small has no chrome worth returning to; ☰ → **Sync & exit to
launcher** still works. Being a pure function of size, this is self-healing:
rotate or resize back above the breakpoint and the chrome returns on its own.

## Renderer

The launcher ships several WebAssembly engine bundles side by side and picks
which one to load at startup. The choice lives in the **Renderer** card in the
launcher panel:

- **WebGlideNitro (default)** — the primary native WebGPU renderer. It builds
  batched scene geometry while preserving indexed palette and colormap
  semantics, and implements the complete playable scene plus dynamic lights,
  fog, warped/translucent liquids, fullbrights, glows and projected shadows.
  Build option value `webgpu`, macro `WEBGPUQUAKE`, bundle
  `hexenwail-nitro.*`.
- **Software (parked reference)** — the classic 8bpp software rasteriser
  presented on an accelerated canvas. It remains available as the exact
  authored-pixel reference but is no longer the primary path.
- **WebGlide (experimental GPU)** — an abortive experiment at a GPU renderer
  chasing the mid-90s 3Dfx look — filtered textures, coloured light,
  translucent water, fog, optional CRT scan-out — with modern shaders
  under it. It is kept building but is not actively pursued, and it is
  **off by default**: it may render incorrectly, produce
  visual glitches, or fail to start entirely. Use only if you are actively
  trying it out. WebGlide is deliberately distinct from any previous
  "maximum GL2" WebGL2 profile — the shared build option value stays
  `webgl2`, but the shipped bundle basename (`hexenwail-webglide.*`) and
  the user-facing renderer name are WebGlide.

The engine bundle is chosen once per launcher load, so a change reloads
the launcher automatically when no game is running; if the engine is
already running, the change is queued and takes effect on the next
launcher load (exit to the launcher via ☰ → **Sync & exit to launcher**).
The preference is stored in the same local browser storage as the touch
controls settings, alongside your other launcher preferences.

If a selected parked bundle is missing from the artifact, the launcher
fails loudly on load, names the toggle, and explains how to switch back to
the software renderer so the launcher never ends up dead. To ship the
other bundles locally, build those configurations explicitly before
assembling `dist/`:

```bash
./scripts/wasm-build.sh nitro engine/build-nitro   # primary renderer
./scripts/wasm-build.sh software                    # parked reference
./scripts/wasm-build.sh webgl2 engine/build-webgl2  # WebGlide bundle
./scripts/wasm-assemble-artifact.sh dist
```

CI builds every configuration on every run, so a deployed Pages artifact
always ships all the bundles regardless of the launcher's default.

## Performance capture

Both renderers carry the same raw frame capture, off by default. Enable it in
the launcher, play for at least 128 frames, then use **Show launcher** and
**Copy latest report**. It collects frame intervals, full host CPU time,
rendering stages and renderer counters without drawing an in-game overlay.
The copied text also contains browser metadata and the bounded runtime log.
See
[`web/PERF_CAPTURE.md`](web/PERF_CAPTURE.md).

## Keyboard

`in_web.c` owns the browser keyboard mapping. Two web-specific rules:

- **`` ` `` acts as Escape.** iPad keyboards (Magic Keyboard, Smart Keyboard
  Folio) have no Escape key, and Hexen II's menus are built entirely around it.
- **`Shift`+`` ` `` (i.e. `~`) toggles the console**, which is the classic second
  console key and is reachable on every keyboard.

Set `in_key_backquote_escape 0` to restore the classic behaviour where both
`` ` `` and `~` toggle the console.

The engine also owns cursor visibility, because there is no window manager and
no Pointer Lock on iPadOS Safari: `IN_Commands` hides the browser cursor while
`key_dest` is `key_game` and shows it again in the menu or console.

Selecting **Quit** inside Hexen II uses a browser-specific Emscripten path: the
engine performs its normal shutdown, cancels the browser main loop, notifies the
launcher, and the launcher syncs saves before returning to a stopped launcher
state. The current WASM runtime is treated as not safely restartable after this
shutdown, so the launcher shows **Restart game**, which performs the same
sync-then-reload flow. Fatal engine errors are reported separately and also ask
for a clean restart.

### Touch mappings

In auto mode, touch controls are shown when the launcher believes the device is
touch-only: a coarse pointer with no hover/fine pointer and no connected
gamepad. Screen size is deliberately not part of that test — a bare iPad is as
touch-only as a phone, and an iPad on a Magic Keyboard is not touch-only at all.
If a touchpad/mouse, physical keyboard activity, pen, wheel, or controller is
detected, auto mode hides the overlay and releases all held touch inputs. The
launcher setting can choose auto/on/off behavior, left- or right-handed layout,
and look sensitivity; **Always show** displays the controls on any running
device.
Preferences are stored in local browser storage, separate from imported game
assets and saves.

The touch overlay uses action labels rather than controller-face letters, so
every visible label describes what that control actually does:

- left virtual stick: `W` / `S` forward and back, `A` / `D` strafe left and right; while a menu is open it becomes the menu D-pad so the touch stick can navigate lists and submenus
- right look region: relative mouse-look deltas through a small JS-to-C bridge
- **Atk**: primary attack (`K_GP_RTRIGGER`)
- **Jump**: jump (`K_GP_A`)
- **Use**: lift/use interaction (`K_GP_LTHUMB`, default `impulse 13`)
- **◀ / ▶**: previous/next weapon (`K_GP_LSHOULDER` / `K_GP_RSHOULDER`)
- **Menu**: open the engine menu (`K_GP_START`)
- in-game overlay **Send Esc/Menu**: Escape

When the engine menu opens, the gameplay controls are replaced with a smaller,
explicitly labelled menu pad: the stick navigates, **Select** confirms,
**Back** moves out one screen, and **Resume** closes the menu outright. The
engine notifies the launcher when menu ownership changes; that transition also
releases every held touch input so movement or attack cannot stick across it.

The controls use Pointer Events and track each touch by pointer ID, so moving,
looking, and firing can happen at the same time. Held keys/buttons are released
on cancellation, backgrounding, orientation changes, overlay opening, and quit to
avoid stuck movement or fire.

The play surface disables browser touch gestures in CSS, carries a fixed-scale
viewport declaration in both launcher HTML paths, and consumes `dblclick` while
the engine is running so a double tap cannot zoom the game view.

Hardware keyboard, mouse, and physical gamepad support are preserved. On iPad
and desktop, touch controls stay hidden by default unless explicitly enabled.

## Gamepad

`in_web.c` contains a first-party gamepad driver written directly against the
browser's Gamepad API (sampled through Emscripten's HTML5 bindings). There is no
SDL joystick layer in the web build to translate, and pointer input is not
practically playable on iPadOS, so the controller is the primary way to play on a
tablet.

Two web-specific facts shape the driver:

- **The Gamepad API is poll-only.** There are no button events, so
  `IN_Commands` samples the pad once per host frame and turns level state into
  the engine's edge-triggered `Key_Event` calls itself.
- **The browser already normalises the layout.** Controllers report the W3C
  "standard gamepad" mapping — a fixed button and axis order — so the mapping is
  a plain index table rather than a controller database. Pads reporting the
  standard mapping are preferred when several are connected.

Buttons produce the engine's existing `K_GP_*` keycodes, so the default binds and
the **Controller Options** menu apply unchanged:

| Physical | Key | Default bind |
| --- | --- | --- |
| A / B / X / Y | `K_GP_A` … `K_GP_Y` | jump, crouch, use artifact, jump |
| L1 / R1 | `K_GP_LSHOULDER` / `K_GP_RSHOULDER` | previous / next weapon |
| L2 / R2 (analog) | `K_GP_LTRIGGER` / `K_GP_RTRIGGER` | jump / attack |
| L3 / R3 | `K_GP_LTHUMB` / `K_GP_RTHUMB` | lift object / `+altmodifier` |
| Start / Back | `K_GP_START` / `K_GP_BACK` | menu (reserved) / console |
| D-pad ◀ ▶ | `K_GP_DPAD_LEFT` / `K_GP_DPAD_RIGHT` | inventory left / right |

Triggers are analog buttons in the standard mapping; they fire once they pass
`joy_deadzone_trigger`. The sticks use a circular deadzone plus a power curve
(`joy_deadzone_move` / `joy_deadzone_look`, `joy_exponent_move` / `joy_exponent`)
and feed movement and view angles from `IN_Move`. `joy_swapmovelook`,
`joy_invert`, `joy_sensitivity_yaw` and `joy_sensitivity_pitch` behave as usual.

Because a cursor is not a realistic input on iPadOS, the pad has to be able to
drive the whole UI: in menus and the console the D-pad and move stick act as
arrow keys with auto-repeat, the triggers act as Enter, and (via the shared menu
code) A confirms while B backs out. Held buttons remember which key they emitted,
so nothing sticks down across a menu transition, a disconnect, or `gamepad 0`.

### Console-style menu navigation

The pad drives the menus the way a console pause menu behaves:

| Button | In the menu |
| --- | --- |
| Start | closes the menu outright, from any screen, and reopens it from gameplay or the console |
| A / L2 / R2 | confirm (Enter) |
| B | back one screen (Escape) |
| D-pad / move stick | move the cursor, with auto-repeat when held |
| L1 / R1 | page up / page down on the scrolling screens (the mods list) |

Start is deliberately not a "back" key: Escape and B unwind one screen at a
time, while Start toggles the whole menu, so a second press resumes play no
matter how deep the player is.

It is also the one gamepad button that is *not* dispatched through the binding
table. `Key_Event` handles it the way it handles Escape, and it is a reserved
key, so neither a rebind nor the `unbindall` that heads every `config.cfg` can
take it away. That matters because a controller has no Escape key and `~` needs
a keyboard: if Start ever failed to reach the menu — most obviously with the
console up, where a plain `togglemenu` merely closes the console again — a
controller-only player would have no way out. Pressing Start in the console
therefore leaves the console and opens the menu, and pressing it on the
**Customize controls** bind prompt cancels the prompt instead of recording a
binding that could never fire.

Rumble uses `vibrationActuator.playEffect` when the browser has it, scaled by
`joy_rumble`. Safari does not implement it today, so it is best-effort and silent
when missing.

Set `gamepad 0` (or **Controller Options → Gamepad Enabled**) to disable polling
entirely. Browsers only expose a pad after it has been used once, so the first
button press is what makes the controller appear — that press is consumed by the
browser, not the game.

## Import behavior

Recognized inputs include:

- `pak0.pak`, `pak1.pak`, `pak2.pak`
- `pak3.pak` (mapped to `portals/`)
- files already inside `data1/`, `portals/`, `hw/`, or `music/`
- music files the engine can decode — `.ogg`, `.mp3`, `.flac`, `.wav` (loose files default to `data1/music/`)

ZIP imports are extracted entirely client-side in the browser. The importer rejects:

- absolute paths
- `..` traversal
- oversized archives / entries beyond the configured safety limits

Re-importing a file with the same logical path overwrites the previous local copy.

## Music

Hexen II has no CD audio and no MIDI synthesiser in the browser, so *all* music
in the PWA comes from external music files you import. The client decodes Ogg
Vorbis, MP3, FLAC and WAV.

The filename is what selects the track, and it must match the name the map asks
for — which is the **MIDI name**, not the CD track number. `casa1.ogg` plays;
`track02.ogg` does not, even though it is the same recording. `docs/README.music`
lists the full track-number → MIDI-name mapping for both Hexen II and Portal of
Praevus, so rename the files before (or after) importing them. This is the single
most common reason imported music appears to do nothing, and it looks exactly
like a codec bug.

`bgm_remap` does **not** rescue ripped `track%02d` filenames: it works the other
way round, pointing a *numeric* track request at a named file, for the mods and
custom maps that set a `CD` worldspawn key instead of a `MIDI` one.

```
bgm_remap 18 myambient
bgm_remap list
```

To confirm what the engine actually has, open the console (`Shift`+`` ` ``) and
look at the startup lines:

```
BGM: MIDI driver: none (midi-named music will not play)
BGM: stream codecs: ogg mp3 flac wav
```

Then `music casa1` plays a track directly. If the codec list is missing a format
you expected, that is a build problem; if the codecs are listed but a track stays
silent, it is almost always the filename.

## Save game persistence

Local save games are just as important to keep offline as the imported PAK/OGG assets, so the
launcher treats the whole runtime data directory (`/persistent`, mounted at the engine's
`-basedir`) as a single persisted tree — not only the files you explicitly imported:

- the engine writes save games (`.hsv`, `clients.gip`, etc.) under the same `data1/`
  tree as the imported PAK files, via its normal `save`/`load` console commands
- the launcher periodically diffs the in-memory runtime filesystem against persistent
  browser storage (every ~10 seconds during play) and writes back anything new or changed,
  including save games created since the last sync
- an additional sync is forced when the tab is hidden/backgrounded (`visibilitychange`),
  when the page is about to be unloaded or bfcache-frozen (`pagehide`, `beforeunload`,
  `freeze`), and immediately after any asset import
- on the next launch (online or fully offline), the persisted tree — imported assets and
  save games alike — is restored into the runtime filesystem before `main()` is called, so
  saved games survive reloads, app restarts, and offline play sessions
- **"Clear imported data & saves"** intentionally clears the entire persisted tree, including
  save games, since they share the same storage; the in-app confirmation dialog says so
  explicitly before it destroys anything

Caveats:

- if the tab/app is killed abruptly (e.g. the OS force-quits it under memory pressure)
  before a sync completes, save-game writes since the last sync point could be lost — quit
  normally (switch away or close the tab) rather than force-killing when possible
- browser storage eviction under storage pressure (see below) can still remove save games
  along with everything else; requesting persistent storage via the Storage panel reduces
  but does not eliminate this risk

## Portable save bundles

The launcher’s **Saves & Backup** section can move save games between devices without
an account, upload, cloud service, or backend:

1. After saving, return to the launcher and choose **Export saves**. The launcher first
   completes its runtime-to-browser-storage sync.
2. Use the iPadOS Share sheet to save the dated `.hexenwail-save.zip` file to **Files** or
   **iCloud Drive** (desktop browsers download the same file).
3. On the other device, import your legally acquired matching game data first, then choose
   **Import saves** and select the bundle from Files/iCloud Drive.
4. Review the date, game directories, file count, size, and any compatibility warning,
   then confirm the import. Reload before loading an imported save if the engine is already
   running.

Bundles are ordinary ZIP files with a versioned `hexenwail-save.json` manifest and save
files under `saves/data1/`, `saves/portals/`, or `saves/hw/`. They contain only recognized
engine save-slot files (such as `s0/info.dat` and `.gip` state), never PAKs, OGG music,
runtime binaries, caches, or other imported commercial game data. The manifest records
file SHA-256 hashes and sizes and records local PAK paths, sizes, and hashes for comparison
only; it does not embed PAKs.

Choose **Merge saves** to replace only bundle paths that already exist, or **Replace saves**
to delete existing recognized save files before adding the bundle. Neither mode deletes PAKs,
music, or other assets. The importer verifies the manifest, ZIP safety limits, paths,
duplicates, file sizes, and SHA-256 hashes before writing anything. It rejects unsupported
future formats and unsafe, unexpected, or undeclared ZIP entries. If persistent storage
cannot complete a write, affected save files are restored from a local rollback snapshot.

Compatibility warnings mean that required base-game or expansion PAKs are absent or differ
from the exporting device. They do not put commercial data in the bundle: import matching
legal assets separately. A bundle can be exported and imported entirely offline; browser
storage and iCloud Drive/Files behavior remain subject to their own available space and
sync timing. Because abrupt app termination can interrupt the normal ten-second save sync,
export from the launcher after switching away from play rather than relying on a force-quit
to preserve the most recent save.

## Offline readiness

The service worker caches the launcher shell and WASM runtime files. Imported user assets live separately in OPFS / IndexedDB and are not part of the service worker cache.

Practical ways to confirm readiness:

- the in-app offline status text reports when the service worker has taken control
- reload once after installation/import
- then test in airplane mode
- on macOS, Safari Web Inspector can also verify the service worker and storage state remotely from an attached iPad

## Browser / iPadOS limitations

- **Pointer Lock:** not currently available on iPadOS Safari, so true desktop-style mouselook is limited there; a game controller is the recommended input on iPad
- **Gamepad discovery:** browsers only expose a controller after it has been used at least once, so the first button press wakes the pad up instead of reaching the game
- **Rumble:** `vibrationActuator` is not implemented by Safari, so `joy_rumble` has no effect on iPadOS
- **Phone look:** iPhone/iOS Safari also lacks Pointer Lock; phone mode feeds relative drag deltas through an explicit engine bridge instead of relying on browser mouse capture
- **Display mode:** installed iPadOS PWAs use `standalone`; this is effectively edge-to-edge but not true unrestricted fullscreen, so the launcher's immersive layout — not the Fullscreen API — is what guarantees a full-window game there
- **Escape key:** iPad keyboards have none, so the web port maps `` ` `` to Escape and moves the console to `Shift`+`` ` ``
- **Restart after Quit:** after the engine's normal Quit, the page returns to the launcher but reloads before starting a new game because the same WebAssembly runtime is not assumed to be restartable
- **Storage persistence:** `navigator.storage.persist()` is requested, but Safari can still evict data under storage pressure
- **Renderer feature gap vs desktop:** WebGL2 / ES 3.0 has no SSBOs, so `r_alias_gpu` remains disabled in WASM builds
- **Import size limits:** large ZIP archives can still hit memory/storage constraints on tablets
- **Music codec set:** Ogg Vorbis, MP3, FLAC and WAV decode in the client; Opus (`opusfile`) and the tracker formats (`libxmp`) are absent because neither has an Emscripten port
- **MIDI:** there is no MIDI synthesiser in the web build, so a PAK-only install has no music until external music files are imported

## Troubleshooting

### Launcher says “running” but the canvas stays black

Use the **Runtime log** card in the launcher. It keeps the latest engine startup
status plus stdout and stderr in the page, including failures that would otherwise
only be visible in browser developer tools.

### “Unable to find a proper Hexen II installation”

You have not imported a valid registered Hexen II data set yet. Import at least:

- `data1/pak0.pak`
- `data1/pak1.pak`

### Service worker updates seem stuck

Reload once while online so the new shell version can install, then reload again to let the updated worker control the page.

### Browser storage problems

Use **Clear imported data** in the launcher, then re-import the assets. If Safari has evicted storage, you will need to import again.

### ZIP import failed

The archive may contain unsupported paths, exceed the configured resource caps, or contain formats outside the recognized Hexen II layout.

### Mouse capture is incomplete on iPad/iPhone

That is expected today. Pointer Lock is the main blocker on iPadOS Safari, which
is why a game controller is the recommended way to play there — see
[Gamepad](#gamepad). External keyboards, mice, and physical controllers remain
supported; touch-only devices get explicit touch controls for movement, looking,
attack, jump, use, weapon switching, and menu access. The system cursor is hidden
during gameplay and shown again in menus and the console, so a visible cursor
while playing means the engine still thinks the menu owns input.

### The controller does nothing

Press a button on the pad first: browsers deliberately hide gamepads until they
have been used, and that first press is consumed by the browser. If it still does
nothing, open the console (Back button, or `Shift`+`` ` ``) and check for the
`Gamepad connected` line, and that `gamepad` is `1`.
