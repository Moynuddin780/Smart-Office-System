import type { CSSProperties, ReactNode } from 'react'
import { useEffect, useRef } from 'react'

/* ── icons ────────────────────────────────────────────────────────────────
   Inline strokes rather than an icon package: this is a dozen glyphs, and a
   dependency that ships a thousand to use twelve is a dependency to audit,
   update and explain. They inherit currentColor so a chip or a nav item
   colours its own icon without being told.                                  */

const PATHS: Record<string, ReactNode> = {
  dashboard: <><rect x="3" y="3" width="7" height="8" rx="1.5" /><rect x="14" y="3" width="7" height="5" rx="1.5" /><rect x="14" y="11" width="7" height="10" rx="1.5" /><rect x="3" y="14" width="7" height="7" rx="1.5" /></>,
  wall: <><rect x="3" y="4" width="8" height="7" rx="1.5" /><rect x="13" y="4" width="8" height="7" rx="1.5" /><rect x="3" y="13" width="8" height="7" rx="1.5" /><rect x="13" y="13" width="8" height="7" rx="1.5" /></>,
  camera: <><path d="M3 8a2 2 0 0 1 2-2h2.2a2 2 0 0 0 1.7-.9l.6-1a1 1 0 0 1 .8-.4h3.4a1 1 0 0 1 .8.4l.6 1a2 2 0 0 0 1.7.9H19a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z" /><circle cx="12" cy="12" r="3.4" /></>,
  face: <><circle cx="12" cy="8.5" r="3.6" /><path d="M4.5 20.5a7.5 7.5 0 0 1 15 0" /></>,
  modules: <><rect x="3" y="3" width="7.5" height="7.5" rx="1.5" /><rect x="13.5" y="3" width="7.5" height="7.5" rx="1.5" /><rect x="3" y="13.5" width="7.5" height="7.5" rx="1.5" /><path d="M17.25 13.5v7.5M13.5 17.25h7.5" /></>,
  gallery: <><rect x="3" y="4" width="18" height="14" rx="2" /><path d="m3 14 4.5-4 4 3.5L15 10l6 5" /><circle cx="8.5" cy="8" r="1.4" /></>,
  alert: <><path d="M12 3.5 2.8 19.5h18.4Z" /><path d="M12 10v4" /><circle cx="12" cy="17" r=".6" fill="currentColor" /></>,
  clock: <><circle cx="12" cy="12" r="8.5" /><path d="M12 7.5V12l3 2" /></>,
  desk: <><path d="M3 9h18M5 9v10M19 9v10M3 9l2-4h14l2 4" /><path d="M9 19h6" /></>,
  users: <><circle cx="9" cy="8" r="3.2" /><path d="M3 20a6 6 0 0 1 12 0" /><path d="M16 5.5a3.2 3.2 0 0 1 0 5M17.5 20a6 6 0 0 0-2-4.1" /></>,
  settings: <><circle cx="12" cy="12" r="3" /><path d="M12 2.5v3M12 18.5v3M21.5 12h-3M5.5 12h-3M18.7 5.3l-2.1 2.1M7.4 16.6l-2.1 2.1M18.7 18.7l-2.1-2.1M7.4 7.4 5.3 5.3" /></>,
  building: <><rect x="4" y="3" width="16" height="18" rx="1.5" /><path d="M9 7h2M13 7h2M9 11h2M13 11h2M9 15h2M13 15h2M10 21v-3h4v3" /></>,
  plus: <path d="M12 5v14M5 12h14" />,
  trash: <><path d="M4 7h16M10 7V5h4v2M6 7l1 13h10l1-13" /><path d="M10 11v6M14 11v6" /></>,
  upload: <><path d="M12 16V4M7.5 8.5 12 4l4.5 4.5" /><path d="M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3" /></>,
  play: <path d="M8 5.5 18 12 8 18.5Z" />,
  refresh: <><path d="M20 12a8 8 0 1 1-2.4-5.7" /><path d="M20 4v4h-4" /></>,
  check: <path d="m5 12.5 4.5 4.5L19 7" />,
  x: <path d="M6 6l12 12M18 6 6 18" />,
  warn: <><circle cx="12" cy="12" r="8.5" /><path d="M12 7.5v5" /><circle cx="12" cy="16" r=".6" fill="currentColor" /></>,
  chip: <><rect x="6" y="6" width="12" height="12" rx="2" /><path d="M10 2.5v3.5M14 2.5v3.5M10 18v3.5M14 18v3.5M2.5 10H6M2.5 14H6M18 10h3.5M18 14h3.5" /></>,
  download: <><path d="M12 4v12M7.5 11.5 12 16l4.5-4.5" /><path d="M4 18v2a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-2" /></>,
  edit: <><path d="M4 20h4L19 9a2.1 2.1 0 0 0-3-3L5 17Z" /><path d="M15 6l3 3" /></>,
  mic: <><rect x="9" y="3" width="6" height="11" rx="3" /><path d="M5.5 11.5a6.5 6.5 0 0 0 13 0M12 18v3" /></>,
}

