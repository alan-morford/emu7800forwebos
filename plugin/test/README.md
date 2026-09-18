# Headless verification harness

Runs the **exact emulation core that ships on the device** (`m6502.c`, `tia.c`,
`pia.c`, `cart.c`, `maria.c`, `machine.c`, sound) on the host, with no SDL, no
OpenGL and no PDL. A ROM boots in milliseconds and the output frame can be
written to disk, hashed, or diffed against the upstream EMU7800 emulator.

The point is to stop reasoning about whether the math is right and **measure**
it instead.

## Build

```bash
cd plugin && make test          # -> plugin/test/headless
```

## Usage

```
headless <rom.a26|rom.a78> [options]
  -f N          run N frames (default 600)
  -o FILE.ppm   write the final visible frame as a PPM
  -raw FILE     write the full uncropped 320x262 colour-INDEX plane
  -seq DIR      write every frame to DIR/fNNNN.ppm
  -hash         print a per-frame framebuffer CRC32
  -dl           dump the Display List / DLL, walked independently of maria.c
  -report       one machine-readable line: cart type, jam, colours, motion, CRC
  -hold SW      hold a button for the whole run (reset|select|fire|up|down|left|right)
  -press SW:F   press SW at frame F for 10 frames
```

`-dl` is the highest-value debugging tool. It re-walks the DLL/DL structures
straight out of 7800 memory using code written from the hardware docs rather
than from `maria.c`, which splits any graphics fault cleanly in two:

* **Display list looks like garbage** -> the fault is *upstream* of Maria
  (CPU, bank switching, or the cart's RAM is missing) and Maria is faithfully
  drawing bad data.
* **Display list looks sane but the picture is wrong** -> the fault is in the
  rasteriser.

That single distinction is what identified the Summer Games / Winter Games bug
as cart mis-detection (missing SuperGame RAM at $4000) rather than a Maria bug.

## Differential testing against upstream EMU7800

The strongest check available: run the same ROM through the original EMU7800
C# core and compare **colour indices** frame by frame. Comparing indices rather
than RGB keeps the comparison independent of this port's palette choice.

Requires a .NET SDK and an EMU7800 source checkout:

```bash
# one-time
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0
export DOTNET_ROOT=$HOME/.dotnet PATH=$HOME/.dotnet:$PATH
export EMU7800_ROMPROPS=<emu7800>/src/assets/ROMProperties.csv

# generate reference + ours, then diff
dotnet refgen.dll "roms/Game.a78" -f 300 -raw ref.raw
./test/headless  "roms/Game.a78" -f 300 -raw our.raw
python3 diffraw.py our.raw ref.raw
```

### Interpreting the diff

* **Expect a +1 row shift.** Upstream writes line RAM to framebuffer row
  `Scanline + 1`; this port writes to row `Scanline` and compensates in the
  crop (`START_LINE_7800 = 12`). Ours actually shows all 242 visible
  scanlines where upstream's own crop clips the top 3. Not a bug.
* **On the 2600, expect a shift of about +36.** The TIA path re-anchors
  content to row 0 using the VBLANK-off scanline; upstream keeps absolute
  scanlines.
* **Small diffs are usually animation phase.** This port runs a few frames
  ahead of upstream at boot (no 7800 BIOS + the NMI start-up delay). Before
  treating a diff as a bug, re-run the reference at `N-3 .. N+3` frames; if it
  hits exactly 0, the cores agree and only the frame counter differs.

## ROM identification

`src/rom_db.h` is generated, not hand-written:

```bash
make romdb ROMPROPS=<emu7800>/src/assets/ROMProperties.csv
```

Cart bankswitching schemes are frequently **not** derivable from the ROM image.
A78SG and A78SGR are byte-for-byte indistinguishable, but A78SGR has 16KB of
RAM at $4000 that the game builds its display lists in. Guessing here is what
broke Summer Games and Winter Games for a long time. Use the database.
