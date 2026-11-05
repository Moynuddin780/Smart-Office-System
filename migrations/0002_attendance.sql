-- Attendance and desk performance.

CREATE TABLE attendance_events (
    id            BIGSERIAL PRIMARY KEY,
    tenant_id     BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    employee_id   BIGINT NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    camera_id     BIGINT REFERENCES cameras(id) ON DELETE SET NULL,
    direction     TEXT NOT NULL CHECK (direction IN ('in','out')),
    ts            TIMESTAMPTZ NOT NULL,
    work_date     DATE NOT NULL,
    confidence    REAL NOT NULL DEFAULT 0,
    track_id      INT NOT NULL DEFAULT -1,
    snapshot_path TEXT NOT NULL DEFAULT ''
);
CREATE INDEX attendance_events_lookup_idx
    ON attendance_events(tenant_id, employee_id, work_date, ts);
CREATE INDEX attendance_events_day_idx ON attendance_events(tenant_id, work_date);

-- The rolled-up day. Recomputed from attendance_events on every write, so it is
-- a cache and never the source of truth - a mis-recorded punch can be deleted
-- and the day recomputed rather than hand-edited.
CREATE TABLE attendance_days (
    tenant_id      BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    employee_id    BIGINT NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    work_date      DATE NOT NULL,
    first_in       TIMESTAMPTZ,
    last_out       TIMESTAMPTZ,
    gross_seconds  INT NOT NULL DEFAULT 0,
    inside_seconds INT NOT NULL DEFAULT 0,
    break_seconds  INT NOT NULL DEFAULT 0,
    entries        INT NOT NULL DEFAULT 0,
    status         TEXT NOT NULL DEFAULT 'absent'
                   CHECK (status IN ('absent','present','late','early_leave')),
    -- The day ended on an unmatched entry and last_out was inferred. A report
    -- must be able to say so rather than presenting a guess as an observation.
    assumed_out    BOOLEAN NOT NULL DEFAULT FALSE,
    updated_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (tenant_id, employee_id, work_date)
);
CREATE INDEX attendance_days_day_idx ON attendance_days(tenant_id, work_date);

CREATE TABLE desk_sessions (
    id                 BIGSERIAL PRIMARY KEY,
    tenant_id          BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    zone_id            BIGINT NOT NULL REFERENCES zones(id) ON DELETE CASCADE,
    employee_id        BIGINT REFERENCES employees(id) ON DELETE SET NULL,
    work_date          DATE NOT NULL,
    started_at         TIMESTAMPTZ NOT NULL,
    ended_at           TIMESTAMPTZ,
    seconds            INT NOT NULL DEFAULT 0,
    -- A face was recognised in this zone and it was NOT the assigned employee.
    -- The counting is unaffected; the flag stops a report crediting the wrong
    -- person without saying so.
    identity_mismatch  BOOLEAN NOT NULL DEFAULT FALSE
);
CREATE INDEX desk_sessions_lookup_idx ON desk_sessions(tenant_id, zone_id, work_date);

CREATE TABLE desk_daily (
    tenant_id                BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    zone_id                  BIGINT NOT NULL REFERENCES zones(id) ON DELETE CASCADE,
    employee_id              BIGINT REFERENCES employees(id) ON DELETE SET NULL,
    work_date                DATE NOT NULL,
    occupied_seconds         INT NOT NULL DEFAULT 0,
    absent_seconds           INT NOT NULL DEFAULT 0,
    longest_absence_seconds  INT NOT NULL DEFAULT 0,
    sessions_count           INT NOT NULL DEFAULT 0,
    mismatch_seconds         INT NOT NULL DEFAULT 0,
    updated_at               TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (tenant_id, zone_id, work_date)
);
CREATE INDEX desk_daily_employee_idx ON desk_daily(tenant_id, employee_id, work_date);
