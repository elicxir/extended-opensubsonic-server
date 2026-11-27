-- M3: albums record where they came from (virtual albums can be made by hand in M4).
ALTER TABLE album ADD COLUMN source INTEGER NOT NULL DEFAULT 2;
CREATE INDEX album_folder_folder ON album_folder(folder_id);
CREATE INDEX folder_parent ON folder(parent_id);
CREATE INDEX arrangement_composition ON arrangement(composition_id);
-- Tracks scanned before M3 have no custom data yet: ask for one full rescan.
INSERT OR REPLACE INTO setting (key, value) VALUES ('rescan_required', '1');
