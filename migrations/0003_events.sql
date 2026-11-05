-- Alerts and their delivery.

CREATE TABLE events (
    id              BIGSERIAL PRIMARY KEY,
    tenant_id       BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    camera_id       BIGINT REFERENCES cameras(id) ON DELETE SET NULL,
    module          TEXT NOT NULL,
    severity        TEXT NOT NULL CHECK (severity IN ('info','warning','critical')),
    title           TEXT NOT NULL,
    detail          TEXT NOT NULL DEFAULT '',
    ts              TIMESTAMPTZ NOT NULL,
    zone_id         BIGINT REFERENCES zones(id) ON DELETE SET NULL,
    employee_id     BIGINT REFERENCES employees(id) ON DELETE SET NULL,
    track_id        INT NOT NULL DEFAULT -1,
    snapshot_path   TEXT NOT NULL DEFAULT '',
    crop_path       TEXT NOT NULL DEFAULT '',
    acknowledged_by BIGINT REFERENCES users(id) ON DELETE SET NULL,
    acknowledged_at TIMESTAMPTZ
);
CREATE INDEX events_recent_idx ON events(tenant_id, ts DESC);
CREATE INDEX events_module_idx ON events(tenant_id, module, ts DESC);

CREATE TABLE notification_settings (
    tenant_id        BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    module           TEXT NOT NULL,
    telegram_enabled BOOLEAN NOT NULL DEFAULT TRUE,
    email_enabled    BOOLEAN NOT NULL DEFAULT TRUE,
    min_severity     TEXT NOT NULL DEFAULT 'warning'
                     CHECK (min_severity IN ('info','warning','critical')),
    cooldown_seconds INT NOT NULL DEFAULT 60,
    PRIMARY KEY (tenant_id, module)
);

-- Delivery runs off this table rather than from the alert path directly. A slow
-- mail server must not be able to block a frame thread, and a failed send must
-- not lose the alert - it is a row that can be retried.
CREATE TABLE notification_outbox (
    id          BIGSERIAL PRIMARY KEY,
    tenant_id   BIGINT NOT NULL REFERENCES tenants(id) ON DELETE CASCADE,
    event_id    BIGINT NOT NULL REFERENCES events(id) ON DELETE CASCADE,
    channel     TEXT NOT NULL CHECK (channel IN ('telegram','email')),
    status      TEXT NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending','sent','failed')),
    attempts    INT NOT NULL DEFAULT 0,
    last_error  TEXT NOT NULL DEFAULT '',
    next_try_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    sent_at     TIMESTAMPTZ
);
CREATE INDEX notification_outbox_due_idx
    ON notification_outbox(status, next_try_at) WHERE status = 'pending';
