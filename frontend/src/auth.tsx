import { createContext, useContext, useEffect, useState, type ReactNode } from 'react'
import { api, getToken, setToken } from './api'

export type Role = 'superadmin' | 'admin' | 'user'

export interface User {
  id: number
  email: string
  name: string
  role: Role
  tenant_id: number
  employee_id?: number
}

interface AuthState {
  user: User | null
  loading: boolean
  login(email: string, password: string): Promise<void>
  logout(): void
}

const Ctx = createContext<AuthState>(null as unknown as AuthState)

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser] = useState<User | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    if (!getToken()) { setLoading(false); return }
    // The token is checked against the server rather than trusted: deleting a
    // user must take effect now, not when their token happens to expire.
    api.get<User>('/api/auth/me')
      .then(setUser)
      .catch(() => setToken(null))
      .finally(() => setLoading(false))
  }, [])

  const value: AuthState = {
    user,
    loading,
    async login(email, password) {
      const r = await api.post<{ token: string; user: User }>('/api/auth/login', { email, password })
      setToken(r.token)
      setUser(r.user)
    },
    logout() {
      setToken(null)
      setUser(null)
      location.href = '/login'
    },
  }
  return <Ctx.Provider value={value}>{children}</Ctx.Provider>
}

export const useAuth = () => useContext(Ctx)
