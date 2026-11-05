-- Where a camera physically is.
--
-- A camera list that reads "camera 1, camera 3, Uniview Varifocal" tells an
-- operator nothing on the day something happens. Two fields, because they are
-- used differently:
--
--   site     — the building or branch. It GROUPS: the wall, the camera list and
--              the alert feed can be filtered down to one office, which is what
--              a customer with several branches needs on one appliance.
--   location — the spot inside that site ("Building A, Gate 1"). It is read,
--              not filtered on, and appears next to the camera name wherever
--              somebody has to act on an alert.
--
-- Both are free text and both may be empty: a single-office customer should not
-- be forced to invent a site name before their first camera works.
--
-- NOT called "zone". In this product a zone is a polygon drawn on a frame, and
-- reusing the word for a grouping label would make two different things share
-- one name in the same UI.
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS site     TEXT NOT NULL DEFAULT '';
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS location TEXT NOT NULL DEFAULT '';

-- Filtering the wall by site is the common read, and a tenant's camera count is
-- small, so this is about consistency of plan rather than raw speed.
CREATE INDEX IF NOT EXISTS cameras_site_idx ON cameras (tenant_id, site);