export function Ic({ k, s = 18, sw = 1.7, style }: { k: string; s?: number; sw?: number; style?: CSSProperties }) {
  const d = PATHS[k]
  if (!d) return null
  return (
    <svg width={s} height={s} viewBox="0 0 24 24" fill="none" stroke="currentColor"
         strokeWidth={sw} strokeLinecap="round" strokeLinejoin="round"
         style={{ display: 'block', flex: 'none', ...style }} aria-hidden>
      {d}
    </svg>
  )
}

/* ── primitives ───────────────────────────────────────────────────────── */

type ChipKind = 'ok' | 'warn' | 'bad' | 'info' | 'idle'

export function Chip({ kind = 'ok', children, title }: { kind?: ChipKind; children: ReactNode; title?: string }) {
  const cls: Record<ChipKind, string> = {
    ok: 'present', warn: 'late', bad: 'critical', info: 'info', idle: 'absent',
  }
  return <span className={`pill ${cls[kind]}`} title={title}>{children}</span>
}

export function Card({ children, style, className = '', onClick, onDoubleClick, title }:
  { children: ReactNode; style?: CSSProperties; className?: string
    onClick?: () => void; onDoubleClick?: () => void; title?: string }) {
  return (
    <div className={`card ${className}`} onClick={onClick} onDoubleClick={onDoubleClick}
         title={title}
         style={{ cursor: onClick || onDoubleClick ? 'pointer' : undefined, ...style }}>
      {children}
    </div>
  )
}

export function Btn({ children, onClick, kind = 'ghost', icon, disabled, wide, type = 'button', title }:
  { children?: ReactNode; onClick?: () => void; kind?: 'primary' | 'ghost' | 'danger'
    icon?: string; disabled?: boolean; wide?: boolean; type?: 'button' | 'submit'; title?: string }) {
  const cls = kind === 'primary' ? '' : kind === 'danger' ? 'danger' : 'ghost'
  return (
    <button type={type} className={cls} onClick={onClick} disabled={disabled} title={title}
            style={{ width: wide ? '100%' : undefined, display: 'inline-flex',
                     alignItems: 'center', justifyContent: 'center', gap: 7 }}>
      {icon && <Ic k={icon} s={15} />}
      {children}
    </button>
  )
}

export function FilterBtn({ active, onClick, children }:
  { active: boolean; onClick: () => void; children: ReactNode }) {
  return (
    <button onClick={onClick} className={active ? '' : 'ghost'}
            style={{ padding: '5px 11px', fontSize: 12.5 }}>
      {children}
    </button>
  )
}

export function SectionTitle({ children, right }: { children: ReactNode; right?: ReactNode }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between',
                  marginBottom: 12, gap: 12 }}>
      <h2 style={{ margin: 0 }}>{children}</h2>
      {right}
    </div>
  )
}

