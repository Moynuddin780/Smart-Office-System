import { useEffect, useState } from 'react'
import { api, hms, mediaUrl, today } from '../api'

interface DayRow {
  employee_id: number; emp_code: string; name: string; department: string
  first_in: string; last_out: string; status: string
  inside_seconds: number; break_seconds: number; entries: number; assumed_out: boolean
}

export default function Attendance() {
  const [date, setDate] = useState(today())
  const [rows, setRows] = useState<DayRow[]>([])
  const [err, setErr] = useState('')
  const [busy, setBusy] = useState(false)
  const [note, setNote] = useState('')

  const load = () => {
    setErr('')
    api.get<{ rows: DayRow[] }>(`/api/attendance/daily?date=${date}`)
      .then(r => setRows(r.rows)).catch(e => setErr(String(e.message)))
  }
  useEffect(load, [date])

  // Re-read this day's totals under the CURRENT rules.
  //
  // A day's figures are worked out when an event arrives and then stored, so a
  // change to how days are counted reaches new events only — every row already
  // written keeps the arithmetic of the rule it was written under. Without this
  // the only way to pick up a corrected rule was to wait for the person to walk
  // past a camera again, which never happens for a day that is over.
  const recompute = async () => {
    setBusy(true); setErr(''); setNote('')
    try {
      const r = await api.post<{ rows: number }>(
        `/api/admin/recompute-days?from=${date}&to=${date}`, {})
      setNote(`${r.rows} ${r.rows === 1 ? 'row' : 'rows'} recalculated`)
      load()
    } catch (e: any) {
      setErr(String(e.message))
    } finally {
      setBusy(false)
    }
  }

  const anyAssumed = rows.some(r => r.assumed_out)

  return (
    <>
      <div className="head">
        <div><h1>Attendance</h1><div className="muted">First in, last out, and time actually inside.</div></div>
        <div className="row">
          <input type="date" value={date} onChange={e => setDate(e.target.value)} style={{ width: 170 }} />
          <button className="ghost" onClick={recompute} disabled={busy}>
            {busy ? 'Recalculating…' : 'Recalculate'}
          </button>
          <a className="btn" href={mediaUrl(`/api/reports/attendance.csv?date=${date}`)}>Export CSV</a>
        </div>
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}
      {note && <div className="note" style={{ marginBottom: 14 }}>{note}</div>}
      {anyAssumed && (
        <div className="note" style={{ marginBottom: 14 }}>
          <b>Last out</b> is the last time somebody was recorded LEAVING, and is empty when
          they never were. Rows marked <b>still in</b> have no closing exit, so working time
          is counted up to now (or to the end of the office day, whichever came first) —
          that part is an assumption, and the departure column stays empty rather than
          reporting one that did not happen.
        </div>
      )}
      <div className="card flush">
        <table>
          <thead><tr>
            <th>Code</th><th>Employee</th><th>Department</th><th>First in</th><th>Last out</th>
            <th>Working time</th><th>Break</th><th>In/out</th><th>Status</th>
          </tr></thead>
          <tbody>
            {rows.map(r => (
              <tr key={r.employee_id}>
                <td className="muted mono">{r.emp_code}</td>
                <td>{r.name}</td>
                <td className="muted">{r.department}</td>
                <td>{r.first_in || '—'}</td>
                <td>{r.last_out || '—'}{r.assumed_out && <span className="muted" style={{ fontSize: 11 }}> still in</span>}</td>
                <td>{hms(r.inside_seconds)}</td>
                <td className="muted">{hms(r.break_seconds)}</td>
                <td className="muted">{r.entries || '—'}</td>
                <td><span className={`pill ${r.status}`}>{r.status.replace('_', ' ')}</span></td>
              </tr>))}
            {rows.length === 0 && <tr><td colSpan={9} className="muted">No employees yet.</td></tr>}
          </tbody>
        </table>
      </div>
    </>
  )
}
