-- Smart Office — initial schema.
--
-- Every tenant-scoped table carries tenant_id, and every one of them is indexed
-- on it first. That is not only for speed: the repository layer takes TenantId
-- as its first argument with no un-scoped overload, so a query that forgot the
-- filter would not compile, and the index makes the filter free.

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     TEXT PRIMARY KEY,
    applied_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ── tenancy ────────────────────────────────────────────────────────────────

CREATE TABLE tenants (
    id             BIGSERIAL PRIMARY KEY,
    name           TEXT NOT NULL,
    slug           TEXT NOT NULL UNIQUE,
    status         TEXT NOT NULL DEFAULT 'active'
                   CHECK (status IN ('active', 'suspended')),
    plan           TEXT NOT NULL DEFAULT 'standard',
    max_cameras    INT  NOT NULL DEFAULT 8,
    max_employees  INT  NOT NULL DEFAULT 200,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- A SuperAdmin has tenant_id NULL: they belong to the vendor, not to a client.
CREATE TABLE users (
    id             BIGSERIAL PRIMARY KEY,
    tenant_id      BIGINT REFERENCES tenants(id) ON DELETE CASCADE,
    email          TEXT NOT NULL UNIQUE,
    password_hash  TEXT NOT NULL,
    name           TEXT NOT NULL DEFAULT '',
    role           TEXT NOT NULL CHECK (role IN ('superadmin', 'admin', 'user')),
    employee_id    BIGINT,
    active         BOOLEAN NOT NULL DEFAULT TRUE,
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    -- The one invariant that keeps the tenancy model honest: only a superadmin
    -- may be outside a tenant, and a superadmin must never be inside one.
    CONSTRAINT users_tenant_matches_role CHECK (
        (role = 'superadmin' AND tenant_id IS NULL) OR
        (role <> 'superadmin' AND tenant_id IS NOT NULL))
);
CREATE INDEX users_tenant_idx ON users(tenant_id);

CREATE TABLE tenant_settings (
    tenant_id                 BIGINT PRIMARY KEY REFERENCES tenants(id) ON DELETE CASCADE,
    timezone                  TEXT NOT NULL DEFAULT 'Asia/Dhaka',
    office_start              TIME NOT NULL DEFAULT '09:00',
    office_end                TIME NOT NULL DEFAULT '18:00',
    grace_minutes             INT  NOT NULL DEFAULT 15,
    early_leave_grace_minutes INT  NOT NULL DEFAULT 15,
    workdays                  INT[] NOT NULL DEFAULT '{0,1,2,3,4}',  -- 0 = Sunday
    full_day_hours            NUMERIC(4,2) NOT NULL DEFAULT 8.0,
    half_day_hours            NUMERIC(4,2) NOT NULL DEFAULT 4.0,
    telegram_bot_token        TEXT NOT NULL DEFAULT '',
    telegram_chat_id          TEXT NOT NULL DEFAULT '',
    smtp_host                 TEXT NOT NULL DEFAULT '',
    smtp_port                 INT  NOT NULL DEFAULT 587,
    smtp_user                 TEXT NOT NULL DEFAULT '',
    smtp_password             TEXT NOT NULL DEFAULT '',
    smtp_from                 TEXT NOT NULL DEFAULT '',
    smtp_from_name            TEXT NOT NULL DEFAULT 'Smart Office',
    email_to                  TEXT NOT NULL DEFAULT '',
    updated_at                TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ── vision configuration ───────────────────────────────────────────────────

CREATE TABLE cameras (
    id           BIGSERIAL PRIMARY KEY,
    tenant_id    BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    name         TEXT NOT NULL,
    rtsp_main    TEXT NOT NULL DEFAULT '',
    rtsp_sub     TEXT NOT NULL DEFAULT '',
    -- 'sub' is the default and the whole performance story; 'main' exists so a
    -- gate camera whose sub-stream is too small can be switched without a code
    -- change.
    stream_pref  TEXT NOT NULL DEFAULT 'sub' CHECK (stream_pref IN ('sub','main','auto')),
    role         TEXT NOT NULL DEFAULT 'area' CHECK (role IN ('area','gate')),
    face_enabled BOOLEAN NOT NULL DEFAULT FALSE,
    enabled      BOOLEAN NOT NULL DEFAULT TRUE,
    detect_hz    INT NOT NULL DEFAULT 10 CHECK (detect_hz BETWEEN 1 AND 60),
    face_hz      INT NOT NULL DEFAULT 6  CHECK (face_hz BETWEEN 1 AND 60),
    -- Recorded at add time so the UI can warn when a gate camera's sub-stream
    -- is too small for a face to ever be identifiable.
    probed_width  INT NOT NULL DEFAULT 0,
    probed_height INT NOT NULL DEFAULT 0,
    created_at   TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX cameras_tenant_idx ON cameras(tenant_id);

CREATE TABLE employees (
    id          BIGSERIAL PRIMARY KEY,
    tenant_id   BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    emp_code    TEXT NOT NULL,
    name        TEXT NOT NULL,
    department  TEXT NOT NULL DEFAULT '',
    designation TEXT NOT NULL DEFAULT '',
    email       TEXT NOT NULL DEFAULT '',
    phone       TEXT NOT NULL DEFAULT '',
    active      BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (tenant_id, emp_code)
);
CREATE INDEX employees_tenant_idx ON employees(tenant_id);

CREATE TABLE zones (
    id            BIGSERIAL PRIMARY KEY,
    tenant_id     BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    camera_id     BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
    type          TEXT NOT NULL CHECK (type IN ('perimeter','restricted','desk')),
    name          TEXT NOT NULL,
    polygon       JSONB NOT NULL,          -- [[x,y], ...] in SOURCE coordinates
    -- A desk zone is bound to one employee, and the binding persists until an
    -- operator edits or resets it. Nothing clears it automatically.
    employee_id   BIGINT REFERENCES employees(id) ON DELETE SET NULL,
    enabled       BOOLEAN NOT NULL DEFAULT TRUE,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT desk_zone_has_employee CHECK (type <> 'desk' OR employee_id IS NOT NULL)
);
CREATE INDEX zones_tenant_camera_idx ON zones(tenant_id, camera_id);

CREATE TABLE gate_lines (
    id         BIGSERIAL PRIMARY KEY,
    tenant_id  BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    camera_id  BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
    name       TEXT NOT NULL DEFAULT 'Entrance',
    ax REAL NOT NULL, ay REAL NOT NULL,
    bx REAL NOT NULL, by REAL NOT NULL,
    enabled    BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX gate_lines_tenant_camera_idx ON gate_lines(tenant_id, camera_id);

CREATE TABLE camera_modules (
    tenant_id BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    camera_id BIGINT NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
    module    TEXT NOT NULL,
    enabled   BOOLEAN NOT NULL DEFAULT TRUE,
    params    JSONB NOT NULL DEFAULT '{}',
    PRIMARY KEY (camera_id, module)
);

-- ── identity ───────────────────────────────────────────────────────────────

CREATE TABLE face_profiles (
    id          BIGSERIAL PRIMARY KEY,
    tenant_id   BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    employee_id BIGINT NOT NULL REFERENCES employees(id) ON DELETE CASCADE,
    embedding   BYTEA NOT NULL,           -- float32[dim], little-endian
    dim         INT NOT NULL,
    quality     REAL NOT NULL DEFAULT 0,
    yaw         REAL NOT NULL DEFAULT 0,
    face_px     REAL NOT NULL DEFAULT 0,
    image_path  TEXT NOT NULL DEFAULT '',
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX face_profiles_tenant_employee_idx ON face_profiles(tenant_id, employee_id);