export function KpiCard({ label, value, sub, icon, tone = 'accent', onClick }:
  { label: string; value: ReactNode; sub?: ReactNode; icon?: string
    tone?: 'accent' | 'ok' | 'warn' | 'crit' | 'muted'; onClick?: () => void }) {
  const colour = tone === 'ok' ? 'var(--ok)' : tone === 'warn' ? 'var(--warn)'
               : tone === 'crit' ? 'var(--crit)' : tone === 'muted' ? 'var(--text-3)'
               : 'var(--accent)'
  return (
    <Card onClick={onClick}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 7, color: colour, marginBottom: 9 }}>
        {icon && <Ic k={icon} s={15} />}
        <span className="stat-label" style={{ color: 'inherit' }}>{label}</span>
      </div>
      <div className="stat" style={{ color: tone === 'accent' ? 'var(--text)' : colour }}>{value}</div>
      {sub && <div className="muted" style={{ fontSize: 12, marginTop: 3 }}>{sub}</div>}
    </Card>
  )
}

export function MiniStat({ label, value, tone }: { label: string; value: ReactNode; tone?: string }) {
  return (
    <div style={{ background: 'var(--surface-2)', border: '1px solid var(--line)',
                  borderRadius: 'var(--r-sm)', padding: '8px 10px' }}>
      <div className="stat-label" style={{ fontSize: 9.5 }}>{label}</div>
      <div style={{ fontSize: 15, fontWeight: 650, marginTop: 2, color: tone || 'var(--text)',
                    fontVariantNumeric: 'tabular-nums' }}>{value}</div>
    </div>
  )
}

export function Modal({ title, sub, onClose, children, width = 620 }:
  { title: string; sub?: ReactNode; onClose: () => void; children: ReactNode; width?: number }) {
  const ref = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose() }
    document.addEventListener('keydown', esc)
    document.body.style.overflow = 'hidden'
    return () => { document.removeEventListener('keydown', esc); document.body.style.overflow = '' }
  }, [onClose])

  return (
    <div style={{ position: 'fixed', inset: 0, background: 'rgba(22,32,44,.42)', zIndex: 80,
                  display: 'grid', placeItems: 'center', padding: 20 }}
         onMouseDown={e => { if (e.target === e.currentTarget) onClose() }}>
      <div ref={ref} className="card" style={{ width, maxWidth: '100%', maxHeight: '90vh',
                                               overflowY: 'auto', padding: 0 }}>
        <div style={{ display: 'flex', alignItems: 'flex-start', justifyContent: 'space-between',
                      gap: 14, padding: '16px 18px', borderBottom: '1px solid var(--line)' }}>
          <div>
            <div style={{ fontWeight: 650, fontSize: 15 }}>{title}</div>
            {sub && <div className="muted" style={{ fontSize: 12.5, marginTop: 2 }}>{sub}</div>}
          </div>
          <button className="ghost" onClick={onClose} style={{ padding: '5px 8px' }} title="Close">
            <Ic k="x" s={15} />
          </button>
        </div>
        <div style={{ padding: 18 }}>{children}</div>
      </div>
    </div>
  )
}

export function Empty({ icon, title, text, action }:
  { icon?: string; title: string; text?: ReactNode; action?: ReactNode }) {
  return (
    <Card style={{ textAlign: 'center', padding: '36px 22px' }}>
      {icon && <div style={{ color: 'var(--text-3)', display: 'grid', placeItems: 'center',
                             marginBottom: 12 }}><Ic k={icon} s={30} sw={1.4} /></div>}
      <div style={{ fontWeight: 650, fontSize: 15, marginBottom: 6 }}>{title}</div>
      {text && <div className="muted" style={{ maxWidth: 520, margin: '0 auto 16px', fontSize: 13 }}>{text}</div>}
      {action}
    </Card>
  )
}
