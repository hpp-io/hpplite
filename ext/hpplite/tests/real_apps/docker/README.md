# The application suites in containers

The host's runtimes drift (a PHP build goes away, a Node or Ruby is older than
the app wants); a container pins each app's runtime, and the module it loads is
built on the same Debian the runtime images stand on.

- `so.sh`: builds `hpplite.so` on Debian trixie (`Dockerfile.build`) into the
  named volume `hpplite-apps-build`, which every app container mounts
  read-only at `/work`. Rebuilt from the working tree each run.
- `Dockerfile.wordpress`, `wordpress.sh`, `wordpress_inner.sh`: PHP 8.4 and a
  stock WordPress with the SQLite Database Integration plugin in the image;
  `wordpress.sh` builds the module and the image, starts anvil and the
  factory on the host (`tests/l1/anvil_test.sh`, anvil listening on
  `0.0.0.0` for the container), runs `wordpress/session.sh` inside, copies
  the database out under `~/.cache/hpplite-apps/` (Docker Desktop shares the
  home directory with containers; macOS's per-user temp directory came back
  empty) and runs the replica check on the host (`tests/orm/orm_common.py`
  with `build_real/hpplite.dylib`). Exit 0 when the session had no failure
  and the replica equals the site.

```bash
tests/real_apps/docker/wordpress.sh
```

- `Dockerfile.rails`, `rails.sh`, `rails_inner.sh`: Ruby 3.3, the rails gem
  and the app `rails/setup.sh` generates, in the image; inside, `db:prepare`,
  Puma with two workers, Solid Queue and the 16-client session
  (`rails/drive.py`); then the four databases copied out and a replica of
  each rebuilt on the host by its name (`dbname={file}`).
- `Dockerfile.ghost`, `ghost.sh`, `ghost_inner.sh`: Node 24 and pnpm 11, Ghost
  installed by ghost-cli as the image's node user; at run time better-sqlite3
  is rebuilt against the volume's SQLite amalgamation (the bundled one
  truncates bound strings in expanded SQL), then Ghost's Admin API session
  (`ghost/drive.py`) and the replica check on the host.

```bash
tests/real_apps/docker/rails.sh
tests/real_apps/docker/ghost.sh
```
