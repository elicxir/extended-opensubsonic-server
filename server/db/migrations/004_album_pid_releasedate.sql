-- The album PID now uses RELEASEDATE only (DATE no longer splits albums). Recompute
-- every PID with one full rescan; albums whose tracks all move keep their ids.
INSERT OR REPLACE INTO setting (key, value) VALUES ('rescan_required', '1');
