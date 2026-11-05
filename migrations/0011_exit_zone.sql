-- A zone somebody leaves THROUGH.
--
-- Attendance records an `in` whenever a face is recognised, and nothing in the
-- system has ever recorded an `out`. The day roll-up has always known how to
-- pair them — inside_seconds, break_seconds and entries are all computed from
-- alternating punches — so every break has been zero and every day closed by
-- assumed_out, not because the arithmetic was missing but because half its
-- input was.
--
-- 'exit' is a distinct kind rather than a flag on 'perimeter' because the two
-- mean opposite things. Standing inside a restricted zone IS the alarm;
-- standing in an exit zone is what everybody does on the way home, and only
-- the leaving is recorded.
ALTER TABLE zones DROP CONSTRAINT IF EXISTS zones_type_check;
ALTER TABLE zones ADD CONSTRAINT zones_type_check
    CHECK (type IN ('perimeter','restricted','desk','exit'));
