import { useCallback, useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api } from '../api'
import { Btn, Card, Chip, Empty, Ic } from '../components/ui'

interface Camera { id: number; name: string; role: string; face_enabled: boolean
                   enabled: boolean; site: string; location: string }
interface Module {
  id: string; name: string; blurb: string; needs: string
  enabled: boolean; active: boolean; blocker: string
}

const ICON: Record<string, string> = {
  perimeter: 'alert', fall: 'warn', desk: 'desk',
  unauthorised: 'face', attendance: 'clock',
}

/**
 * Which AI modules run where.
 *
 * The distinction this page exists to make is between ENABLED and ACTIVE. A
 * module can be switched on and still be doing nothing because the thing it
 * needs — a zone, a gate line, face recognition — is not there. A page that
 * showed only the switch would let somebody believe their building was being
 * watched when it was not.
 */
export default function Modules() {
  const [cams, setCams] = useState<Camera[] | null>(null)
  const [sel, setSel] = useState<number | null>(null)
  const [mods, setMods] = useState<Module[] | null>(null)
  const [busy, setBusy] = useState('')
  const [err, setErr] = useState('')

  useEffect(() => {
    api.get<Camera[]>('/api/cameras')
      .then(c => { const e = c.filter(x => x.enabled); setCams(e); if (e.length) setSel(e[0].id) })
      .catch(e => setErr(String(e.message)))
  }, [])

  const load = useCallback(() => {
    if (!sel) return
    api.get<Module[]>(`/api/cameras/${sel}/modules`).then(setMods).catch(e => setErr(String(e.message)))
  }, [sel])
  useEffect(() => { load() }, [load])

  const toggle = async (m: Module) => {
    if (!sel) return
    setBusy(m.id)
    try { await api.put(`/api/cameras/${sel}/modules`, { [m.id]: !m.enabled }); load() }
    catch (e) { setErr(e instanceof Error ? e.message : 'could not save') }
    finally { setBusy('') }
  }

  const cam = cams?.find(c => c.id === sel)

  return (
    <>
      <div className="head">
        <div>
          <h1>AI modules</h1>
          <div className="muted">
            What runs on each camera — and, where it is switched on but doing nothing, why.
            You can also assign modules while adding or editing a camera under{' '}
            <Link to="/cameras">Cameras &amp; zones</Link>.
          </div>
        </div>
        {cams && cams.length > 0 && (
          <select value={sel ?? ''} onChange={e => setSel(Number(e.target.value))}
                  style={{ width: 240 }}>
            {cams.map(c => (
              <option key={c.id} value={c.id}>
                {c.name}{c.location ? ` — ${c.location}` : ''}
              </option>))}
          </select>
        )}
      </div>

      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      {cams === null ? <div className="muted">Loading…</div>
       : cams.length === 0 ? (
        <Empty icon="modules" title="No cameras yet"
               text="Modules run on cameras. Add one to configure what it watches for."
               action={<Link className="btn" to="/cameras">Add a camera</Link>} />
       ) : (
        <div className="grid" style={{ gridTemplateColumns: 'repeat(auto-fill,minmax(330px,1fr))' }}>
          {(mods ?? []).map(m => (
            <Card key={m.id} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
              <div style={{ display: 'flex', alignItems: 'flex-start', justifyContent: 'space-between', gap: 10 }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 9, minWidth: 0 }}>
                  <span style={{ color: m.active ? 'var(--accent)' : 'var(--text-3)' }}>
                    <Ic k={ICON[m.id] || 'modules'} s={18} />
                  </span>
                  <div style={{ fontWeight: 650, fontSize: 14 }}>{m.name}</div>
                </div>
                {m.active ? <Chip kind="ok">active</Chip>
                 : m.enabled ? <Chip kind="warn">idle</Chip>
                 : <Chip kind="idle">off</Chip>}
              </div>

              <div className="muted" style={{ fontSize: 12.5 }}>{m.blurb}</div>

              {m.enabled && m.blocker && (
                <div className="note" style={{ fontSize: 12, padding: '8px 10px' }}>
                  Switched on, but doing nothing: {m.blocker}.{' '}
                  {sel && <Link to={`/cameras/${sel}`}>Configure this camera</Link>}
                </div>
              )}

              <div style={{ fontSize: 11.5, color: 'var(--text-3)' }}>
                <b style={{ fontWeight: 600 }}>Needs:</b> {m.needs}
              </div>

              <div className="row" style={{ marginTop: 'auto' }}>
                <Btn kind={m.enabled ? 'ghost' : 'primary'} disabled={busy === m.id}
                     icon={m.enabled ? 'x' : 'check'} onClick={() => toggle(m)}>
                  {m.enabled ? 'Switch off' : 'Switch on'}
                </Btn>
              </div>
            </Card>
          ))}
        </div>
      )}

      {cam && (
        <Card style={{ marginTop: 16 }}>
          <div className="muted" style={{ fontSize: 12.5 }}>
            <b style={{ fontWeight: 600, color: 'var(--text)' }}>{cam.name}</b> — role{' '}
            <b style={{ fontWeight: 600 }}>{cam.role}</b>, face recognition{' '}
            <b style={{ fontWeight: 600 }}>{cam.face_enabled ? 'on' : 'off'}</b>. The role is a
            label you choose; what a module needs is the real thing — attendance needs face
            recognition and a <Link to={`/cameras/${cam.id}`}>gate line</Link> to count people
            across, and the others need the zone they watch.
          </div>
        </Card>
      )}
    </>
  )
}
