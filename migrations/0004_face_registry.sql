-- Face registry, ported from the VisionX enrolment model.
--
-- The important change from 0001: a profile is no longer a bag of independent
-- embeddings. It is ONE vector per person, and every photo that built it is
-- kept as a row of its own carrying its own embedding.
--
-- That matters for three reasons, all of them measured on the sibling project:
--
--  1. TOPPING UP. With independent rows, uploading five more photos for someone
--     who already had six left the recogniser with eleven unrelated entries.
--     With per-image rows and a recomputed mean, a second upload genuinely adds
--     to the profile instead of sitting beside it.
--
--  2. COHERENCE. A profile is only as good as the agreement between its OWN
--     photos. Over an 11-profile registry, healthy profiles agreed with
--     themselves at a median cosine of 0.64-0.74 while three sat at 0.32, 0.38
--     and 0.40 - one built from downcast frames, two from group shots where the
--     largest face was not always the same person. Averaging disagreeing
--     vectors produces a profile pointing at the MIDDLE of the gallery, which
--     then wins the argmax against any face too poor to look like anybody in
--     particular: the 0.40 profile was proposed for 6 of 8 tracks on a real
--     recording and outscored the right person on his own face, 0.59 to 0.54.
--     Every per-image quality score in it was 80-97, so nothing an operator
--     could see said anything was wrong. Keeping the images is what makes that
--     number computable at all.
--
--  3. REPAIR. When a profile is bad the fix is "delete those two photos", not
--     "start again". That needs the photos.

CREATE TABLE face_images (
    id           BIGSERIAL PRIMARY KEY,
    tenant_id    BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    employee_id  BIGINT NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    embedding    BYTEA NOT NULL,          -- float32[dim], little-endian
    dim          INT NOT NULL,
    quality      INT NOT NULL DEFAULT 0,  -- 0..100, from the pose and face size
    yaw          REAL NOT NULL DEFAULT 0,
    face_px      REAL NOT NULL DEFAULT 0,
    det_score    REAL NOT NULL DEFAULT 0,
    image_path   TEXT NOT NULL DEFAULT '',-- relative to the snapshot root
    filename     TEXT NOT NULL DEFAULT '',
    content_type TEXT NOT NULL DEFAULT 'image/jpeg',
    size_bytes   INT NOT NULL DEFAULT 0,
    source       TEXT NOT NULL DEFAULT 'upload'
                 CHECK (source IN ('upload','capture','legacy')),
    -- A row normally stands for one photo. `weight` exists so a row can stand
    -- for several, which is how a profile carried over from an older scheme
    -- keeps its combined pull instead of being outvoted by a single new photo.
    weight       INT NOT NULL DEFAULT 1,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX face_images_employee_idx ON face_images(tenant_id, employee_id, created_at);

-- face_profiles becomes ONE row per employee: the recomputed mean.
DROP TABLE IF EXISTS face_profiles;

CREATE TABLE face_profiles (
    tenant_id    BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    employee_id  BIGINT NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    embedding    BYTEA NOT NULL,
    dim          INT NOT NULL,
    image_count  INT NOT NULL DEFAULT 0,     -- sum of weights, not row count
    quality      INT NOT NULL DEFAULT 0,     -- mean over measured images
    -- Median pairwise cosine between this profile's own images. NULL under two
    -- images. This is the number that tells an operator WHICH profile is the
    -- one handing its name to other people.
    coherence    REAL,
    updated_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (tenant_id, employee_id)
);
