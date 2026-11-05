import { useEffect, useState } from 'react'
import { api } from '../api'
import { useAuth } from '../auth'

interface Tenant {
  id: number; name: string; slug: string; status: string
  max_cameras: number; max_employees: number; created_at: string
  admin_email: string; cameras: number; employees: number
}

export default function Tenants() {
  const { logout } = useAuth()
  const [rows, setRows] = useState<Tenant[] | null>(null)
  const [open, setOpen] = useState(false)
  const [form, setForm] = useState({ name: '', admin_email: '', admin_name: '', admin_password: '' })
  const [err, setErr] = useState('')
  const [created, setCreated] = useState<{ email: string; password: string } | null>(null)

  const load = () => api.get<Tenant[]>('/api/admin/tenants').then(setRows).catch(e => setErr(String(e.message)))
  useEffect(() => { load() }, [])

  const create = async (e: React.FormEvent) => {
    e.preventDefault()
    setErr(''); setCreated(null)
    try {
      const r = await api.post<{ admin_created?: string | false; admin_error?: string }>(
        '/api/admin/tenants', form)
      if (r.admin_created) setCreated({ email: form.admin_email, password: form.admin_password })
      else setErr(r.admin_error ?? 'The company was created but no administrator account was made.')
      setForm({ name: '', admin_email: '', admin_name: '', admin_password: '' })
      setOpen(false); load()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not create the company') }
  }

  const setStatus = async (t: Tenant, status: string) => {
    await api.patch(`/api/admin/tenants/${t.id}`, { status }); load()
  }

  const empty = rows !== null && rows.length === 0

  return (
    <>
      <div className="head">
        <div>
          <h1>Vendor console</h1>
          <div className="muted">You are signed in as the vendor. You provision client
            companies here — you do not see inside them.</div>
        </div>
        <button onClick={() => setOpen(o => !o)}>{open ? 'Cancel' : 'New company'}</button>
      </div>

      {/* The thing this screen previously did not say, and should have. */}
      <div className="note" style={{ marginBottom: 16 }}>
        <b>Smart Office itself — attendance, cameras, zones, fall detection, desk
        performance — lives inside a company account, not here.</b> A superadmin
        deliberately cannot read a client's cameras or faces, so this console shows
        companies and nothing else. To use or demonstrate the product, sign out and
        sign in with a company's administrator email below.
      </div>

      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      {created && (
        <div className="card" style={{ marginBottom: 16, borderColor: 'var(--ok)' }}>
          <h2>Company created</h2>
          <p className="muted" style={{ marginTop: 0 }}>
            These credentials are shown once. Give them to the client, or use them
            yourself to set the company up.
          </p>
          <table style={{ maxWidth: 460 }}><tbody>
            <tr><th style={{ width: 120 }}>Email</th><td><code>{created.email}</code></td></tr>
            <tr><th>Password</th><td><code>{created.password}</code></td></tr>
          </tbody></table>
          <div className="row" style={{ marginTop: 12 }}>
            <button onClick={logout}>Sign out and sign in as them</button>
            <button className="ghost" onClick={() => setCreated(null)}>Dismiss</button>
          </div>
        </div>
      )}

      {open && (
        <form className="card" style={{ marginBottom: 16 }} onSubmit={create}>
          <h2>Provision a company</h2>
          <p className="muted" style={{ marginTop: 0 }}>
            The administrator account is created with the company — a company with no
            way in is not provisioned.
          </p>
          <div className="grid cols-2">
            <label><span>Company name</span>
              <input value={form.name} autoFocus
                     onChange={e => setForm({ ...form, name: e.target.value })} /></label>
            <label><span>Administrator name</span>
              <input value={form.admin_name}
                     onChange={e => setForm({ ...form, admin_name: e.target.value })} /></label>
            <label><span>Administrator email</span>
              <input value={form.admin_email} placeholder="admin@theircompany.com"
                     onChange={e => setForm({ ...form, admin_email: e.target.value })} /></label>
            <label><span>Administrator password (at least 8 characters)</span>
              <input type="password" value={form.admin_password}
                     onChange={e => setForm({ ...form, admin_password: e.target.value })} /></label>
          </div>
          <button disabled={!form.name || !form.admin_email || form.admin_password.length < 8}>
            Create company
          </button>
        </form>
      )}

      {empty ? (
        <div className="card" style={{ textAlign: 'center', padding: '40px 20px' }}>
          <h2 style={{ marginBottom: 6 }}>No client companies yet</h2>
          <p className="muted" style={{ maxWidth: 520, margin: '0 auto 18px' }}>
            Create one to get started. You will be given an administrator email and
            password for it — sign in with those to add cameras, enrol employees, draw
            zones and see attendance.
          </p>
          <button onClick={() => setOpen(true)}>Create the first company</button>
        </div>
      ) : (
        <div className="card flush">
          <table>
            <thead><tr>
              <th>Company</th><th>Sign in as</th><th>Set up</th><th>Status</th><th>Limits</th><th></th>
            </tr></thead>
            <tbody>
              {(rows ?? []).map(t => (
                <tr key={t.id}>
                  <td>{t.name}<div className="muted mono" style={{ fontSize: 12 }}>{t.slug}</div></td>
                  <td>{t.admin_email
                    ? <code style={{ fontSize: 12 }}>{t.admin_email}</code>
                    : <span className="muted">no administrator — nobody can sign in</span>}</td>
                  <td className="muted">
                    {t.cameras === 0 && t.employees === 0
                      ? 'nothing configured yet'
                      : `${t.cameras} camera${t.cameras === 1 ? '' : 's'} · ${t.employees} employee${t.employees === 1 ? '' : 's'}`}
                  </td>
                  <td><span className={`pill ${t.status === 'active' ? 'present' : 'absent'}`}>{t.status}</span></td>
                  <td className="muted">{t.max_cameras} / {t.max_employees}</td>
                  <td>
                    {t.status === 'active'
                      ? <button className="ghost" onClick={() => setStatus(t, 'suspended')}>Suspend</button>
                      : <button className="ghost" onClick={() => setStatus(t, 'active')}>Reactivate</button>}
                  </td>
                </tr>))}
              {rows === null && <tr><td colSpan={6} className="muted">Loading…</td></tr>}
            </tbody>
          </table>
          <p className="muted" style={{ fontSize: 12, margin: 0, padding: '11px 14px',
                                        borderTop: '1px solid var(--line-soft)' }}>
            Suspending a company stops its cameras immediately, not at the next restart.
          </p>
        </div>
      )}
    </>
  )
}
