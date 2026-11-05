import { useEffect, useState } from 'react'
import { api } from '../api'

interface S {
  timezone: string; office_start: string; office_end: string
  grace_minutes: number; early_leave_grace_minutes: number; min_break_seconds: number
  telegram_chat_id: string; telegram_configured: boolean
  smtp_host: string; smtp_port: number; smtp_user: string; smtp_from: string
  smtp_from_name: string; email_to: string; smtp_password_set: boolean
}

export default function Settings() {
  const [s, setS] = useState<S | null>(null)
  const [token, setToken] = useState('')
  const [password, setPassword] = useState('')
  const [msg, setMsg] = useState('')
  const [err, setErr] = useState('')

  useEffect(() => { api.get<S>('/api/settings').then(setS).catch(e => setErr(String(e.message))) }, [])
  if (!s) return <div className="muted">Loading…</div>

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    setErr(''); setMsg('')
    try {
      // Blank secret fields mean "leave alone" — the server never sends them
      // back, so posting an empty value would otherwise wipe them.
      await api.put('/api/settings', {
        ...s,
        telegram_bot_token: token || undefined,
        smtp_password: password || undefined,
      })
      setToken(''); setPassword(''); setMsg('Saved.')
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not save') }
  }

  return (
    <>
      <div className="head"><div><h1>Settings</h1>
        <div className="muted">Office hours drive who is late; the channels carry the alerts.</div></div></div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}
      {msg && <div className="note" style={{ marginBottom: 14 }}>{msg}</div>}

      <form onSubmit={save} className="grid cols-2">
        <div className="card">
          <h2>Office hours</h2>
          <div className="grid cols-2">
            <label><span>Start</span>
              <input type="time" value={s.office_start} onChange={e => setS({ ...s, office_start: e.target.value })} /></label>
            <label><span>End</span>
              <input type="time" value={s.office_end} onChange={e => setS({ ...s, office_end: e.target.value })} /></label>
            <label><span>Arrival grace (minutes)</span>
              <input type="number" value={s.grace_minutes}
                     onChange={e => setS({ ...s, grace_minutes: Number(e.target.value) })} /></label>
            <label><span>Early-leave grace (minutes)</span>
              <input type="number" value={s.early_leave_grace_minutes}
                     onChange={e => setS({ ...s, early_leave_grace_minutes: Number(e.target.value) })} /></label>

            <label><span>Shortest break</span>
              <div className="row">
                <input type="number" min={0} max={3600} step={15} value={s.min_break_seconds}
                       onChange={e => setS({ ...s, min_break_seconds: Number(e.target.value) })} />
                <span className="muted" style={{ whiteSpace: 'nowrap' }}>seconds, 0 = count every gap</span>
              </div>
              <small className="muted">
                An absence shorter than this is treated as a tracking gap rather than a
                break, and the time goes back into working hours. It exists because an
                exit zone has to be drawn somewhere: where its edge falls near a desk,
                somebody standing there can stop being tracked and look exactly like
                somebody walking out. A false exit of that kind is always followed by the
                same person being seen again moments later, usually on another camera.
                Raise it if short absences are being recorded as breaks; lower it if real
                short breaks are being swallowed.
              </small></label>
          </div>
          <label><span>Time zone</span>
            <input value={s.timezone} onChange={e => setS({ ...s, timezone: e.target.value })} /></label>
        </div>

        <div className="card">
          <h2>Telegram</h2>
          <label><span>Bot token {s.telegram_configured && <em>— already set; leave blank to keep it</em>}</span>
            <input value={token} onChange={e => setToken(e.target.value)} placeholder={s.telegram_configured ? '••••••••' : ''} /></label>
          <label><span>Chat ID</span>
            <input value={s.telegram_chat_id} onChange={e => setS({ ...s, telegram_chat_id: e.target.value })} /></label>

          <h2 style={{ marginTop: 18 }}>Email</h2>
          <div className="grid cols-2">
            <label><span>SMTP host</span>
              <input value={s.smtp_host} onChange={e => setS({ ...s, smtp_host: e.target.value })} /></label>
            <label><span>Port</span>
              <input type="number" value={s.smtp_port} onChange={e => setS({ ...s, smtp_port: Number(e.target.value) })} /></label>
            <label><span>Username</span>
              <input value={s.smtp_user} onChange={e => setS({ ...s, smtp_user: e.target.value })} /></label>
            <label><span>Password {s.smtp_password_set && <em>— set</em>}</span>
              <input type="password" value={password} onChange={e => setPassword(e.target.value)} /></label>
            <label><span>From address</span>
              <input value={s.smtp_from} onChange={e => setS({ ...s, smtp_from: e.target.value })} /></label>
            <label><span>Send alerts to</span>
              <input value={s.email_to} onChange={e => setS({ ...s, email_to: e.target.value })} placeholder="a@x.com, b@y.com" /></label>
          </div>
        </div>
        <div><button>Save settings</button></div>
      </form>
    </>
  )
}
