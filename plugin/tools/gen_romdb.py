#!/usr/bin/env python3
"""
gen_romdb.py -- generate src/rom_db.h from EMU7800's ROMProperties.csv.

The upstream EMU7800 project (which this port derives from) ships an
authoritative MD5 -> cart-type/controller database. Guessing a cart's
bankswitching scheme from ROM size and byte patterns is unreliable: it is
what caused Summer Games / Winter Games (A78SGR -- SuperGame *with* 16KB RAM
at $4000) to be mis-detected as plain A78SG, so the games' display lists,
which they build in that RAM, were read back as ROM graphics bytes.

Usage:
    python3 tools/gen_romdb.py <path/to/ROMProperties.csv> > src/rom_db.h

Rows whose CartType this port does not implement are still emitted with
CART_UNKNOWN so the controller info is kept and the existing size-based
detection continues to handle the cart.
"""

import csv
import re
import sys

MD5_RE = re.compile(r'^[0-9a-fA-F]{32}$')

# EMU7800 CartType string -> this port's CartType enum.
# Unlisted / unimplemented schemes map to CART_UNKNOWN, which means
# "no override" -- size-based detection still runs.
CART_TYPE_MAP = {
    'A2K':       'CART_A2K',
    'A4K':       'CART_A4K',
    'A8K':       'CART_A8K',
    'A16K':      'CART_A16K',
    'A32K':      'CART_A32K',
    'DC8K':      'CART_DC8K',
    'PB8K':      'CART_PB8K',
    'CBS12K':    'CART_CBS12K',
    'DPC':       'CART_DPC',
    'AR':        'CART_AR',
    # 7800 linear
    'A7808':     'CART_7800_8K',
    'A7816':     'CART_7800_16K',
    'A7832':     'CART_7800_32K',
    'A7832P':    'CART_7800_32K',   # + Pokey
    'A7832PL':   'CART_7800_32K',   # + Pokey @ $0450
    'A7848':     'CART_7800_48K',
    # 7800 SuperGame family
    'A78SG':     'CART_7800_SG',
    'A78SGR':    'CART_7800_SGR',   # SuperGame + 16KB RAM at $4000
    'A78SGP':    'CART_7800_SG',    # + Pokey
    'A78S9':     'CART_7800_S9',
    'A78S9PL':   'CART_7800_S9',    # + Pokey @ $0450
    'A78S4':     'CART_7800_S4',
    'A78S4R':    'CART_7800_S4R',
    'A78AB':     'CART_7800_AB',
    'A78AC':     'CART_7800_AC',
}

CONTROLLER_MAP = {
    '':                'CTRL_NONE',
    'ProLineJoystick': 'CTRL_PROLINE_JOYSTICK',
    'Lightgun':        'CTRL_LIGHTGUN',
    'Paddles':         'CTRL_PADDLE',
}

MACHINE_7800 = ('A7800NTSC', 'A7800PAL', 'A7800NTSCxm', 'A7800PALxm', 'A7800NTSChsc')


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: gen_romdb.py <ROMProperties.csv>')

    rows, skipped = [], 0
    with open(sys.argv[1], newline='', encoding='utf-8-sig') as fp:
        for fields in csv.reader(fp):
            # Titles may contain unquoted commas, so column positions are not
            # stable. Anchor on the MD5 field (the only 32-hex-char column)
            # and index the rest relative to it.
            md5_idx = next((i for i, f in enumerate(fields)
                            if MD5_RE.match(f.strip())), None)
            if md5_idx is None or md5_idx < 4:
                skipped += 1
                continue

            md5      = fields[md5_idx].strip().lower()
            rctrl    = fields[md5_idx - 1].strip()
            lctrl    = fields[md5_idx - 2].strip()
            machine  = fields[md5_idx - 3].strip()
            carttype = fields[md5_idx - 4].strip()
            title    = ','.join(fields[:md5_idx - 4]).strip()

            cart = CART_TYPE_MAP.get(carttype, 'CART_UNKNOWN')
            lc   = CONTROLLER_MAP.get(lctrl, 'CTRL_NONE')
            rc   = CONTROLLER_MAP.get(rctrl, 'CTRL_NONE')

            # A 7800 cart with no explicit controller uses the ProLine stick.
            if machine in MACHINE_7800:
                if lc == 'CTRL_NONE' and lctrl == '':
                    lc = 'CTRL_PROLINE_JOYSTICK'
                if rc == 'CTRL_NONE' and rctrl == '':
                    rc = 'CTRL_PROLINE_JOYSTICK'

            # Nothing to say about this ROM -- omit it rather than bloat the table.
            if cart == 'CART_UNKNOWN' and lc == 'CTRL_NONE' and rc == 'CTRL_NONE':
                continue

            rows.append((md5, cart, lc, rc, title, carttype, machine))

    rows.sort(key=lambda r: r[0])

    out = sys.stdout
    out.write('/*\n')
    out.write(' * rom_db.h -- GENERATED FILE, DO NOT EDIT BY HAND.\n')
    out.write(' *\n')
    out.write(' * Regenerate with:\n')
    out.write(' *     python3 tools/gen_romdb.py <ROMProperties.csv> > src/rom_db.h\n')
    out.write(' *\n')
    out.write(' * Source: EMU7800 ROMProperties.csv (upstream, authoritative).\n')
    out.write(' * Sorted by MD5 so lookup can binary-search.\n')
    out.write(' */\n\n')
    out.write('#ifndef ROM_DB_H\n#define ROM_DB_H\n\n')
    out.write('static const RomDbEntry rom_properties_db[] = {\n')
    for md5, cart, lc, rc, title, carttype, machine in rows:
        b = ','.join('0x%s' % md5[i:i + 2] for i in range(0, 32, 2))
        safe = title.replace('*/', '').replace('\\', '')[:56]
        out.write('    /* %-56s %-9s %s */\n' % (safe, carttype or '-', machine or '-'))
        out.write('    {{%s},\n     %s, %s, %s},\n' % (b, cart, lc, rc))
    out.write('};\n\n')
    out.write('#endif /* ROM_DB_H */\n')

    sys.stderr.write('rom_db.h: %d entries (%d rows skipped, no MD5)\n'
                     % (len(rows), skipped))


if __name__ == '__main__':
    main()
