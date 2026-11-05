// One place that talks to the backend.
//
// The token lives in memory and in sessionStorage, not localStorage: a token in
// localStorage survives the tab closing, which for a building-access dashboard
// on a shared machine is a longer life than it should have.

const TOKEN_KEY = 'so.token'

export function getToken(): string | null {
  return sessionStorage.getItem(TOKEN_KEY)
}
export function setToken(t: string | null) {
  if (t) sessionStorage.setItem(TOKEN_KEY, t)
  else sessionStorage.removeItem(TOKEN_KEY)
}

export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const headers = new Headers(init.headers)
  const token = getToken()
  if (token) headers.set('Authorization', `Bearer ${token}`)
  if (init.body && !(init.body instanceof FormData))
    headers.set('Content-Type', 'application/json')

  const res = await fetch(path, { ...init, headers })
  if (res.status === 401) {
    // The session is gone. Clearing it here means every screen recovers the
    // same way instead of each one inventing its own.
    setToken(null)
    if (!location.pathname.startsWith('/login')) location.href = '/login'
    throw new ApiError(401, 'session expired')
  }
  const text = await res.text()
  const data = text ? (() => { try { return JSON.parse(text) } catch { return text } })() : null
  if (!res.ok) throw new ApiError(res.status, (data && data.error) || res.statusText)
  return data as T
}

export const api = {
  get: <T,>(p: string) => request<T>(p),
  post: <T,>(p: string, body?: unknown) =>
    request<T>(p, { method: 'POST', body: body ? JSON.stringify(body) : undefined }),
  patch: <T,>(p: string, body: unknown) =>
    request<T>(p, { method: 'PATCH', body: JSON.stringify(body) }),
  put: <T,>(p: string, body: unknown) =>
    request<T>(p, { method: 'PUT', body: JSON.stringify(body) }),
  del: <T,>(p: string) => request<T>(p, { method: 'DELETE' }),
  upload: <T,>(p: string, form: FormData) => request<T>(p, { method: 'POST', body: form }),
}

/// Image and stream URLs cannot carry a header, so the token goes in the query.
export function mediaUrl(path: string): string {
  const t = getToken() ?? ''
  const sep = path.includes('?') ? '&' : '?'
  return `${path}${sep}token=${encodeURIComponent(t)}`
}

export function hms(seconds: number): string {
  if (!seconds || seconds <= 0) return '—'
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  return `${h}h ${String(m).padStart(2, '0')}m`
}

export function today(): string {
  const d = new Date()
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
}
