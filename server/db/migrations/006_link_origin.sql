-- Where each link came from (docs/link-origin.md): what kind of evidence made it
-- (origin) and where to look it up (origin_ref, e.g. a MusicBrainz URL).
--
-- Suggestions carry it, and accepting one copies it onto the estimated rows (source 1)
-- the suggestion writes. Tag (2) and manual (3) rows leave it NULL: their origin is their
-- source. On an estimated row NULL means "not known" (made before this, or imported).

ALTER TABLE suggestion ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE suggestion ADD COLUMN origin_ref TEXT;

ALTER TABLE track_arrangement ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE track_arrangement ADD COLUMN origin_ref TEXT;
ALTER TABLE track_title ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE track_title ADD COLUMN origin_ref TEXT;
ALTER TABLE relation ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE relation ADD COLUMN origin_ref TEXT;
ALTER TABLE participant ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE participant ADD COLUMN origin_ref TEXT;
ALTER TABLE album_track ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE album_track ADD COLUMN origin_ref TEXT;
ALTER TABLE album_folder ADD COLUMN origin TEXT CHECK (origin IN ('external', 'audio', 'name', 'llm'));
ALTER TABLE album_folder ADD COLUMN origin_ref TEXT;

-- Existing suggestions. The server's own rules all match names; the tools in tools/
-- are told apart by their rule names, this once.
UPDATE suggestion SET origin = CASE
  WHEN method = 'rule' THEN 'name'
  WHEN rule IN ('musicbrainz_work', 'musicbrainz_arrangement', 'musicbrainz_parts') THEN 'external'
  WHEN rule = 'musicbrainz_alias' THEN 'name'
  WHEN rule = 'audio_match' THEN 'audio'
  ELSE 'llm' END;
UPDATE suggestion SET origin_ref = json_extract(evidence, '$.works[0]')
  WHERE evidence IS NOT NULL AND json_valid(evidence) AND json_type(evidence, '$.works') = 'array';

-- Existing estimated rows take their suggestion's origin.
UPDATE track_arrangement SET (origin, origin_ref) =
  (SELECT s.origin, s.origin_ref FROM suggestion s WHERE s.id = track_arrangement.suggestion_id)
  WHERE source = 1 AND suggestion_id IS NOT NULL;
UPDATE track_title SET (origin, origin_ref) =
  (SELECT s.origin, s.origin_ref FROM suggestion s WHERE s.id = track_title.suggestion_id)
  WHERE source = 1 AND suggestion_id IS NOT NULL;
UPDATE relation SET (origin, origin_ref) =
  (SELECT s.origin, s.origin_ref FROM suggestion s WHERE s.id = relation.suggestion_id)
  WHERE source = 1 AND suggestion_id IS NOT NULL;
UPDATE participant SET (origin, origin_ref) =
  (SELECT s.origin, s.origin_ref FROM suggestion s WHERE s.id = participant.suggestion_id)
  WHERE source = 1 AND suggestion_id IS NOT NULL;
UPDATE album_track SET (origin, origin_ref) =
  (SELECT s.origin, s.origin_ref FROM suggestion s WHERE s.id = album_track.suggestion_id)
  WHERE source = 1 AND suggestion_id IS NOT NULL;
UPDATE album_folder SET (origin, origin_ref) =
  (SELECT s.origin, s.origin_ref FROM suggestion s WHERE s.id = album_folder.suggestion_id)
  WHERE source = 1 AND suggestion_id IS NOT NULL;
