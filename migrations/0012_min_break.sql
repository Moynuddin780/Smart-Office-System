-- The shortest absence that counts as a break.
--
-- WHY THIS IS A SETTING AND NOT A CONSTANT. An exit zone has to be drawn
-- somewhere, and on this site its far edge falls in front of the reception
-- desk: somebody standing at the desk leaves the zone and stops being tracked,
-- which looks exactly like walking out of the door. Measured on 27 October,
-- one employee was booked out twelve seconds after arriving and showed no
-- working hours for the rest of the day.
--
-- A false exit of that kind is always followed by the same person being seen
-- again moments later, usually on another camera. So an absence shorter than
-- this is read as what it is — a tracking gap, not a break — and the two
-- stays either side are rejoined into one.
--
-- 60 s is the operator's rule. It belongs here rather than in the code because
-- it is a policy about this office, and it has already been changed twice.
-- 0 disables it and counts every gap however short.
ALTER TABLE tenant_settings
    ADD COLUMN IF NOT EXISTS min_break_seconds integer NOT NULL DEFAULT 60;

DO $$ BEGIN
    ALTER TABLE tenant_settings ADD CONSTRAINT tenant_settings_min_break_sane
        CHECK (min_break_seconds BETWEEN 0 AND 3600);
EXCEPTION WHEN duplicate_object THEN NULL;
END $$;
