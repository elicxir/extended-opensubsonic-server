-- M5: ReplayGain values from tags, and each device's volume for the shared remote control.
ALTER TABLE track ADD COLUMN rg_track_gain REAL;
ALTER TABLE track ADD COLUMN rg_track_peak REAL;
ALTER TABLE track ADD COLUMN rg_album_gain REAL;
ALTER TABLE track ADD COLUMN rg_album_peak REAL;
ALTER TABLE device ADD COLUMN volume INTEGER NOT NULL DEFAULT 100;
ALTER TABLE device ADD COLUMN muted INTEGER NOT NULL DEFAULT 0;
-- Existing tracks need one full rescan to pick up their ReplayGain tags.
INSERT OR REPLACE INTO setting (key, value) VALUES ('rescan_required', '1');
