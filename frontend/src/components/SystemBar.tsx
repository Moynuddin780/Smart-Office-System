import { useEffect, useState } from 'react'

interface Health {
  ok: boolean
  database: boolean
  npu: boolean
  engines: number
  cameras_running: number
  cameras_online: number
  npu_core_utilisation: number[]
  gallery: number
  cpu?: {
    board: number; little: number; big: number; hottest: number
    cores: { core: number; busy: number; mhz: number; big: boolean }[]
  }
}

/**
 * The control-panel strip.
 *
 * Reads /healthz, which is public and cheap. It is here rather than buried on a
 * diagnostics page because the two questions an operator of this kind of system
 * asks constantly are "is the NPU alive" and "are my cameras up", and an answer
 * that needs navigating to is an answer nobody checks.
 */
export default function SystemBar() {
  const [h, setH] = useState<Health | null>(null)

  useEffect(() => {
    const poll = () => fetch('/healthz').then(r => r.json()).then(setH).catch(() => setH(null))
    poll()
    const t = setInterval(poll, 5000)
    return () => clearInterval(t)
  }, [])

  const cores = h?.npu_core_utilisation ?? []
  const camsOk = (h?.cameras_online ?? 0) === (h?.cameras_running ?? 0) && (h?.cameras_running ?? 0) > 0

  return (
    <div className="sysbar">
      <div className="sysbar-title">System</div>

      <div className="sysrow">
        <span className={`led ${h?.npu ? 'on' : 'off'}`} />
        <span className="k">NPU</span>
        <span className="v">{h ? `${h.engines} eng` : '—'}</span>
      </div>

      {/* Labelled NPU, not "core N". Three bars under a heading that said only
          "core 0..2" read as the processor, and the board has eight of those —
          so the panel appeared to show a machine running on three cores. */}
      {cores.map((u, i) => (
        <div className="sysrow" key={`npu${i}`}>
          <span className="k" style={{ width: 46 }}>NPU {i}</span>
          <span className="meter"><i style={{ width: `${Math.min(100, Math.max(2, u))}%` }} /></span>
          <span className="v" style={{ width: 34, textAlign: 'right' }}>{u.toFixed(0)}%</span>
        </div>
      ))}

      {/* THE CPU, which is what actually runs out first.
          The NPU reads around a fifth while cameras fall behind, and that looks
          like headroom until you can see the processor beside it. The hottest
          core is called out because the average hides the case that matters:
          one thread pinned at 100% stalls a camera while the board looks calm. */}
      {h?.cpu && (
        <>
          <div className="sysrow" style={{ marginTop: 6 }}>
            <span className={`led ${h.cpu.hottest > 95 ? 'off' : 'on'}`} />
            <span className="k">CPU</span>
            <span className="v" title={`busiest core ${h.cpu.hottest.toFixed(0)}%`}>
              {h.cpu.board.toFixed(0)}% avg
            </span>
          </div>
          {h.cpu.cores.map(c => (
            <div className="sysrow" key={`cpu${c.core}`}>
              <span className="k" style={{ width: 46 }}
                    title={`${c.mhz} MHz ${c.big ? 'performance' : 'efficiency'} core`}>
                cpu{c.core}{c.big ? '\u2191' : ''}
              </span>
              <span className="meter">
                <i style={{ width: `${Math.min(100, Math.max(2, c.busy))}%`,
                            background: c.busy > 95 ? 'var(--crit)' : undefined }} />
              </span>
              <span className="v" style={{ width: 34, textAlign: 'right' }}>{c.busy.toFixed(0)}%</span>
            </div>
          ))}
        </>
      )}

      <div className="sysrow">
        <span className={`led ${camsOk ? 'on' : (h?.cameras_running ? 'off' : '')}`} />
        <span className="k">Cameras</span>
        <span className="v">{h ? `${h.cameras_online}/${h.cameras_running}` : '—'}</span>
      </div>

      <div className="sysrow">
        <span className={`led ${h?.database ? 'on' : 'off'}`} />
        <span className="k">Database</span>
        <span className="v">{h?.database ? 'up' : 'down'}</span>
      </div>
    </div>
  )
}
