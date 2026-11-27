# eos

An OpenSubsonic-compatible music server with first-class support for soundtrack
collections (works, compositions, arrangements, relations, virtual albums).

- Requirements: https://claude.ai/artifact/YSMz59jZw56DzgCaJiT9Rt
- Detailed design: https://claude.ai/artifact/NzgE1GsRzzUFN6cHbja7ce

Status: **M5** — the shared remote control over WebSocket (`/ext/session`): one queue and
playback state per user, edits applied in arrival order, one active device making the
sound, transfer between devices, per-device volume, REST play queue kept in step.
ReplayGain values are read from tags and sent as `replayGain` in song data.

**Server contract for client authors: [docs/](docs/README.md)** (tags and IDs, `/rest`,
`/ext`, sync protocol, configuration).

Earlier milestones: the `/ext` API (reads, edits, suggestions and LLM estimates with
provenance, login and API keys, export/import) in M4; transcoding (opus/mp3), custom tags (`COLLECTION_WORK`, `COLLECTION`,
`COLLECTION_ALBUM`, `WORK`, `ARRANGEMENT`), rule-based suggestions, FTS5 search, virtual
folders in `getIndexes` / `getMusicDirectory`. Virtual albums stay out of `/rest` album
lists unless `[subsonic] virtual_albums = "show"`.

Clients log in with a per-device API key (`eos-admin apikey create <user> <device>`) or
with username and password (token auth). Plain `p=` passwords are off by default.

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
