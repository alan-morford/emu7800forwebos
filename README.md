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

### Launcher Shortcuts
- Add any game to the webOS launcher from the in-game Options menu
- Tapping the launcher icon cold-starts the emulator and immediately launches that game
- Auto-loads the save state if one exists

### App
- In-game Options menu with all settings accessible during gameplay
- Update checker — fetches latest version from App Museum II and updates from within the app
- Bug report link
- HP TouchPad (1024x768, OpenGL ES 1.1) and HP Pre3 (800x480, software rendering) both supported
