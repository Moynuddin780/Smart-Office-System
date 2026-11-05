import { useEffect, useRef, useState } from 'react'
import { api, mediaUrl } from '../api'

interface EventRow {
  id: number; module: string; severity: string; title: string; detail: string
  ts: string; camera_name: string; camera_site: string; camera_location: string
  snapshot: string; crop: string; acknowledged: boolean
}

const MODULES = ['', 'perimeter', 'fall', 'unauthorised', 'desk', 'attendance']

export default function Events() {
  const [rows, setRows] = useState<EventRow[]>([])
  const [module, setModule] = useState('')
  const [err, setErr] = useState('')
  const [live, setLive] = useState(false)
  const wsRef = useRef<WebSocket | null>(null)

  const load = () => api.get<EventRow[]>(`/api/events?limit=200${module ? `&module=${module}` : ''}`)
    .then(setRows).catch(e => setErr(String(e.message)))
  useEffect(() => { load() }, [module])

  useEffect(() => {
    // The live feed is a convenience on top of the list, not a replacement:
    // if the socket cannot connect, the page still works by reloading.
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = new WebSocket(`${proto}://${location.host}${mediaUrl('/ws/events')}`)
    ws.onopen = () => setLive(true)
    ws.onclose = () => setLive(false)
    ws.onerror = () => setLive(false)
    ws.onmessage = () => load()
    wsRef.current = ws
    return () => ws.close()
  }, [])

  return (
    <>
      <div className="head">
        <div><h1>Alerts</h1>
          <div className="muted">
            {live ? 'Live — updating as events arrive.' : 'Not connected to the live feed; reload to refresh.'}
          </div></div>
        <select value={module} onChange={e => setModule(e.target.value)} style={{ width: 200 }}>
          {MODULES.map(m => <option key={m} value={m}>{m || 'All modules'}</option>)}
        </select>
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}
      <div className="card flush">
        <table>
          <thead><tr><th style={{ width: 96 }}></th><th>When</th><th>Alert</th><th>Camera</th><th></th></tr></thead>
          <tbody>
            {rows.map(e => (
              <tr key={e.id}>
                <td>{e.crop && <img src={mediaUrl(`/api/snapshots?path=${encodeURIComponent(e.crop)}`)}
                                    alt="" style={{ width: 84, borderRadius: 6 }} />}</td>
                <td className="muted" style={{ whiteSpace: 'nowrap' }}>{e.ts.replace('T', ' ').slice(0, 16)}</td>
                <td>
                  <span className={`pill ${e.severity}`}>{e.severity}</span>{' '}
                  <b>{e.title}</b>
                  <div className="muted" style={{ fontSize: 12 }}>{e.detail}</div>
                </td>
                <td className="muted">
                  {e.camera_name}
                  {(e.camera_location || e.camera_site) && (
                    <div style={{ fontSize: 11, color: 'var(--text-3)', marginTop: 1 }}>
                      {[e.camera_location, e.camera_site].filter(Boolean).join(' · ')}
                    </div>
                  )}
                </td>
                <td>{e.acknowledged
                  ? <span className="muted">acknowledged</span>
                  : <button className="ghost" onClick={async () => {
                      await api.post(`/api/events/${e.id}/ack`); load()
                    }}>Acknowledge</button>}</td>
              </tr>))}
            {rows.length === 0 && <tr><td colSpan={5} className="muted">No alerts.</td></tr>}
          </tbody>
        </table>
      </div>
    </>
  )
}
