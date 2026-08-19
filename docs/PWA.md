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
./scripts/wasm-build.sh software                    # shipping default
./scripts/wasm-build.sh webgl2 engine/build-webgl2  # retained GPU renderer
```

`scripts/wasm-build.sh` is the same script CI runs, so the two cannot drift.
Build output lands in `engine/build/bin/`.

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
   - loose `.pak` / `.ogg` files
   - a directory selected with the folder picker (where supported)
   - or a `.zip` archive

Use assets from your own copy of Hexen II / Portal of Praevus, for example from GOG or Steam. This project does **not** include game data.

## Fullscreen play

Starting the engine always switches the page from the launcher into a
game-focused surface: the header and import panels disappear, the canvas fills
the window, safe-area insets are honored, page scrolling is disabled while
playing, and resize/rotation/fullscreen events are forwarded to the renderer
through `Web_ResizeCanvas`.

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
the immersive layout too, so the launcher chrome always comes back.

The in-game **☰** button opens a small overlay. It can resume play, send Escape
to the engine menu, **Show launcher** (leave fullscreen but keep playing), or
**Sync & exit to launcher**. The last one first syncs the runtime filesystem to
browser storage and then reloads the page to get a fresh WebAssembly runtime.

Phone mode is now only about panel size: it is detected from the viewport's
*short* side (500 CSS px in either orientation), so a phone is in phone mode in
both orientations and an iPad never is. The previous 820px rule matched iPad
landscape and left iPads permanently in phone mode.

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

Default mappings reuse existing engine input bindings:

- left virtual stick: `W` / `S` forward and back, `A` / `D` strafe left and right
- right look region: relative mouse-look deltas through a small JS-to-C bridge
- **Atk**: primary attack (`MOUSE1`)
- **Jump**: jump (`SPACE`)
- **Use**: lift/use interaction (`K_GP_LTHUMB`, default `impulse 13`)
- **◀ / ▶**: previous/next weapon (`K_GP_LSHOULDER` / `K_GP_RSHOULDER`)
- in-game overlay **Send Esc/Menu**: Escape

The controls use Pointer Events and track each touch by pointer ID, so moving,
looking, and firing can happen at the same time. Held keys/buttons are released
on cancellation, backgrounding, orientation changes, overlay opening, and quit to
avoid stuck movement or fire.

Hardware keyboard, mouse, and physical gamepad support are preserved. On iPad
and desktop, touch controls stay hidden by default unless explicitly enabled.

## Import behavior

Recognized inputs include:

- `pak0.pak`, `pak1.pak`, `pak2.pak`
- `pak3.pak` (mapped to `portals/`)
- files already inside `data1/`, `portals/`, `hw/`, or `music/`
- `.ogg` music files (loose files default to `data1/music/`)

ZIP imports are extracted entirely client-side in the browser. The importer rejects:

- absolute paths
- `..` traversal
- oversized archives / entries beyond the configured safety limits

Re-importing a file with the same logical path overwrites the previous local copy.

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

- **Pointer Lock:** not currently available on iPadOS Safari, so true desktop-style mouselook is limited there
- **Phone look:** iPhone/iOS Safari also lacks Pointer Lock; phone mode feeds relative drag deltas through an explicit engine bridge instead of relying on browser mouse capture
- **Display mode:** installed iPadOS PWAs use `standalone`; this is effectively edge-to-edge but not true unrestricted fullscreen, so the launcher's immersive layout — not the Fullscreen API — is what guarantees a full-window game there
- **Escape key:** iPad keyboards have none, so the web port maps `` ` `` to Escape and moves the console to `Shift`+`` ` ``
- **Restart after Quit:** after the engine's normal Quit, the page returns to the launcher but reloads before starting a new game because the same WebAssembly runtime is not assumed to be restartable
- **Storage persistence:** `navigator.storage.persist()` is requested, but Safari can still evict data under storage pressure
- **Renderer feature gap vs desktop:** WebGL2 / ES 3.0 has no SSBOs, so `r_alias_gpu` remains disabled in WASM builds
- **Import size limits:** large ZIP archives can still hit memory/storage constraints on tablets
- **Audio feature parity:** the documented WASM build path currently disables Vorbis and ALSA-specific paths; browser audio parity still needs broader runtime validation

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

That is expected today. Pointer Lock is the main blocker on iPadOS Safari. External keyboards, mice, and physical controllers remain supported; touch-only devices get explicit touch controls for movement, looking, attack, jump, use, weapon switching, and menu access. The system cursor is hidden during gameplay and shown again in menus and the console, so a visible cursor while playing means the engine still thinks the menu owns input.
