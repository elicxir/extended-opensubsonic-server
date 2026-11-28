-- A picture of an artist, put there by hand or by a tool (PUT /ext/artists/{id}/image).
-- The server never finds one by itself. The picture is kept as it was sent; smaller
-- sizes are made on request and cached like other artwork.
CREATE TABLE artist_image (
  artist_id TEXT PRIMARY KEY REFERENCES artist(id) ON DELETE CASCADE,
  mime TEXT NOT NULL,
  data BLOB NOT NULL,
  source_url TEXT,   -- the page the picture came from
  license TEXT,      -- e.g. "CC BY-SA 4.0", "Public domain"
  credit TEXT,       -- who made it, as the source asks to be named
  updated_at INTEGER NOT NULL);
