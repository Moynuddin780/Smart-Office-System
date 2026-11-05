-- Per-camera limit on how big a frame the pipeline carries.
--
-- Face recognition needs pixels on the FACE, and how many pixels that takes
-- depends entirely on where the camera is: a desk camera a metre away shows a
-- 300 px face on a 2688 px frame, while a corridor camera shows 25 px on the
-- same frame. One shared cap therefore either starves the far camera or makes
-- the near one carry four times the pixels it has any use for — and the pixels
-- are not free, they are copied through pose, head crop, overlay and JPEG on
-- every frame.
--
-- The long side, in pixels. The short side follows from the stream's own aspect
-- ratio, so this is a bound and never a shape.
ALTER TABLE cameras ADD COLUMN IF NOT EXISTS max_px integer NOT NULL DEFAULT 1000;

-- A floor that means something: below roughly 320 px of frame a face cannot
-- reach the 24 px where matching starts, at any distance.
ALTER TABLE cameras ADD CONSTRAINT cameras_max_px_sane
    CHECK (max_px BETWEEN 320 AND 4096);
