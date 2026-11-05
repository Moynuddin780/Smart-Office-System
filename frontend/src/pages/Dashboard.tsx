import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { api, hms, mediaUrl, today } from '../api'
import { Card, Chip, Ic, KpiCard, SectionTitle } from '../components/ui'
import LiveTile from '../components/LiveTile'

interface Summary { present: number; late: number; absent: number; total: number }
interface DayRow { employee_id: number; name: string; status: string; first_in: string
                   last_out: string; inside_seconds: number }
interface EventRow { id: number; module: string; severity: string; title: string
                     camera_name: string; camera_site: string; camera_location: string
                     ts: string; crop: string }
interface Camera { id: number; name: string; role: string; enabled: boolean; face_enabled: boolean }
interface Health { engines: number; cameras_online: number; cameras_running: number
                   gallery: number; npu_core_utilisation: number[] }

export default function Dashboard() {
  const [sum, setSum] = useState<Summary | null>(null)
  const [rows, setRows] = useState<DayRow[]>([])
  const [events, setEvents] = useState<EventRow[]>([])
  const [cams, setCams] = useState<Camera[]>([])
  const [health, setHealth] = useState<Health | null>(null)
  const [err, setErr] = useState('')

  const load = () => {
    api.get<{ summary: Summary; rows: DayRow[] }>(`/api/attendance/daily?date=${today()}`)
      .then(r => { setSum(r.summary); setRows(r.rows) })
      .catch(e => setErr(String(e.message)))
    api.get<EventRow[]>('/api/events?limit=6').then(setEvents).catch(() => {})
    api.get<Camera[]>('/api/cameras').then(c => setCams(c.filter(x => x.enabled))).catch(() => {})
    fetch('/healthz').then(r => r.json()).then(setHealth).catch(() => {})
  }

  useEffect(() => {
    load()
    const t = setInterval(load, 20000)
    return () => clearInterval(t)
  }, [])

  const inNow = rows.filter(r => r.first_in && !r.last_out).length
  const critical = events.filter(e => e.severity === 'critical').length

  return (
    <>
      <div className="head">
        <div>
          <h1>Today</h1>
          <div className="muted" style={{ fontVariantNumeric: 'tabular-nums' }}>{today()}</div>
        </div>
        {health && (
          <div className="row">
            <Chip kind={health.engines > 0 ? 'ok' : 'bad'}>
              <Ic k="chip" s={11} style={{ display: 'inline-block', verticalAlign: '-1px', marginRight: 4 }} />
              {health.engines} NPU engines
            </Chip>
            <Chip kind={health.cameras_online === health.cameras_running && health.cameras_running > 0 ? 'ok' : 'warn'}>
              {health.cameras_online}/{health.cameras_running} cameras online
            </Chip>
            <Chip kind={health.gallery > 0 ? 'ok' : 'idle'}>{health.gallery} faces enrolled</Chip>
          </div>
        )}
      </div>

      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      <div className="grid cols-4" style={{ marginBottom: 16 }}>
        <KpiCard label="Present" icon="check" tone="ok" value={sum?.present ?? '—'}
                 sub="arrived within the grace window" />
        <KpiCard label="Late" icon="clock" tone="warn" value={sum?.late ?? '—'}
                 sub="arrived after it" />
        <KpiCard label="Absent" icon="users" tone="muted" value={sum?.absent ?? '—'}
                 sub="no entry recorded today" />
        <KpiCard label="In the building" icon="building" value={inNow}
                 sub="entered and not yet left" />
      </div>

      {cams.length > 0 && (
        <div style={{ marginBottom: 16 }}>
          <SectionTitle right={<Link to="/live">Open the wall →</Link>}>Cameras</SectionTitle>
          <div className="grid" style={{ gridTemplateColumns: 'repeat(auto-fill,minmax(240px,1fr))' }}>
            {/* EVERY enabled camera, not a fixed number.
                This was capped at four while the roster was four, and the cap
                then quietly hid the fifth camera the day one was added — a
                dashboard that silently omits a camera is worse than one that is
                a row longer. The grid already reflows, and the tiles poll at
                3 fps, which is the cheapest rate in the product. */}
            {cams.map(c => (
              <Card key={c.id} style={{ padding: 0, overflow: 'hidden' }}>
                <LiveTile cameraId={c.id} fps={3}
                          style={{ width: '100%', aspectRatio: '16 / 9' }} />
                <div style={{ padding: '8px 11px', display: 'flex', alignItems: 'center',
                              justifyContent: 'space-between', gap: 8 }}>
                  <span style={{ fontSize: 12.5, fontWeight: 600, whiteSpace: 'nowrap',
                                 overflow: 'hidden', textOverflow: 'ellipsis' }}>{c.name}</span>
                  {c.role === 'gate' && <Chip kind="info">gate</Chip>}
                </div>
              </Card>
            ))}
          </div>
        </div>
      )}

      <div className="grid cols-2">
        <Card className="flush">
          <SectionTitle right={<Link to="/attendance">All →</Link>}>
            <span style={{ paddingLeft: 18 }}>Latest arrivals</span>
          </SectionTitle>
          <table>
            <thead><tr><th>Employee</th><th>First in</th><th>Working</th><th>Status</th></tr></thead>
            <tbody>
              {rows.filter(r => r.first_in).slice(0, 8).map(r => (
                <tr key={r.employee_id}>
                  <td>{r.name}</td>
                  <td>{r.first_in}</td>
                  <td>{hms(r.inside_seconds)}</td>
                  <td><span className={`pill ${r.status}`}>{r.status.replace('_', ' ')}</span></td>
                </tr>))}
              {rows.filter(r => r.first_in).length === 0 &&
                <tr><td colSpan={4} className="muted">Nobody has been recognised yet today.</td></tr>}
            </tbody>
          </table>
        </Card>

        <Card className="flush">
          <SectionTitle right={<Link to="/events">All →</Link>}>
            <span style={{ paddingLeft: 18 }}>
              Recent alerts{critical > 0 && <> · <span style={{ color: 'var(--crit)' }}>{critical} critical</span></>}
            </span>
          </SectionTitle>
          <table>
            <thead><tr><th style={{ width: 60 }}></th><th>When</th><th>Alert</th><th>Camera</th></tr></thead>
            <tbody>
              {events.map(e => (
                <tr key={e.id}>
                  <td>{e.crop && <img src={mediaUrl(`/api/snapshots?path=${encodeURIComponent(e.crop)}`)}
                                      alt="" style={{ width: 46, height: 46, objectFit: 'cover',
                                                      borderRadius: 'var(--r-sm)', display: 'block' }} />}</td>
                  <td className="muted">{e.ts.slice(11, 16)}</td>
                  <td><span className={`pill ${e.severity}`}>{e.severity}</span> {e.title}</td>
                  <td className="muted">
                    {e.camera_name}
                    {e.camera_location && (
                      <div style={{ fontSize: 11, color: 'var(--text-3)' }}>{e.camera_location}</div>
                    )}
                  </td>
                </tr>))}
              {events.length === 0 && <tr><td colSpan={4} className="muted">No alerts.</td></tr>}
            </tbody>
          </table>
        </Card>
      </div>
    </>
  )
}
