-- Exits the system could not put a name to, with the evidence it did have.
--
-- WHY THIS EXISTS. A person walking out is the hardest face on the camera: it
-- is the furthest away, it is turning, and there is no second chance — once
-- they are through the door the evidence stops. So the pooled evidence very
-- often falls short of the bar for naming somebody, and until now it was
-- simply discarded. The exit went unrecorded and nobody could say who it was,
-- even though the system had looked straight at them.
--
-- Nothing here is attendance. A row is an unanswered question: a crop of the
-- face, the identity it most resembled, and how thin that resemblance was. It
-- becomes attendance only when a person says so, which is what `resolved_by`
-- records — the system never promotes its own guess.
CREATE TABLE IF NOT EXISTS exit_reviews (
    id            BIGSERIAL PRIMARY KEY,
    tenant_id     BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    camera_id     BIGINT REFERENCES cameras(id) ON DELETE SET NULL,
    zone_id       BIGINT REFERENCES zones(id) ON DELETE SET NULL,
    ts            TIMESTAMPTZ NOT NULL,
    work_date     DATE NOT NULL,
    track_id      INT NOT NULL DEFAULT -1,

    -- What the evidence said, and how much of it there was. Kept even when it
    -- names nobody, because "looked most like Alice at 0.41" and "never got a
    -- face at all" are different problems with different fixes.
    guess_employee_id BIGINT REFERENCES employees(id) ON DELETE SET NULL,
    guess_score   REAL NOT NULL DEFAULT 0,
    guess_margin  REAL NOT NULL DEFAULT 0,
    guess_weight  REAL NOT NULL DEFAULT 0,

    -- The face, and the frame it came from. A reviewer needs both: the crop to
    -- recognise the person and the frame to see where they were going.
    crop_path     TEXT NOT NULL DEFAULT '',
    frame_path    TEXT NOT NULL DEFAULT '',

    -- Set when somebody answers the question. `dismissed` is an answer too —
    -- a visitor, or a false exit at the reception desk — and recording it is
    -- what stops the same row being re-examined every day.
    resolved_employee_id BIGINT REFERENCES employees(id) ON DELETE SET NULL,
    dismissed     BOOLEAN NOT NULL DEFAULT FALSE,
    resolved_by   BIGINT REFERENCES users(id) ON DELETE SET NULL,
    resolved_at   TIMESTAMPTZ,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- The list a reviewer opens is "what is still unanswered, newest first".
CREATE INDEX IF NOT EXISTS exit_reviews_open_idx
    ON exit_reviews(tenant_id, work_date DESC, ts DESC)
    WHERE resolved_employee_id IS NULL AND NOT dismissed;
