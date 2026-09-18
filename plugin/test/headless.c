/*
 * headless.c
 *
 * Headless verification harness for the EMU7800 core.
 *
 * Links ONLY the emulation core (m6502/tia/pia/maria/cart/machine/sound) --
 * no SDL, no OpenGL, no PDL -- so the exact code that runs on the device can
 * be executed, inspected and diffed on the host in milliseconds.
 *
 * Usage:
 *   headless <rom> [options]
 *     -f N          run N frames (default 600)
 *     -o FILE.ppm   write final frame as PPM
 *     -seq DIR      write every frame as DIR/fNNNN.ppm
 *     -hash         print per-frame framebuffer CRC32
 *     -trace N      print a CPU trace of the first N instructions
 *     -cputest      run as a bare 64K 6502 test image (Klaus Dormann suite)
 *     -hold SW      hold a console switch/button for the whole run
 *                   (reset|select|fire|up|down|left|right)
 *     -press SW:F   press SW on frame F for 10 frames
 *
 * Copyright (c) 2026 EMU7800
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "m6502.h"
#include "tia.h"
#include "maria.h"

/* The core references log_msg(); route it to stderr when EMU_VERBOSE is set. */
void log_msg(const char *msg)
{
    static int checked = 0, on = 0;
    if (!checked) { checked = 1; on = getenv("EMU_VERBOSE") != NULL; }
    if (on) fprintf(stderr, "[log] %s\n", msg);
}

/* ---- framebuffer geometry, mirroring video.c exactly ---- */
#define FB_WIDTH_2600   160
#define FB_WIDTH_7800   320
#define FB_HEIGHT       262
#define VISIBLE_7800    223   /* NTSC TV-visible window; see video.c */
#define START_LINE_7800 22
#define CRT_DISPLAY_START_SL 31
#define CRT_MAX_DISPLAY_H    210

