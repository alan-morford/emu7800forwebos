#!/usr/bin/env python3
"""
gen_quirkdb.py -- generate src/quirk_db.h from ProSystem's Database.c.

A handful of 7800 carts need a per-cart deviation that cannot (yet) be derived
from a general hardware rule. The ProSystem emulator has tracked these upstream
for years in Database.c; this imports that table rather than hand-rolling
special cases here.

The only flag this port acts on today is bit 1 (CARTRIDGE_WSYNC_MASK), which
suppresses the MARIA WSYNC CPU halt. Kung Fu Master builds its display with
chained DLI handlers that set BACKGRND and then burn scanlines with runs of
`STA WSYNC`; honouring those halts puts every colour band in the wrong place.
Only Kung Fu Master (3 dumps) and Missing in Action carry the flag -- disabling
WSYNC globally changes 35 of 74 7800 ROMs, so it must stay per-cart.

Digests are MD5 of the ROM image *after* any 128-byte A78 header is stripped,
matching ProSystem's cartridge_digest.

Usage:
    python3 tools/gen_quirkdb.py <path/to/ProSystem/core/Database.c> > src/quirk_db.h
"""

import re
import sys

DIGEST_RE = re.compile(r'"([0-9a-fA-F]{32})"')
FLAGS_RE = re.compile(r'(\d+)\s*,\s*/\*\s*flags\s*\*/')
TITLE_RE = re.compile(r'"([^"]*)"\s*,\s*/\*\s*title\s*\*/')


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: gen_quirkdb.py <ProSystem Database.c>')

    src = open(sys.argv[1], encoding='utf-8', errors='replace').read()

    rows, scanned = [], 0
    for block in re.findall(r'\{(.*?)\}', src, re.S):
        d = DIGEST_RE.search(block)
        f = FLAGS_RE.search(block)
        if not d or not f:
            continue
        scanned += 1
        flags = int(f.group(1))
        if flags == 0:
            continue                      # nothing to say about this cart
        t = TITLE_RE.search(block)
        rows.append((d.group(1).lower(), flags, t.group(1) if t else ''))

    rows.sort(key=lambda r: r[0])

    out = sys.stdout
    out.write('/*\n')
    out.write(' * quirk_db.h -- GENERATED FILE, DO NOT EDIT BY HAND.\n')
    out.write(' *\n')
    out.write(' * Regenerate with:\n')
    out.write(' *     python3 tools/gen_quirkdb.py <ProSystem>/core/Database.c > src/quirk_db.h\n')
    out.write(' *\n')
    out.write(' * Source: ProSystem emulator Database.c (upstream, authoritative).\n')
    out.write(' * MD5 is of the ROM image after any 128-byte A78 header is stripped.\n')
    out.write(' * Sorted by MD5 so lookup can binary-search.\n')
    out.write(' */\n\n')
    out.write('#ifndef QUIRK_DB_H\n#define QUIRK_DB_H\n\n')
    out.write('static const CartQuirkEntry cart_quirk_db[] = {\n')
    for md5, flags, title in rows:
        b = ','.join('0x%s' % md5[i:i + 2] for i in range(0, 32, 2))
        names = []
        if flags & 1:
            names.append('CYCLE_STEALING')
        if flags & 2:
            names.append('NO_WSYNC')
        safe = title.replace('*/', '')[:48]
        out.write('    /* %-48s %s */\n' % (safe, '|'.join(names) or '-'))
        out.write('    {{%s}, 0x%02x},\n' % (b, flags))
    out.write('};\n\n')
    out.write('#endif /* QUIRK_DB_H */\n')

    sys.stderr.write('quirk_db.h: %d entries with quirks (%d carts scanned)\n'
                     % (len(rows), scanned))


if __name__ == '__main__':
    main()
