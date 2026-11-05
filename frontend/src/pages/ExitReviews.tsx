import { useEffect, useState } from 'react'
import { api, mediaUrl, today } from '../api'

/// Snapshots are served BY THE API, not as static files, so a stored path has
/// to be handed to that endpoint rather than used as a URL. Linking the bare
/// path resolved it against the current page and asked for
/// /exit-reviews/live-proof-co/... — which is why the face never appeared.
const snap = (path: string) =>
  mediaUrl(`/api/snapshots?path=${encodeURIComponent(path)}`)

interface Review {
  id: number; camera_id: number; camera: string; time: string; work_date: string
  guess_employee_id: number; guess_name: string; guess_code: string
  score: number; margin: number; weight: number
  crop: string; frame: string
}
interface Employee { id: number; emp_code: string; name: string }

/**
 * Exits the system watched but could not name.
 *
 * A QUEUE OF QUESTIONS, NOT OF ALERTS. Somebody walking out is the hardest face
 * on any camera — furthest away, turning, and with no second chance once the
 * door closes — so the evidence often falls short of the bar for naming them.
 * The alternatives were both bad: drop it, and the exit disappears along with
 * the only look the system will ever get; or write the best guess as fact, and
 * put the wrong name on somebody's timesheet.
 *
 * So the question is kept with its evidence, and a person answers it. The score
 * and weight are shown rather than hidden, because "looked like Eshita at 0.41
 * on one glance" and "looked like Eshita at 0.62 on four" deserve different
 * amounts of trust from the reviewer.
 */
export default function ExitReviews() {
  const [date, setDate] = useState(today())
  const [rows, setRows] = useState<Review[]>([])
  const [people, setPeople] = useState<Employee[]>([])
  const [err, setErr] = useState('')
  const [busy, setBusy] = useState(0)

  const load = () => {
    setErr('')
    api.get<{ rows: Review[] }>(`/api/exit-reviews?date=${date}`)
      .then(r => setRows(r.rows)).catch(e => setErr(String(e.message)))
  }
  useEffect(load, [date])
  useEffect(() => {
    api.get<Employee[]>('/api/employees').then(setPeople).catch(() => {})
  }, [])

  const answer = async (r: Review, employee_id: number) => {
    setBusy(r.id); setErr('')
    try {
      await api.post(`/api/exit-reviews/${r.id}/resolve`, { employee_id })
      load()
    } catch (e: any) {
      setErr(String(e.message))
    } finally {
      setBusy(0)
    }
  }

  return (
    <>
      <div className="head">
        <div>
          <h1>Unnamed exits</h1>
          <div className="muted">People seen leaving that the system could not identify.</div>
        </div>
        <input type="date" value={date} onChange={e => setDate(e.target.value)}
               style={{ width: 170 }} />
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      <div className="note" style={{ marginBottom: 14 }}>
        Confirming one records the departure <b>at the time they were seen</b>, not now, so
        the break lands where it actually happened. <b>Not them</b> discards the exit —
        use it for a visitor, or when the zone caught somebody who never left.
      </div>

      {rows.length === 0 ? (
        <div className="card"><div className="muted">
          Nothing to answer for this day.
        </div></div>
      ) : (
        <div className="card flush">
          <table>
            <thead><tr>
              <th style={{ width: 90 }}>Face</th><th>Time</th><th>Camera</th>
              <th>Looked most like</th><th>Evidence</th><th style={{ width: 260 }}>Answer</th>
            </tr></thead>
            <tbody>
              {rows.map(r => (
                <tr key={r.id}>
                  <td>
                    {r.crop
                      ? <a href={snap(r.frame || r.crop)} target="_blank" rel="noreferrer">
                          <img src={snap(r.crop)} alt="face at the door"
                               style={{ width: 72, height: 72, objectFit: 'cover',
                                        borderRadius: 6, display: 'block' }} />
                        </a>
                      : <span className="muted">no face</span>}
                  </td>
                  <td>{r.time}</td>
                  <td className="muted">{r.camera}</td>
                  <td>
                    {r.guess_employee_id
                      ? <>{r.guess_name} <span className="muted">{r.guess_code}</span></>
                      : <span className="muted">nobody — no face was read</span>}
                  </td>
                  <td className="muted" style={{ fontSize: 12, whiteSpace: 'nowrap' }}>
                    {r.guess_employee_id
                      ? <>score {r.score} · margin {r.margin} · weight {r.weight}</>
                      : '—'}
                  </td>
                  <td>
                    <div className="row" style={{ gap: 6, flexWrap: 'wrap' }}>
                      {r.guess_employee_id > 0 && (
                        <button disabled={busy === r.id}
                                onClick={() => answer(r, r.guess_employee_id)}>
                          Yes, {r.guess_name.split(' ')[0]}
                        </button>
                      )}
                      <select defaultValue="" disabled={busy === r.id}
                              onChange={e => e.target.value && answer(r, Number(e.target.value))}>
                        <option value="">Someone else…</option>
                        {people.map(p => (
                          <option key={p.id} value={p.id}>{p.emp_code} — {p.name}</option>
                        ))}
                      </select>
                      <button className="ghost" disabled={busy === r.id}
                              onClick={() => answer(r, 0)}>Not them</button>
                    </div>
                  </td>
                </tr>))}
            </tbody>
          </table>
        </div>
      )}
    </>
  )
}
