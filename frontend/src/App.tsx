import { NavLink, Navigate, Route, Routes } from 'react-router-dom'
import { useAuth } from './auth'
import SystemBar from './components/SystemBar'
import { Ic } from './components/ui'
import Login from './pages/Login'
import Dashboard from './pages/Dashboard'
import LiveWall from './pages/LiveWall'
import Modules from './pages/Modules'
import FaceRegistry from './pages/FaceRegistry'
import Cameras from './pages/Cameras'
import CameraDetail from './pages/CameraDetail'
import Employees from './pages/Employees'
import Attendance from './pages/Attendance'
import Desk from './pages/Desk'
import ExitReviews from './pages/ExitReviews'
import Events from './pages/Events'
import Settings from './pages/Settings'
import Tenants from './pages/Tenants'
import MyAttendance from './pages/MyAttendance'

/// A nav row with its icon. Kept here rather than in the UI kit because the
/// active state comes from the router, and NavLink already owns that.
function Item({ to, icon, children }:
  { to: string; icon: string; children: React.ReactNode }) {
  return (
    <NavLink to={to} end={to === '/'}>
      <span style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
        <Ic k={icon} s={16} />
        <span>{children}</span>
      </span>
    </NavLink>
  )
}

function Shell({ children }: { children: React.ReactNode }) {
  const { user, logout } = useAuth()
  const admin = user?.role === 'admin'
  const superadmin = user?.role === 'superadmin'
  return (
    <div className="shell">
      <aside className="side">
        <div className="brand">Smart<span>Office</span></div>

        <nav className="nav">
          {superadmin && <>
            <div className="nav-group">Vendor</div>
            <Item to="/tenants" icon="building">Client companies</Item>
          </>}

          {admin && <>
            <div className="nav-group">Monitor</div>
            <Item to="/" icon="dashboard">Dashboard</Item>
            <Item to="/live" icon="wall">Live wall</Item>

            <div className="nav-group">Reports</div>
            <Item to="/attendance" icon="clock">Attendance</Item>
            <Item to="/desk" icon="desk">Desk performance</Item>
            <Item to="/exit-reviews" icon="clock">Unnamed exits</Item>

            <div className="nav-group">Configure</div>
            <Item to="/cameras" icon="camera">Cameras &amp; zones</Item>
            <Item to="/modules" icon="modules">AI modules</Item>
            <Item to="/faces" icon="face">Face registry</Item>
            <Item to="/employees" icon="users">Employees</Item>
            <Item to="/events" icon="alert">Alerts</Item>
            <Item to="/settings" icon="settings">Settings</Item>
          </>}

          {user?.role === 'user' && <>
            <div className="nav-group">Me</div>
            <Item to="/me" icon="clock">My attendance</Item>
          </>}
        </nav>

        {/* Only where it means something: the vendor console has no cameras. */}
        {!superadmin && <SystemBar />}

        <div className="side-foot">
          <div className="who">{user?.name || user?.email}</div>
          <div className="who-role">{user?.role}</div>
          <button className="ghost" style={{ width: '100%' }} onClick={logout}>Sign out</button>
        </div>
      </aside>
      <main className="main">{children}</main>
    </div>
  )
}

export default function App() {
  const { user, loading } = useAuth()
  if (loading) return <div className="login"><div className="muted">Loading…</div></div>
  if (!user) return (
    <Routes>
      <Route path="/login" element={<Login />} />
      <Route path="*" element={<Navigate to="/login" replace />} />
    </Routes>
  )

  const home = user.role === 'superadmin' ? '/tenants' : user.role === 'user' ? '/me' : '/'
  return (
    <Shell>
      <Routes>
        <Route path="/login" element={<Navigate to={home} replace />} />
        <Route path="/tenants" element={<Tenants />} />
        <Route path="/" element={user.role === 'admin' ? <Dashboard /> : <Navigate to={home} replace />} />
        <Route path="/attendance" element={<Attendance />} />
        <Route path="/desk" element={<Desk />} />
        <Route path="/exit-reviews" element={<ExitReviews />} />
        <Route path="/events" element={<Events />} />
        <Route path="/live" element={<LiveWall />} />
        <Route path="/modules" element={<Modules />} />
        <Route path="/faces" element={<FaceRegistry />} />
        <Route path="/cameras" element={<Cameras />} />
        <Route path="/cameras/:id" element={<CameraDetail />} />
        <Route path="/employees" element={<Employees />} />
        <Route path="/settings" element={<Settings />} />
        <Route path="/me" element={<MyAttendance />} />
        <Route path="*" element={<Navigate to={home} replace />} />
      </Routes>
    </Shell>
  )
}
