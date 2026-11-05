import { useState } from 'react'
import { useAuth } from '../auth'

export default function Login() {
  const { login } = useAuth()
  const [email, setEmail] = useState('')
  const [password, setPassword] = useState('')
  const [err, setErr] = useState('')
  const [busy, setBusy] = useState(false)

  const submit = async (e: React.FormEvent) => {
    e.preventDefault()
    setErr('')
    setBusy(true)
    try { await login(email, password) }
    catch (x) { setErr(x instanceof Error ? x.message : 'sign-in failed') }
    finally { setBusy(false) }
  }

  return (
    <div className="login">
      <form className="card" onSubmit={submit}>
        <div className="brand" style={{ padding: '0 0 14px' }}>Smart<span>Office</span></div>
        {err && <div className="err" style={{ marginBottom: 12 }}>{err}</div>}
        <label><span>Email</span>
          <input value={email} onChange={e => setEmail(e.target.value)} autoFocus autoComplete="username" />
        </label>
        <label><span>Password</span>
          <input type="password" value={password} onChange={e => setPassword(e.target.value)} autoComplete="current-password" />
        </label>
        <button style={{ width: '100%', marginTop: 6 }} disabled={busy || !email || !password}>
          {busy ? 'Signing in…' : 'Sign in'}
        </button>
      </form>
    </div>
  )
}
