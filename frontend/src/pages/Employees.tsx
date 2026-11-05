import { useEffect, useRef, useState } from 'react'
import { Link } from 'react-router-dom'
import { api } from '../api'
import { Chip, Ic } from '../components/ui'

interface Employee {
  id: number; emp_code: string; name: string; department: string
  designation: string; email: string; phone: string; active: boolean; face_profiles: number
}
interface PhotoResult { filename: string; accepted: boolean; reason?: string; yaw: number; face_px: number }

const blank = { emp_code: '', name: '', department: '', designation: '', email: '', phone: '', active: true }

export default function Employees() {
  const [rows, setRows] = useState<Employee[] | null>(null)
  const [form, setForm] = useState({ ...blank })
  const [open, setOpen] = useState(false)
  const [editing, setEditing] = useState<number | null>(null)
  const [err, setErr] = useState('')
  const [enrolling, setEnrolling] = useState<number | null>(null)
  const [results, setResults] = useState<PhotoResult[] | null>(null)
  const fileRef = useRef<HTMLInputElement>(null)

  const load = () => api.get<Employee[]>('/api/employees').then(setRows)
    .catch(e => setErr(String(e.message)))
  useEffect(() => { load() }, [])

  const startAdd = () => { setForm({ ...blank }); setEditing(null); setOpen(true); setErr('') }
  const startEdit = (e: Employee) => {
    setForm({
      emp_code: e.emp_code, name: e.name, department: e.department,
      designation: e.designation, email: e.email, phone: e.phone, active: e.active,
    })
    setEditing(e.id); setOpen(true); setErr('')
    window.scrollTo({ top: 0, behavior: 'smooth' })
  }
  const close = () => { setOpen(false); setEditing(null); setForm({ ...blank }); setErr('') }

  const save = async (ev: React.FormEvent) => {
    ev.preventDefault()
    setErr('')
    if (!form.name.trim()) { setErr('The employee needs a name.'); return }
    if (!form.emp_code.trim()) { setErr('The employee needs a code — it is what attendance reports key on.'); return }
    try {
      if (editing === null) await api.post('/api/employees', form)
      else await api.patch(`/api/employees/${editing}`, form)
      close(); load()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not save the employee') }
  }

  const setActive = async (e: Employee, active: boolean) => {
    setErr('')
    try { await api.patch(`/api/employees/${e.id}`, { active }); load() }
    catch (x) { setErr(x instanceof Error ? x.message : 'could not change the employee') }
  }

  const remove = async (e: Employee) => {
    // Deleting an employee CASCADES to attendance_events and attendance_days —
    // their whole history goes with them, and no undo exists. Deactivating is
    // almost always what somebody actually wants, so say so here rather than
    // letting a two-word confirm stand in for that difference.
    const ok = confirm(
      `Delete ${e.name} (${e.emp_code}) permanently?\n\n` +
      `THIS ALSO DELETES, AND CANNOT BE UNDONE:\n` +
      `  • every attendance record ever stored for them\n` +
      `  • their ${e.face_profiles} enrolled face photo(s)\n\n` +
      `Kept, but no longer linked to anybody:\n` +
      `  • past alerts, desk sessions, and any desk zone drawn for them\n\n` +
      `If they have simply left or are on long leave, press Cancel and use ` +
      `Deactivate instead — that stops recognition and keeps the history.`)
    if (!ok) return
    setErr('')
    try {
      await api.del(`/api/employees/${e.id}`)
      if (editing === e.id) close()
      load()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not delete the employee') }
  }

  const enrol = async (employeeId: number, files: FileList) => {
    setErr(''); setResults(null); setEnrolling(employeeId)
    try {
      const fd = new FormData()
      for (const f of Array.from(files)) fd.append('photos', f)
      const r = await api.upload<{ photos: PhotoResult[] }>(`/api/employees/${employeeId}/enroll`, fd)
      setResults(r.photos)
      load()
    } catch (x) { setErr(x instanceof Error ? x.message : 'enrolment failed') }
    finally { setEnrolling(null) }
  }

  return (
    <>
      <div className="head">
        <div><h1>Employees</h1>
          <div className="muted">
            The roster. Faces are enrolled in the <Link to="/faces">face registry</Link>, which
            also shows whether each profile is usable.
          </div></div>
        {open ? <button className="ghost" onClick={close}>Cancel</button>
              : <button onClick={startAdd}>Add employee</button>}
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      {open && (
        <form className="card" style={{ marginBottom: 16 }} onSubmit={save}>
          <h2>{editing === null ? 'New employee' : 'Edit employee'}</h2>
          <div className="grid cols-2">
            <label><span>Employee code</span>
              <input value={form.emp_code} autoFocus
                     onChange={e => setForm({ ...form, emp_code: e.target.value })} /></label>
            <label><span>Name</span>
              <input value={form.name} onChange={e => setForm({ ...form, name: e.target.value })} /></label>
            <label><span>Department</span>
              <input value={form.department} onChange={e => setForm({ ...form, department: e.target.value })} /></label>
            <label><span>Designation</span>
              <input value={form.designation} onChange={e => setForm({ ...form, designation: e.target.value })} /></label>
            <label><span>Email</span>
              <input value={form.email} onChange={e => setForm({ ...form, email: e.target.value })} /></label>
            <label><span>Phone</span>
              <input value={form.phone} onChange={e => setForm({ ...form, phone: e.target.value })} /></label>
          </div>
          <label className="row" style={{ marginTop: 4, alignItems: 'flex-start' }}>
            <input type="checkbox" style={{ width: 'auto', marginTop: 3 }} checked={form.active}
                   onChange={e => setForm({ ...form, active: e.target.checked })} />
            <span style={{ margin: 0 }}>
              Active
              <small className="muted" style={{ fontWeight: 400 }}>
                Off stops this person being recognised on any camera and leaves them out of
                the attendance roster, while keeping everything already recorded.
              </small>
            </span>
          </label>
          <div className="row" style={{ marginTop: 8 }}>
            <button>{editing === null ? 'Add employee' : 'Save changes'}</button>
            <button type="button" className="ghost" onClick={close}>Cancel</button>
          </div>
        </form>
      )}

      {results && (
        <div className="card" style={{ marginBottom: 16 }}>
          <h2>Enrolment result</h2>
          <table><thead><tr><th>Photo</th><th>Result</th><th>Face size</th><th>Head turn</th></tr></thead>
            <tbody>{results.map((p, i) => (
              <tr key={i}>
                <td>{p.filename}</td>
                <td>{p.accepted
                  ? <span className="pill present">accepted</span>
                  : <><span className="pill absent">rejected</span> <span className="muted">{p.reason}</span></>}</td>
                <td className="muted">{Math.round(p.face_px)} px</td>
                <td className="muted">{p.yaw.toFixed(2)}</td>
              </tr>))}</tbody></table>
          <button className="ghost" style={{ marginTop: 10 }} onClick={() => setResults(null)}>Close</button>
        </div>
      )}

      <div className="card flush">
        <table>
          <thead><tr>
            <th>Code</th><th>Name</th><th>Department</th><th>Face profiles</th>
            <th style={{ textAlign: 'right' }}>Actions</th>
          </tr></thead>
          <tbody>
            {(rows ?? []).map(e => (
              <tr key={e.id} style={e.active ? undefined : { opacity: 0.55 }}>
                <td className="muted mono">{e.emp_code}</td>
                <td>{e.name}{!e.active && <> <Chip kind="idle">inactive</Chip></>}</td>
                <td className="muted">{e.department}</td>
                <td>{e.face_profiles > 0
                  ? <span className="pill present">{e.face_profiles} enrolled</span>
                  : <span className="pill absent">not enrolled</span>}</td>
                <td>
                  <div className="row" style={{ justifyContent: 'flex-end', gap: 6 }}>
                    <button className="ghost" disabled={enrolling === e.id}
                            onClick={() => { fileRef.current!.onchange = (ev) => {
                              const f = (ev.target as HTMLInputElement).files
                              if (f && f.length) enrol(e.id, f)
                            }; fileRef.current!.click() }}>
                      {enrolling === e.id ? 'Enrolling…' : 'Enrol photos'}
                    </button>
                    {e.face_profiles > 0 && (
                      <button className="ghost" onClick={async () => {
                        if (!confirm(`Remove every enrolled face for ${e.name}?`)) return
                        await api.del(`/api/employees/${e.id}/faces`); load()
                      }}>Clear faces</button>
                    )}
                    <button className="ghost" onClick={() => startEdit(e)}>Edit</button>
                    <button className="ghost" onClick={() => setActive(e, !e.active)}>
                      {e.active ? 'Deactivate' : 'Activate'}
                    </button>
                    <button className="ghost danger" onClick={() => remove(e)}>
                      <span style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
                        <Ic k="trash" s={14} /> Delete
                      </span>
                    </button>
                  </div>
                </td>
              </tr>))}
            {rows !== null && rows.length === 0 &&
              <tr><td colSpan={5} className="muted">No employees yet.</td></tr>}
            {rows === null && <tr><td colSpan={5} className="muted">Loading…</td></tr>}
          </tbody>
        </table>
      </div>
      <input ref={fileRef} type="file" accept="image/*" multiple hidden />
    </>
  )
}