static uint32_t crc_table[256];
static void crc_init(void)
{
    uint32_t c; int n, k;
    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc32_buf(const unsigned char *b, size_t len)
{
    uint32_t c = 0xffffffffu; size_t i;
    for (i = 0; i < len; i++) c = crc_table[(c ^ b[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/*
 * Resolve the visible window exactly the way video.c does, so a PPM written
 * here is what the device would put on screen (modulo scaling).
 */
static int visible_window(int *out_w, int *out_h, int *out_x0, int *out_y0)
{
    if (machine_get_type() == MACHINE_7800) {
        *out_w = FB_WIDTH_7800; *out_h = VISIBLE_7800;
        *out_x0 = 0; *out_y0 = START_LINE_7800;
        return 1;
    } else {
        int active_height = tia_get_active_height();
        int vbo_sl, display_offset, display_height;
        if (active_height < 1) active_height = 192;
        if (active_height > 256) active_height = 256;

        vbo_sl = tia_get_vblank_off_scanline();
        display_offset = 0;
        if (vbo_sl >= 0 && vbo_sl < CRT_DISPLAY_START_SL) {
            display_offset = CRT_DISPLAY_START_SL - vbo_sl;
            if (display_offset >= active_height) display_offset = 0;
        }
        display_height = active_height - display_offset;
        if (display_offset > 0 && display_height > CRT_MAX_DISPLAY_H)
            display_height = CRT_MAX_DISPLAY_H;
        if (display_height < 1) display_height = 192;
        if (display_height > 256) display_height = 256;

        *out_w = FB_WIDTH_2600; *out_h = display_height;
        *out_x0 = 0; *out_y0 = display_offset;
        return 1;
    }
}

static int write_ppm(const char *path)
{
    uint8_t *fb = machine_get_frame_buffer();
    const uint32_t *pal;
    int w, h, x0, y0, x, y, fbw;
    FILE *fp;

    if (!fb) return 0;
    visible_window(&w, &h, &x0, &y0);
    fbw = (machine_get_type() == MACHINE_7800) ? FB_WIDTH_7800 : FB_WIDTH_2600;
    pal = (machine_get_type() == MACHINE_7800) ? maria_ntsc_palette : tia_ntsc_palette;

    fp = fopen(path, "wb");
    if (!fp) { perror(path); return 0; }
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        const uint8_t *src = fb + (y + y0) * fbw + x0;
        for (x = 0; x < w; x++) {
            uint32_t c = pal[src[x]];
            fputc((c >> 16) & 0xff, fp);
            fputc((c >> 8) & 0xff, fp);
            fputc(c & 0xff, fp);
        }
    }
    fclose(fp);
    return 1;
}

static uint32_t frame_hash(void)
{
    uint8_t *fb = machine_get_frame_buffer();
    int w, h, x0, y0, fbw;
    if (!fb) return 0;
    visible_window(&w, &h, &x0, &y0);
    fbw = (machine_get_type() == MACHINE_7800) ? FB_WIDTH_7800 : FB_WIDTH_2600;
    {
        uint32_t c = 0xffffffffu; int x, y; size_t i;
        for (y = 0; y < h; y++) {
            const uint8_t *src = fb + (y + y0) * fbw + x0;
            for (x = 0; x < w; x++) c = crc_table[(c ^ src[x]) & 0xff] ^ (c >> 8);
        }
        (void)i;
        return c ^ 0xffffffffu;
    }
}

/*
 * Independent Display List walker.
 *
 * Re-walks the DLL/DL structures straight out of 7800 memory using an
 * implementation written from the hardware docs, NOT from maria.c. If this
 * agrees with what Maria drew, the display list is sound and any corruption
 * is in the rasteriser. If it disagrees, the fault is upstream (CPU, banking
 * or RAM) and Maria is faithfully drawing bad data.
 */
#define R_BACKGRND 0x20
#define R_DPPH     0x2c
#define R_DPPL     0x30
#define R_CHARBASE 0x34
#define R_CTRL     0x3c

static void dump_display_list(int max_zones)
{
    MariaInternalState st;
    uint16_t dll, dl;
    int zone, rm, cwidth, total_lines = 0;

    maria_get_internal_state(&st);
    rm     = st.registers[R_CTRL] & 0x03;
    cwidth = (st.registers[R_CTRL] & 0x10) != 0;

    printf("CTRL=$%02X  rm=%d cwidth=%d dma=%s  CHARBASE=$%02X  BACKGRND=$%02X\n",
           st.registers[R_CTRL], rm, cwidth,
           ((st.registers[R_CTRL] & 0x60) == 0x40) ? "on" : "OFF",
           st.registers[R_CHARBASE], st.registers[R_BACKGRND]);

    dll = (uint16_t)(st.registers[R_DPPL] | (st.registers[R_DPPH] << 8));
    printf("DPP=$%04X\n", dll);

    for (zone = 0; zone < max_zones; zone++) {
        uint8_t d0 = machine_peek_bus(dll);
        uint8_t d1 = machine_peek_bus((uint16_t)(dll + 1));
        uint8_t d2 = machine_peek_bus((uint16_t)(dll + 2));
        int dli   = (d0 & 0x80) != 0;
        int holey = (d0 & 0x60) >> 5;
        int off   = d0 & 0x0f;
        int entry;

        dl = (uint16_t)(d2 | (d1 << 8));
        printf("\nzone %2d @DLL $%04X [%02X %02X %02X]  DL=$%04X off=%d (%d lines) "
               "holey=%d dli=%d  [rows %d..%d]\n",
               zone, dll, d0, d1, d2, dl, off, off + 1, holey, dli,
               total_lines, total_lines + off);
        total_lines += off + 1;

        for (entry = 0; entry < 32; entry++) {
            uint8_t mode = machine_peek_bus((uint16_t)(dl + 1));
            uint16_t graphaddr;
            int wm = 0, ind = 0, pal, width, hpos, hdr;

            if ((mode & 0x5f) == 0) { printf("    <end of DL>\n"); break; }

            if ((mode & 0x1f) == 0) {
                uint8_t e0 = machine_peek_bus(dl);
                uint8_t e1 = machine_peek_bus((uint16_t)(dl + 1));
                uint8_t e2 = machine_peek_bus((uint16_t)(dl + 2));
                uint8_t e3 = machine_peek_bus((uint16_t)(dl + 3));
                uint8_t e4 = machine_peek_bus((uint16_t)(dl + 4));
                graphaddr = (uint16_t)(e0 | (e2 << 8));
                wm    = (e1 & 0x80) != 0;
                ind   = (e1 & 0x20) != 0;
                pal   = (e3 & 0xe0) >> 3;
                width = (~e3 & 0x1f) + 1;
                hpos  = e4;
                dl = (uint16_t)(dl + 5); hdr = 5;
            } else {
                uint8_t e0 = machine_peek_bus(dl);
                uint8_t e1 = machine_peek_bus((uint16_t)(dl + 1));
                uint8_t e2 = machine_peek_bus((uint16_t)(dl + 2));
                uint8_t e3 = machine_peek_bus((uint16_t)(dl + 3));
                graphaddr = (uint16_t)(e0 | (e2 << 8));
                pal   = (e1 & 0xe0) >> 3;
                width = (~e1 & 0x1f) + 1;
                hpos  = e3;
                dl = (uint16_t)(dl + 4); hdr = 4;
            }

            printf("    hdr%d gfx=$%04X w=%2d hpos=%3d pal=%2d ind=%d wm=%d\n",
                   hdr, graphaddr, width, hpos, pal, ind, wm);
        }

        dll = (uint16_t)(dll + 3);
        if (total_lines >= 242) { printf("\n(covered %d lines)\n", total_lines); break; }
    }
}

/*
 * One-line machine-readable summary, for the whole-library sweep.
 * Reports what the cart was detected as, whether the CPU survived, and two
 * cheap "does this look like a picture" metrics:
 *   uniq  -- distinct colour indices in the visible frame
 *   motion-- how many of the last frames had a different CRC than the one
 *            before it (0 = completely frozen image)
 */
static const char *cart_type_name(int t)
{
    static const char *names[] = {
        "UNKNOWN", "A2K", "A4K", "A8K", "A16K", "A32K", "DC8K", "PB8K", "AR",
        "7800_8K", "7800_16K", "7800_32K", "7800_48K",
        "7800_SG", "7800_SGR", "7800_S9", "7800_AB", "7800_AC",
        "CBS12K", "DPC", "7800_S4", "7800_S4R", "SB"
    };
    if (t < 0 || t >= (int)(sizeof(names)/sizeof(names[0]))) return "?";
    return names[t];
}

static int frame_unique_colors(void)
{
    uint8_t *fb = machine_get_frame_buffer();
    int seen[256], w, h, x0, y0, fbw, x, y, n = 0;
    if (!fb) return 0;
    memset(seen, 0, sizeof(seen));
    visible_window(&w, &h, &x0, &y0);
    fbw = (machine_get_type() == MACHINE_7800) ? FB_WIDTH_7800 : FB_WIDTH_2600;
    for (y = 0; y < h; y++) {
        const uint8_t *src = fb + (y + y0) * fbw + x0;
        for (x = 0; x < w; x++) seen[src[x]] = 1;
    }
    for (x = 0; x < 256; x++) n += seen[x];
    return n;
}

/* ---- console switch / joystick helpers ---- */
static int name_to_switch(const char *n)
{
    if (!strcmp(n, "reset"))  return 0;
    if (!strcmp(n, "select")) return 1;
    if (!strcmp(n, "ldiff"))  return 2;
    if (!strcmp(n, "rdiff"))  return 3;
    return -1;
}
static int name_to_dir(const char *n)
{
    if (!strcmp(n, "up"))    return 0;
    if (!strcmp(n, "down"))  return 1;
    if (!strcmp(n, "left"))  return 2;
    if (!strcmp(n, "right")) return 3;
    return -1;
}
static void apply_input(const char *name, int pressed)
{
    int s = name_to_switch(name), d;
    if (s >= 0) { machine_set_switch(s, pressed); return; }
    d = name_to_dir(name);
    if (d >= 0) { machine_set_joystick(0, d, pressed); return; }
    if (!strcmp(name, "fire"))  { machine_set_trigger(0, pressed); return; }
    if (!strcmp(name, "fire2")) { machine_set_trigger2(0, pressed); return; }
    fprintf(stderr, "unknown input '%s'\n", name);
}

int main(int argc, char **argv)
{
    const char *rom = NULL, *out = NULL, *seq = NULL, *raw = NULL;
    const char *hold[8]; int nhold = 0;
    struct { char name[16]; int frame; } press[8]; int npress = 0;
    int frames = 600, do_hash = 0, do_dl = 0, do_report = 0, i, f, mtype;
    uint32_t prev_crc = 0; int motion = 0, motion_window = 0;

    crc_init();

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-raw") && i + 1 < argc) raw = argv[++i];
        else if (!strcmp(argv[i], "-seq") && i + 1 < argc) seq = argv[++i];
        else if (!strcmp(argv[i], "-hash")) do_hash = 1;
        else if (!strcmp(argv[i], "-dl")) do_dl = 1;
        else if (!strcmp(argv[i], "-report")) do_report = 1;
        else if (!strcmp(argv[i], "-hold") && i + 1 < argc) {
            if (nhold < 8) hold[nhold++] = argv[++i]; else i++;
        }
        else if (!strcmp(argv[i], "-press") && i + 1 < argc) {
            char *spec = argv[++i], *colon = strchr(spec, ':');
            if (colon && npress < 8) {
                size_t len = (size_t)(colon - spec);
                if (len > 15) len = 15;
                memcpy(press[npress].name, spec, len);
                press[npress].name[len] = 0;
                press[npress].frame = atoi(colon + 1);
                npress++;
            }
        }
        else if (argv[i][0] != '-') rom = argv[i];
        else fprintf(stderr, "ignoring unknown option %s\n", argv[i]);
    }

    if (!rom) {
        fprintf(stderr, "usage: headless <rom.a26|rom.a78> [-f N] [-o out.ppm] "
                        "[-seq DIR] [-hash] [-dl] [-report] [-hold SW] [-press SW:FRAME]\n");
        return 2;
    }

    /* Machine type from extension, matching filepicker_detect_rom_type(). */
    {
        const char *dot = strrchr(rom, '.');
        mtype = (dot && (!strcmp(dot, ".a78") || !strcmp(dot, ".A78")))
                ? MACHINE_7800 : MACHINE_2600;
    }

    machine_init();
    if (machine_load_rom(rom, mtype) != 0) {
        fprintf(stderr, "FAILED to load %s\n", rom);
        return 1;
    }
    machine_reset();

    for (i = 0; i < nhold; i++) apply_input(hold[i], 1);

    for (f = 0; f < frames; f++) {
        for (i = 0; i < npress; i++) {
            if (f == press[i].frame) apply_input(press[i].name, 1);
            if (f == press[i].frame + 10) apply_input(press[i].name, 0);
        }

        machine_run_frame();

        if (do_hash) printf("frame %5d crc %08x\n", f, frame_hash());

        /* Track image motion over the last 60 frames. */
        if (f >= frames - 60) {
            uint32_t c = frame_hash();
            if (motion_window++ > 0 && c != prev_crc) motion++;
            prev_crc = c;
        }
        if (seq) {
            char path[512];
            snprintf(path, sizeof(path), "%s/f%04d.ppm", seq, f);
            write_ppm(path);
        }
    }

    if (do_report) {
        M6502 *cpu = machine_get_cpu();
        Cart *cart = machine_get_cart();
        printf("%s\tjam=%d\tuniq=%d\tmotion=%d\tcrc=%08x\t%s\n",
               cart_type_name(cart ? (int)cart->type : -1),
               cpu->jammed, frame_unique_colors(), motion, frame_hash(), rom);
    }

    /* Full, uncropped colour-index plane, for diffing against the reference. */
    if (raw) {
        uint8_t *fb = machine_get_frame_buffer();
        int fbw = (machine_get_type() == MACHINE_7800) ? FB_WIDTH_7800 : FB_WIDTH_2600;
        FILE *fp = fopen(raw, "wb");
        if (!fp) { perror(raw); return 1; }
        fwrite(fb, 1, (size_t)fbw * FB_HEIGHT, fp);
        fclose(fp);
    }

    if (do_dl) dump_display_list(32);

    if (out && !write_ppm(out)) return 1;

    {
        M6502 *cpu = machine_get_cpu();
        fprintf(stderr, "done: %d frames, PC=%04X A=%02X X=%02X Y=%02X S=%02X P=%02X "
                        "jammed=%d clock=%llu final_crc=%08x\n",
                frames, cpu->PC, cpu->A, cpu->X, cpu->Y, cpu->S, cpu->P,
                cpu->jammed, (unsigned long long)cpu->clock, frame_hash());
    }
    return 0;
}
