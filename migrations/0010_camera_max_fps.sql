-- Per-camera ceiling on the frame rate the pipeline carries. 0 = every frame.
--
-- This column exists because a camera cannot be relied on to honour its own
-- frame-rate setting. The VARITO at the front door advertises a settable
-- 5-30 fps range over ONVIF, accepts SetVideoEncoderConfiguration with
-- FrameRateLimit=10 and answers with no fault, and then continues to send 25.
-- Its web UI reverts the same field the same way. So the rate has to be bounded
-- on this side of the wire or not at all.
--
-- The frames were already being discarded, by `appsink max-buffers=1
-- drop=true`, at the END of the pipeline — after each one had been scaled and
-- converted to BGR at full resolution. This moves the drop to just after the
-- decoder, which discards the same frames without paying for them.
--
-- It does NOT reduce decode cost: inter-frame prediction means the skipped
-- frames are needed to decode the kept ones. It bounds everything after the
-- decoder, which on this board is the more expensive half.
--
-- 0 by default, so every existing camera keeps behaving exactly as before.
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS max_fps integer NOT NULL DEFAULT 0;

-- Below 5 fps a person crosses a doorway between two frames and the tracker
-- has nothing to associate, which breaks identification rather than cheapening
-- it. 0 is the separate, explicit "no limit".
--
-- Guarded, because ADD CONSTRAINT has no IF NOT EXISTS and this file is replayed
-- on every start: an unguarded one aborts the whole migration script, and the
-- process then crash-loops on a schema that is in fact already correct.
DO $$ BEGIN
    ALTER TABLE cameras ADD CONSTRAINT cameras_max_fps_sane
        CHECK (max_fps = 0 OR max_fps BETWEEN 5 AND 60);
EXCEPTION WHEN duplicate_object THEN NULL;
END $$;
