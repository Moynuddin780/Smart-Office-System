-- One desk is one desk, however many cameras can see it.
--
-- A desk zone is drawn per camera, so a desk two cameras cover is two zones —
-- and the daily report, keyed on (zone, date), showed it as two rows for the
-- same person. On this site Ramim appeared twice with 27 and 9 minutes, which
-- is not just ugly: the two are the SAME minutes seen from two angles, so any
-- total that adds them is roughly double the truth.
--
-- desk_key is what the zones have in common. Backfilled from the zone name,
-- because that is already how the operator says "these two are desk A".
ALTER TABLE zones ADD COLUMN IF NOT EXISTS desk_key text;
UPDATE zones SET desk_key = name WHERE type = 'desk' AND desk_key IS NULL;

CREATE INDEX IF NOT EXISTS zones_desk_key ON zones (tenant_id, desk_key)
    WHERE type = 'desk';
