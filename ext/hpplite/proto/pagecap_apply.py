#!/usr/bin/env python3
"""Rebuild a database from pagecap spools (#146): records ordered by their
commit time across spools, each record's pages written at their offsets and
the file truncated to the commit's size. The result must equal the live file
after PRAGMA wal_checkpoint(TRUNCATE).   usage: pagecap_apply.py OUT SPOOL..."""
import struct, sys
out = sys.argv[1]
recs = []
for path in sys.argv[2:]:
    data = open(path, 'rb').read(); o = 0
    while o + 32 <= len(data):
        ts, s1, s2, frame, ntrunc, ps, n = struct.unpack('>QIIIIII', data[o:o+32]); o += 32
        pages = []
        for _ in range(n):
            (pg,) = struct.unpack('>I', data[o:o+4]); o += 4
            pages.append((pg, data[o:o+ps])); o += ps
        recs.append((ts, s1, s2, frame, ntrunc, ps, pages))
recs.sort(key=lambda r: r[0])
f = open(out, 'wb')
for ts, s1, s2, frame, ntrunc, ps, pages in recs:
    for pg, img in pages:
        f.seek((pg - 1) * ps); f.write(img)
    f.truncate(ntrunc * ps)
f.close()
print(f"{len(recs)} commits from {len(sys.argv) - 2} spool(s) applied into {out}")
