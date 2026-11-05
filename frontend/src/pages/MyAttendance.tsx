import { useEffect, useState } from 'react'
import { api, hms, today } from '../api'

interface DayRow {
  department: string   // the range view carries the date in this slot
  first_in: string; last_out: string; status: string
  inside_seconds: number; break_seconds: number; entries: number; assumed_out: boolean
}

export default function MyAttendance() {
  const [rows, setRows] = useState<DayRow[]>([])
  const [err, setErr] = useState('')
  const [from, setFrom] = useState(() => {
    const d = new Date(); d.setDate(d.getDate() - 30)
    return d.toISOString().slice(0, 10)
  })
  const [to, setTo] = useState(today())

  useEffect(() => {
    api.get<{ rows: DayRow[] }>(`/api/me/attendance?from=${from}&to=${to}`)
      .then(r => setRows(r.rows)).catch(e => setErr(String(e.message)))
  }, [from, to])

  const total = rows.reduce((a, r) => a + r.inside_seconds, 0)

  return (
    <>
      <div className="head">
        <div><h1>My attendance</h1><div className="muted">{hms(total)} worked over this range.</div></div>
        <div className="row">
          <input type="date" value={from} onChange={e => setFrom(e.target.value)} style={{ width: 160 }} />
          <input type="date" value={to} onChange={e => setTo(e.target.value)} style={{ width: 160 }} />
        </div>
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}
      <div className="card flush">
        <table>
          <thead><tr><th>Date</th><th>In</th><th>Out</th><th>Working</th><th>Break</th><th>Status</th></tr></thead>
          <tbody>
            {rows.map((r, i) => (
              <tr key={i}>
                <td>{r.department}</td>
                <td>{r.first_in || '—'}</td>
                <td>{r.last_out || '—'}{r.assumed_out && <span className="muted" style={{ fontSize: 11 }}> still in</span>}</td>
                <td>{hms(r.inside_seconds)}</td>
                <td className="muted">{hms(r.break_seconds)}</td>
                <td><span className={`pill ${r.status}`}>{r.status.replace('_', ' ')}</span></td>
              </tr>))}
            {rows.length === 0 && <tr><td colSpan={6} className="muted">No records in this range.</td></tr>}
          </tbody>
        </table>
      </div>
    </>
  )
}
