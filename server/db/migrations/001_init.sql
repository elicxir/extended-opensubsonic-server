-- eos schema, version 1. See the detailed design (データベース設計).
-- ids: 22-char base62 TEXT, except user and library (INTEGER).
-- time: unix milliseconds. source: 1 = estimate (accepted), 2 = tag, 3 = manual.

CREATE TABLE library (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL UNIQUE,
  last_scan_at INTEGER);

CREATE TABLE artist (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL, sort_name TEXT, name_norm TEXT NOT NULL,
  mbz_artist_id TEXT);

CREATE TABLE artwork (
  id TEXT PRIMARY KEY, kind TEXT NOT NULL CHECK (kind IN ('embedded', 'file')),
  track_id TEXT, path TEXT, mtime INTEGER NOT NULL, sha1 TEXT NOT NULL);

CREATE TABLE album (
  id TEXT PRIMARY KEY,                -- PID at creation; never changes
  pid TEXT NOT NULL,                  -- PID.Album computed from the current tags
  library_id INTEGER NOT NULL REFERENCES library(id),
  name TEXT NOT NULL, sort_name TEXT, display_artist TEXT,
  release_date TEXT, original_release_date TEXT, year INTEGER,
  kind TEXT NOT NULL DEFAULT 'physical' CHECK (kind IN ('physical', 'virtual')),
  mbz_album_id TEXT, is_compilation INTEGER NOT NULL DEFAULT 0,
  dir_path TEXT,
  cover_art_id TEXT REFERENCES artwork(id),
  song_count INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0,
  missing INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE INDEX album_pid ON album(pid);
CREATE INDEX album_kind ON album(kind);

CREATE TABLE album_disc (
  album_id TEXT NOT NULL REFERENCES album(id) ON DELETE CASCADE,
  disc_number INTEGER NOT NULL, title TEXT NOT NULL,
  PRIMARY KEY (album_id, disc_number));

CREATE TABLE track (
  id TEXT PRIMARY KEY,                -- PID at creation; never changes
  pid TEXT NOT NULL,                  -- PID.Track computed from the current tags
  library_id INTEGER NOT NULL REFERENCES library(id),
  path TEXT NOT NULL,                 -- relative to the library root
  album_id TEXT NOT NULL REFERENCES album(id),  -- the physical album
  title TEXT NOT NULL, sort_title TEXT, subtitle TEXT,
  disc_number INTEGER, track_number INTEGER,
  duration_ms INTEGER NOT NULL, suffix TEXT NOT NULL, content_type TEXT NOT NULL,
  bit_rate INTEGER, sample_rate INTEGER, bit_depth INTEGER, channels INTEGER,
  size INTEGER NOT NULL, mtime INTEGER NOT NULL,
  audio_md5 TEXT,                     -- FLAC STREAMINFO MD5 of the decoded audio
  mbz_recording_id TEXT, mbz_track_id TEXT,
  display_artist TEXT, display_composer TEXT,
  performance_kind TEXT CHECK (performance_kind IN ('studio', 'live')),
  recorded_date TEXT,
  release_date TEXT, year INTEGER,
  has_embedded_art INTEGER NOT NULL DEFAULT 0,
  tags_json TEXT NOT NULL,
  missing INTEGER NOT NULL DEFAULT 0,
  missing_since INTEGER,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
  UNIQUE (library_id, path));
CREATE INDEX track_album ON track(album_id, disc_number, track_number);
CREATE INDEX track_audio_md5 ON track(audio_md5);
CREATE INDEX track_pid ON track(pid);

CREATE TABLE album_track (            -- membership of virtual albums
  album_id TEXT NOT NULL REFERENCES album(id) ON DELETE CASCADE,
  track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  disc_number INTEGER, track_number INTEGER,
  source INTEGER NOT NULL, suggestion_id TEXT,
  excluded INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (album_id, track_id, source));
CREATE INDEX album_track_track ON album_track(track_id);

CREATE TABLE participant (
  entity_type TEXT NOT NULL CHECK (entity_type IN ('track', 'album', 'composition', 'arrangement')),
  entity_id TEXT NOT NULL,
  artist_id TEXT NOT NULL REFERENCES artist(id),
  role TEXT NOT NULL,
  sub_role TEXT NOT NULL DEFAULT '',
  position INTEGER NOT NULL, source INTEGER NOT NULL, suggestion_id TEXT,
  PRIMARY KEY (entity_type, entity_id, role, sub_role, artist_id, source));
CREATE INDEX participant_artist ON participant(artist_id, role);

CREATE TABLE genre (id TEXT PRIMARY KEY, name TEXT NOT NULL UNIQUE);
CREATE TABLE track_genre (
  track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  genre_id TEXT NOT NULL REFERENCES genre(id), position INTEGER NOT NULL,
  PRIMARY KEY (track_id, genre_id));

CREATE TABLE composition (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, sort_name TEXT, name_norm TEXT NOT NULL,
  mbz_work_id TEXT UNIQUE, source INTEGER NOT NULL, suggestion_id TEXT,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE INDEX composition_norm ON composition(name_norm);

CREATE TABLE arrangement (
  id TEXT PRIMARY KEY,
  composition_id TEXT NOT NULL REFERENCES composition(id) ON DELETE CASCADE,
  name TEXT,
  kind TEXT NOT NULL DEFAULT 'original',
  is_implicit INTEGER NOT NULL DEFAULT 0,
  source INTEGER NOT NULL, suggestion_id TEXT,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE UNIQUE INDEX arrangement_implicit ON arrangement(composition_id) WHERE is_implicit = 1;

CREATE TABLE track_arrangement (
  track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  arrangement_id TEXT NOT NULL REFERENCES arrangement(id) ON DELETE CASCADE,
  source INTEGER NOT NULL, suggestion_id TEXT,
  PRIMARY KEY (track_id, source));
CREATE INDEX track_arrangement_arr ON track_arrangement(arrangement_id);

CREATE TABLE title (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, sort_name TEXT, name_norm TEXT NOT NULL UNIQUE,
  kind TEXT, year INTEGER, source INTEGER NOT NULL, suggestion_id TEXT,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);

CREATE TABLE track_title (
  track_id TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  title_id TEXT NOT NULL REFERENCES title(id) ON DELETE CASCADE,
  source INTEGER NOT NULL, suggestion_id TEXT,
  excluded INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (track_id, title_id, source));
CREATE INDEX track_title_title ON track_title(title_id);

CREATE TABLE folder (
  id TEXT PRIMARY KEY, parent_id TEXT REFERENCES folder(id) ON DELETE CASCADE,
  name TEXT NOT NULL, path_key TEXT NOT NULL UNIQUE,
  sort_order INTEGER, source INTEGER NOT NULL, suggestion_id TEXT);

CREATE TABLE album_folder (
  album_id TEXT NOT NULL REFERENCES album(id) ON DELETE CASCADE,
  folder_id TEXT NOT NULL REFERENCES folder(id) ON DELETE CASCADE,
  source INTEGER NOT NULL, suggestion_id TEXT,
  excluded INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (album_id, folder_id, source));

CREATE TABLE relation (
  id TEXT PRIMARY KEY,
  from_type TEXT NOT NULL CHECK (from_type IN ('composition', 'track')), from_id TEXT NOT NULL,
  to_type   TEXT NOT NULL CHECK (to_type   IN ('composition', 'track')), to_id   TEXT NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('medley', 'quote', 'derivative', 'cover', 'related')),
  subtype TEXT, position INTEGER, note TEXT,
  source INTEGER NOT NULL, suggestion_id TEXT,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL,
  UNIQUE (from_type, from_id, to_type, to_id, type));
CREATE INDEX relation_to ON relation(to_type, to_id);

CREATE TABLE field_override (
  entity_type TEXT NOT NULL, entity_id TEXT NOT NULL, field TEXT NOT NULL,
  value TEXT, updated_at INTEGER NOT NULL,
  PRIMARY KEY (entity_type, entity_id, field));

CREATE TABLE user (
  id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE,
  is_admin INTEGER NOT NULL DEFAULT 0,
  password_hash TEXT,
  password_enc BLOB,
  created_at INTEGER NOT NULL);

CREATE TABLE suggestion (
  id TEXT PRIMARY KEY,
  rule TEXT NOT NULL,
  method TEXT NOT NULL CHECK (method IN ('rule', 'llm')),
  model TEXT, evidence TEXT,
  fingerprint TEXT NOT NULL UNIQUE,
  payload TEXT NOT NULL,
  score REAL NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('pending', 'accepted', 'auto_applied', 'rejected', 'stale')),
  created_at INTEGER NOT NULL, decided_at INTEGER, decided_by INTEGER REFERENCES user(id));
CREATE INDEX suggestion_status ON suggestion(status, rule);

CREATE TABLE api_key (
  id TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  device_name TEXT NOT NULL,
  key_hash BLOB NOT NULL UNIQUE,
  created_at INTEGER NOT NULL, last_used_at INTEGER, revoked_at INTEGER);

CREATE TABLE device (
  id TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  name TEXT NOT NULL, client TEXT, last_seen_at INTEGER);

CREATE TABLE annotation (
  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  item_type TEXT NOT NULL CHECK (item_type IN ('track', 'album', 'artist')),
  item_id TEXT NOT NULL,
  starred_at INTEGER, rating INTEGER CHECK (rating BETWEEN 0 AND 5),
  play_count INTEGER NOT NULL DEFAULT 0, last_played_at INTEGER,
  PRIMARY KEY (user_id, item_type, item_id));

CREATE TABLE scrobble (
  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  track_id TEXT NOT NULL, played_at INTEGER NOT NULL, device_id TEXT);

CREATE TABLE playlist (
  id TEXT PRIMARY KEY, owner_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  name TEXT NOT NULL, comment TEXT, public INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
CREATE TABLE playlist_entry (
  playlist_id TEXT NOT NULL REFERENCES playlist(id) ON DELETE CASCADE,
  position INTEGER NOT NULL, track_id TEXT NOT NULL,
  PRIMARY KEY (playlist_id, position));

CREATE TABLE play_queue (
  user_id INTEGER PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,
  revision INTEGER NOT NULL,
  current_index INTEGER, position_ms INTEGER NOT NULL DEFAULT 0,
  active_device_id TEXT, changed_at INTEGER NOT NULL, changed_by TEXT);
CREATE TABLE play_queue_entry (
  user_id INTEGER NOT NULL REFERENCES play_queue(user_id) ON DELETE CASCADE,
  position INTEGER NOT NULL,
  entry_id TEXT NOT NULL,
  track_id TEXT NOT NULL,
  PRIMARY KEY (user_id, position));

CREATE TABLE preferred_version (
  user_id INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
  composition_id TEXT NOT NULL REFERENCES composition(id) ON DELETE CASCADE,
  track_id TEXT NOT NULL,
  PRIMARY KEY (user_id, composition_id));

CREATE TABLE scan_run (
  id INTEGER PRIMARY KEY, kind TEXT NOT NULL CHECK (kind IN ('quick', 'full')),
  started_at INTEGER NOT NULL, finished_at INTEGER,
  added INTEGER, updated INTEGER, moved INTEGER, missing INTEGER, errors INTEGER);

CREATE TABLE setting (key TEXT PRIMARY KEY, value TEXT NOT NULL);

CREATE VIRTUAL TABLE search_index USING fts5(
  kind UNINDEXED, ref_id UNINDEXED, name, alt, context, tokenize = 'trigram');

CREATE VIEW track_arrangement_eff AS
SELECT ta.track_id, ta.arrangement_id
FROM track_arrangement ta
WHERE ta.source = (SELECT MAX(source) FROM track_arrangement WHERE track_id = ta.track_id);
