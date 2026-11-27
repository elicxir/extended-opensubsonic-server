-- Similarity between songs (docs/similarity.md). Computed by tools such as audio-embed and
-- replaced one kind at a time; not facts, so no source and no suggestion.

-- One computation per kind, rewritten on every replacement.
CREATE TABLE similarity_set (
  kind TEXT PRIMARY KEY,              -- 'audio_embed', 'audio_match', ...
  model TEXT NOT NULL,
  params TEXT NOT NULL,               -- JSON object
  track_count INTEGER NOT NULL,       -- songs that went into the computation
  pair_count INTEGER NOT NULL,
  created_at INTEGER NOT NULL);

-- An unordered pair of songs and its score: one row, track_a < track_b. The tool sends
-- each song's top K neighbours; the scores are symmetric, so ordering the rows that touch
-- a song by score gives its top k (k <= K).
-- The song comes first in both keys: the neighbour lookups use them, and so do the
-- cascades when a track is purged (they look up track_a = ? / track_b = ? alone).
-- Replacing a kind scans the table once.
CREATE TABLE similarity (
  kind TEXT NOT NULL REFERENCES similarity_set(kind) ON DELETE CASCADE,
  track_a TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  track_b TEXT NOT NULL REFERENCES track(id) ON DELETE CASCADE,
  score REAL NOT NULL,
  PRIMARY KEY (track_a, track_b, kind),
  CHECK (track_a < track_b)) WITHOUT ROWID;
CREATE INDEX similarity_b ON similarity(track_b, kind);
