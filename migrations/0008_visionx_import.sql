-- Room for face data imported from another system.
--
-- Two things are needed, and both exist so the import can be UNDONE. An import
-- that cannot be reversed is one nobody dares run twice, and this one will be
-- run again whenever the other system re-exports.

-- 1. Where a face image came from. 'visionx' marks a vector carried in from the
--    other system rather than captured or uploaded here, which is what makes
--    "remove everything that was imported" a single, exact delete.
ALTER TABLE face_images DROP CONSTRAINT IF EXISTS face_images_source_check;
ALTER TABLE face_images ADD CONSTRAINT face_images_source_check
    CHECK (source = ANY (ARRAY['upload'::text, 'capture'::text,
                               'legacy'::text, 'visionx'::text]));

-- 2. Who a person is in the system they came from.
--
--    external_uid keeps their EMP-xxxx so a later export updates the same person
--    instead of creating a second one; names cannot do that job, because the
--    same person is "Esheta" there and "Eshita" here.
--
--    origin says who created the row. Only people this import created carry
--    'visionx', so removing the import never touches somebody enrolled here —
--    the merge adds vectors to existing staff without taking ownership of them.
ALTER TABLE employees ADD COLUMN IF NOT EXISTS external_uid text;
ALTER TABLE employees ADD COLUMN IF NOT EXISTS origin text NOT NULL DEFAULT 'local';

CREATE UNIQUE INDEX IF NOT EXISTS employees_tenant_external_uid
    ON employees (tenant_id, external_uid) WHERE external_uid IS NOT NULL;
