# eos

An OpenSubsonic-compatible music server with first-class support for soundtrack
collections (works, compositions, arrangements, relations, virtual albums).

- Requirements: https://claude.ai/artifact/YSMz59jZw56DzgCaJiT9Rt
- Detailed design: https://claude.ai/artifact/NzgE1GsRzzUFN6cHbja7ce

Status: **M1** — database, scanner (standard tags, persistent IDs, move detection) and
`eos-admin`. The HTTP API arrives in M2.

## Run with Docker

```bash
EOS_MUSIC_DIR=/path/to/music docker compose up -d --build
docker compose exec eos eos-admin user add alice --admin
docker compose exec eos eos-admin apikey create alice desktop
docker compose exec eos eos-admin tag-report
```

| Host folder (env var)        | In the container | Holds                                   |
| ---------------------------- | ---------------- | --------------------------------------- |
| `EOS_MUSIC_DIR` (`./music`)  | `/music` (ro)    | your music                              |
| `EOS_DATA_DIR` (`./data`)    | `/data`          | `eos.db`, `secret.key`, caches          |

The container runs as `EOS_UID:EOS_GID` (default `1000:1000`). To change settings, copy
`docker/eos.toml`, edit it and mount it over `/etc/eos/eos.toml` (see `compose.yaml`).
Back up `secret.key` separately from the database: it decrypts stored passwords.

## Develop

Everything builds inside the `eos-dev` container (Ubuntu 26.04), so the host needs only Docker.

```bash
scripts/dev.sh build   # configure + build into build/
scripts/dev.sh test    # build and run the tests
```

Layout: `server/` (core, db, auth, scanner, `main.cpp` = eosd), `tools/eos-admin/`,
`tests/`, `server/db/migrations/*.sql` (embedded at build time).
