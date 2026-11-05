-- Custom camera roles.
--
-- `role` started as a closed set of two values enforced by a CHECK. That was
-- wrong for the product: a site wants to call a camera "reception", "warehouse
-- gate" or "server room", and refusing that buys nothing, because the engine
-- does not branch on the role. Behaviour comes from `camera_modules` (which
-- modules run on this camera) and from whether a gate LINE has been drawn —
-- the role is a label the operator reads.
--
-- The one name that still carries meaning is 'gate', which the UI uses to warn
-- about sub-stream resolution before somebody wires up a camera that can never
-- recognise anybody. That is a warning, not a constraint, so it does not belong
-- in the schema either.
--
-- Emptiness is still refused: a blank role would render as a blank pill.
ALTER TABLE cameras DROP CONSTRAINT IF EXISTS cameras_role_check;
ALTER TABLE cameras ADD CONSTRAINT cameras_role_not_blank
    CHECK (length(btrim(role)) > 0);
