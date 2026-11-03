# Home Assistant on HPPLite (#19 validation)

Stock Home Assistant, its recorder on SQLite, HPPLite underneath. HA has no
hook for SQLite extensions, so the module is loaded into Python's `sqlite3`
when the interpreter starts (`hpplite_preload.py`, imported from a `.pth`
file - Homebrew's Python ships its own `sitecustomize`, which would shadow
one) and `HPPLITE_FILES` names the recorder's database.

`configuration.yaml` keeps the recorder busy without any device: an
automation every second increments a counter, sets a random number, toggles
a boolean (a template sensor follows), and every 60 ticks calls
`recorder.purge` with `keep_days: 0, repack: true` - HA's purge, then
VACUUM, then `PRAGMA OPTIMIZE`.

```bash
python3.13 -m venv venv && venv/bin/pip install homeassistant
SP=$(venv/bin/python -c "import site; print(site.getsitepackages()[0])")
cp tests/real_apps/homeassistant/hpplite_preload.py $SP/ && echo "import hpplite_preload" > $SP/hpplite.pth
mkdir config && cp tests/real_apps/homeassistant/configuration.yaml config/
export HPPLITE_EXT=build_real/hpplite.dylib HPPLITE_FILES="*/home-assistant_v2.db" \
       HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x...&datadir=$PWD/config&interval=2s" \
       HA_DB_URL="sqlite:///$PWD/config/home-assistant_v2.db"
venv/bin/hass -c config        # stop with SIGTERM; then the replica check:
HPPLITE_ANVIL_KEY0=0x... HPPLITE_ORM_DIR=$PWD/config ... python tests/orm/orm_common.py home-assistant_v2.db
```

## In a container (2026-11-03, anvil; Python 3.13 image, Debian trixie; page capture, the default)

`tests/real_apps/docker/homeassistant.sh`: Home Assistant from pip in the
image (`docker/Dockerfile.homeassistant`, with the requirements of the
integrations the suite's configuration loads, read from their manifests:
inside Docker Home Assistant installs nothing at first start), the `.pth`
preload, the Linux `hpplite.so` from `docker/so.sh`, anvil and the factory
on the host, the recorder running inside for 150 s (two purge-and-repack
cycles), the replica check on the host.

- Home Assistant up 4 s after its migrations; 176 ticks, two purges and
  VACUUMs, no HPPLite message in its log, nothing pending at the SIGTERM.
  75 page batches.
- The replica rebuilt from the chain equals the recorder's database: 13
  tables, 341 rows, 74 roots compared, the same root at height 75.

## Results (2026-10-31, anvil; Home Assistant 2026.2.3, Python 3.13.9, SQLite 3.51.0)

- Two HA processes one after the other on the same database (a SIGTERM
  between them), 842 ticks, about 14 purge-and-repack cycles: the recorder's
  migrations, states, events, purges and VACUUMs all captured (rows by
  default; schema changes and VACUUM as statements). No refusals, no HPPLite
  errors, nothing pending at either stop.
- A replica rebuilt from the chain alone equals the recorder's database: 13
  tables, 233 batches, same state root.

## Found on the way

- VACUUM was refused (its internal ATTACH) and, had it run, would not have
  reached replicas; fixed before this run (89dabecb80). Every repack here
  went through and replayed.
- `PRAGMA OPTIMIZE` (after every purge) runs ANALYZE on the tables it
  picks: `sqlite_stat1` on the writer, none on a replica. Query planner
  statistics, never results; the state root leaves them out, and so does
  the replica check now.
- A Python that clears a connection's callbacks as it closes it at
  interpreter exit (Homebrew's 3.13) made HPPLite report "another component
  replaced the connection's hooks" for nothing; a hook cleared at close is
  no longer counted (`test_loadable`).
