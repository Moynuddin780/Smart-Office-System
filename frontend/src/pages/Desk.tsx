import { useEffect, useState } from 'react'
import { api, hms, today } from '../api'

interface DeskRow {
  zone_id: number; zone_name: string; employee_id: number; employee_name: string
  occupied_seconds: number; absent_seconds: number; longest_absence_seconds: number
  sessions: number; identity_mismatch_seconds: number
  employee_present: boolean
}

export default function Desk() {
  const [date, setDate] = useState(today())
  const [rows, setRows] = useState<DeskRow[]>([])
  const [err, setErr] = useState('')

  useEffect(() => {
    setErr('')
    api.get<{ rows: DeskRow[] }>(`/api/desk/daily?date=${date}`)
      .then(r => setRows(r.rows)).catch(e => setErr(String(e.message)))
  }, [date])

  const mismatch = rows.some(r => r.identity_mismatch_seconds > 0)

  return (
    <>
      <div className="head">
        <div><h1>Desk performance</h1>
          <div className="muted">How long each desk zone was occupied.</div></div>
        <input type="date" value={date} onChange={e => setDate(e.target.value)} style={{ width: 170 }} />
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}
      {mismatch && (
        <div className="note" style={{ marginBottom: 14 }}>
          A desk marked <b>mismatch</b> had somebody other than its assigned employee recognised
          at it. The time is still counted against the assignment — this is flagged so a report
          does not credit the wrong person in silence.
        </div>
      )}
      <div className="card flush">
        <table>
          <thead><tr>
            <th>Desk</th><th>Assigned to</th><th>At desk</th><th>Away</th>
            <th>Longest absence</th><th>Sessions</th><th></th>
          </tr></thead>
          <tbody>
            {rows.map(r => (
              <tr key={r.zone_id}>
                <td>{r.zone_name}</td>
                <td>{r.employee_name || <span className="muted">unassigned</span>}</td>
                <td>{hms(r.occupied_seconds)}</td>
                <td className="muted">{hms(r.absent_seconds)}</td>
                <td className="muted">{hms(r.longest_absence_seconds)}</td>
                <td className="muted">{r.sessions || '—'}</td>
                <td>{!r.employee_present && r.occupied_seconds > 0 &&
                  <span className="pill warning" title={
                    'Somebody sat here, but ' + (r.employee_name || 'this employee') +
                    ' was never recorded arriving today — so this time is probably not theirs.'}>
                    never arrived
                  </span>}
                {r.identity_mismatch_seconds > 0 &&
                  <span className="pill warning">mismatch {hms(r.identity_mismatch_seconds)}</span>}</td>
              </tr>))}
            {rows.length === 0 &&
              <tr><td colSpan={7} className="muted">
                No desk zones yet. Open a camera and draw one to start tracking time at a desk.
              </td></tr>}
          </tbody>
        </table>
      </div>
    </>
  )
}
