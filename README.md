This 100% Claude Code vibe-coded project is a port of EMU7800 which, for the first time, brings Atari 7800 gaming to your HP TouchPad and HP Pre3. EMU7800 was developed by Atari enthusiast Mike Murphy. Originally released around 2003, the emulator is an open-source project (GNU GPLv2) designed for Windows and later adapted for other platforms, with active development continuing on GitHub. Portions of this port of EMU7800 also utilized open-source code from Stella version 7.0 (released in Oct 2024). Stella was originally developed for Linux by Bradford W. Mott and is now maintained and developed by Stephen Anthony and the Stella Team.

NOTE: The Pre3 performance isn't great, however, it is drastically improved with Uberkernel and Govnah installed and configured to overclock the Pre3 to 1.9GHz.

## Features

### Emulation
- Atari 2600 and 7800 ProSystem emulation
- Starpath Supercharger ROM support
- ZIP-compressed ROM support (.zip)
- Asteroids for the Atari 7800 included — just like the European release!

### Controls
- On-screen multitouch controls: D-pad, one and two fire button layouts
- Resizable controls: Small, Medium, and Large presets
- Adjustable control overlay brightness
- Physical keyboard support (auto-detected; shows button labels when active)
- Paddle controller support with Slider or D-pad input mode
- Wired USB game controller support (**TouchPad only** — no USB host/controller
  support on Pre3), matching the Android port's button map: B/Y = Fire 1,
  A/X = Fire 2, D-Pad/L Stick = Joystick or Paddle, LT = Select, RT = Reset,
  Start = Pause, Select = Back, LB = Save State, RB = Load State. Also
  navigates the ROM list (D-Pad + A/B). See it any time from the in-game
  Options menu's "Controller Buttons" → MAP. **Requires installing the .ipk
  via Preware or WebOS Quick Install**, not `palm-install` — a one-time
  root maintainer script exposes `/dev/input` inside the app's jail, which
  `palm-install`'s unprivileged install never runs. Everything else about
  the app installs and runs the same either way.
- "Control Visibility" in the in-game Options menu cycles BRIGHT → DIM →
  DIMMER (**+ OFF on TouchPad only**) — OFF hides just the
  D-pad/paddle-slider/Fire/Fire2 (for use with a controller);
  BACK/PAUSE/SAVE/LOAD/ZOOM/OPTIONS/RESET/SELECT stay visible at DIMMER's
  brightness no matter what. Touch regions stay active either way, so
  tapping where a hidden button used to be still works. Pre3 has no OFF
  step, since it has no controller to fall back on.

### Display
- Multiple video modes: Original Aspect Ratio, 2X, 3X integer scaling (TouchPad), and Fullscreen
- Scanline overlay: Off, Light, Medium, Dark
- 7800 color palette selection: Cool, Warm, Hot (Trebor A7800 NTSC LCD variants)

### File Management
- Built-in file picker for browsing and launching ROMs (.a26, .a78, .bin, .zip)
- Set and remember a default ROM directory
- Resume last played ROM from the file picker
- Recently played list for quick access (scrollable on Pre3)

### Save States
- Save, load, and delete save states per ROM
- Auto-save on close (optional, with ask-before-save confirmation)
- Resume directly from a save state on launch

### Launcher Shortcuts (TouchPad only)
- Add any game to the webOS launcher from the in-game Options menu
- Tapping the launcher icon cold-starts the emulator and immediately launches that game
- Auto-loads the save state if one exists

### App
- In-game Options menu with all settings accessible during gameplay
- Update checker — fetches latest version from App Museum II and updates from within the app
- Bug report link
- HP TouchPad (1024x768, OpenGL ES 1.1) and HP Pre3 (800x480, software rendering) both supported

## Known Issues

- **Pre3/Veer/Pre2 (webOS 2.2.4) with the OpenSSL-legacyWebOS TLS 1.3 patch installed**:
  the app icon does nothing on tap. This affects *every* native PDK app on the device, not
  just EMU7800 — it's a bug in that patch's webOS 2.x build (confirmed on-device: it never
  even reaches the point of spawning a process). Not something this app can work around.
  Tracked upstream at
  [Herrie82/OpenSSL-legacyWebOS#5](https://github.com/Herrie82/OpenSSL-legacyWebOS/issues/5).
  HP TouchPad (webOS 3.0.5/LunaCE) is unaffected — that patch's build already has an
  equivalent fix for TouchPad.
