import { Ic } from './ui'

export interface ModuleRow {
  id: string; name: string; blurb: string; needs: string
  draws: string           // "zone" | "desk" | "line" | ""
  wants_face: boolean
  enabled: boolean; active: boolean; blocker: string
}

const ICON: Record<string, string> = {
  perimeter: 'alert', fall: 'warn', desk: 'desk',
  unauthorised: 'face', attendance: 'clock',
}

const DRAWS: Record<string, string> = {
  zone: 'needs a zone drawn',
  desk: 'needs a desk zone bound to an employee',
  line: 'needs a gate line drawn',
}

/**
 * Which modules run on one camera.
 *
 * This sits INSIDE the camera form rather than on a page of its own, because
 * that is the question an operator is answering while they add a camera —
 * "what is this one watching for?" — and splitting it across two screens meant
 * every new camera silently ran the defaults until somebody went looking for
 * the other screen.
 *
 * Each chip says what its module still needs. That matters more than it looks:
 * a module can be switched on and be completely silent because the zone it
 * watches was never drawn, and a row of plain on/off switches cannot tell you
 * that. What each module needs comes from the server catalogue, so this cannot
 * disagree with the build about which modules want a shape on the frame.
 */
export default function ModulePicker(
  { rows, value, onChange, cameraId }:
  { rows: ModuleRow[] | null
    value: Record<string, boolean>
    onChange: (v: Record<string, boolean>) => void
    cameraId?: number },
) {
  if (!rows) return <div className="muted">Loading modules…</div>

  const on = (m: ModuleRow) => value[m.id] ?? m.enabled
  const count = rows.filter(on).length
  const toggle = (m: ModuleRow) => onChange({ ...value, [m.id]: !on(m) })

  return (
    <div>
      <div className="stat-label" style={{ marginBottom: 8 }}>
        Assign AI modules ({count}/{rows.length})
      </div>
      <div className="grid" style={{ gridTemplateColumns: 'repeat(auto-fill,minmax(232px,1fr))', gap: 8 }}>
        {rows.map(m => {
          const active = on(m)
          return (
            <button key={m.id} type="button" onClick={() => toggle(m)}
                    aria-pressed={active}
                    className={`mod-chip${active ? ' on' : ''}`}>
              <span className="mod-ic"><Ic k={ICON[m.id] || 'modules'} s={16} /></span>
              <span style={{ minWidth: 0 }}>
                <span className="mod-name">{m.name}</span>
                <span className="mod-sub">
                  {m.draws ? DRAWS[m.draws] : 'nothing to draw'}
                  {m.wants_face && ' · needs face recognition'}
                </span>
              </span>
              {active && <span className="mod-tick"><Ic k="check" s={14} /></span>}
            </button>
          )
        })}
      </div>

      {/* Only shown where it can be acted on — an unsaved camera has nowhere to
          draw yet, so pointing at a drawing page would be a dead end. */}
      {cameraId !== undefined && rows.some(m => on(m) && m.blocker) && (
        <div className="note" style={{ marginTop: 10 }}>
          <b>Switched on but doing nothing:</b>
          <ul style={{ margin: '6px 0 0 18px', padding: 0 }}>
            {rows.filter(m => on(m) && m.blocker).map(m => (
              <li key={m.id} style={{ marginBottom: 2 }}>{m.name} — {m.blocker}.</li>
            ))}
          </ul>
        </div>
      )}
    </div>
  )
}
