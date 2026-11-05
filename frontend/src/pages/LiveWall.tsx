import { useEffect, useState } from 'react'
import { Link, useNavigate } from 'react-router-dom'
import { api } from '../api'
import { Card, Chip, Empty, FilterBtn, Ic } from '../components/ui'
import LiveTile from '../components/LiveTile'

interface Camera { id: number; name: string; role: string; enabled: boolean
                   face_enabled: boolean; site: string; location: string }

/// What each camera is actually doing, polled from the server.
///
/// NOT measured in the browser: the tile asks for a picture at whatever rate the
/// Rate buttons say, and gets one every time whether or not the camera produced
/// a new frame. Counting those would report the button, not the camera.
interface Stat { id: number; online: boolean; decode_fps: number; stream_fps: number
                 width: number; height: number; behind: boolean }

/// How often each tile asks for a new picture.
///
/// This used to be three buttons — 4, 8, 15 fps — and it asked the wrong person
/// the wrong question. Nobody watching a wall knows what this board can encode;
/// they know whether the picture looks live. And the honest answer does not
/// depend on taste at all, it depends on arithmetic the page can do itself.
///
/// Every request makes the worker JPEG-encode one annotated frame, so the cost
/// is PIXELS PER SECOND: tiles on screen x their size x this rate. A single
/// 640x480 tile at 15 fps is 4.6 MP/s and nothing; sixteen 1880x1056 tiles at
/// the same rate is 476 MP/s, which would take the board away from the cameras
/// it is supposed to be watching.
///
/// So a budget is shared out instead. 24 MP/s is what this appliance gives the
/// wall while still decoding five cameras and running three models — measured
/// here, not guessed. Clamped to 2..15: below 2 it reads as a slideshow, and
/// above 15 nobody can see the difference.
const WALL_BUDGET_MP_PER_SEC = 24

function autoFps(sizes: { width: number; height: number }[]): number {
  const mp = sizes.reduce((a, s) => a + (s.width * s.height) / 1e6, 0)
  if (mp <= 0) return 8
  return Math.max(2, Math.min(15, Math.round(WALL_BUDGET_MP_PER_SEC / mp)))
}

/**
 * The wall.
 *
 * A tile only requests its stream while it is MOUNTED, and the server only
 * draws overlays and encodes JPEG while somebody is attached — so a wall left
 * on one camera costs one camera's worth of work, not eight. That is why the
 * grid unmounts tiles rather than hiding them.
 */
export default function LiveWall() {
  const [cams, setCams] = useState<Camera[] | null>(null)
  const [cols, setCols] = useState(2)
  const [site, setSite] = useState('')
  const [stats, setStats] = useState<Record<number, Stat>>({})
  const navigate = useNavigate()
  const [err, setErr] = useState('')

  useEffect(() => {
    api.get<Camera[]>('/api/cameras')
      .then(c => setCams(c.filter(x => x.enabled)))
      .catch(e => setErr(String(e.message)))
  }, [])

  useEffect(() => {
    const pull = () => api.get<Stat[]>('/api/camera-stats')
      .then(rows => setStats(Object.fromEntries(rows.map(r => [r.id, r]))))
      .catch(() => {})
    pull()
    // Every three seconds: this is a steady average, not something that twitches.
    const t = window.setInterval(pull, 3000)
    return () => window.clearInterval(t)
  }, [])

  // What is actually on screen decides the rate: the tiles this filter leaves
  // visible, at whatever size each camera turns out to be.
  const visible = (cams ?? []).filter(c => !site || c.site === site)
  const fps = autoFps(visible.map(c => stats[c.id]).filter(Boolean)
                             .map(st => ({ width: st.width, height: st.height })))

  return (
    <>
      <div className="head">
        <div>
          <h1>Live wall</h1>
          <div className="muted">
            Overlays are drawn and encoded only while a tile is open, so a closed tab costs
            nothing. <b>Double-click a camera</b> to set up its zones and gate line.
          </div>
        </div>
        <div className="row">
          {[...new Set((cams ?? []).map(c => c.site).filter(Boolean))].length > 0 && (
            <select value={site} onChange={e => setSite(e.target.value)}
                    style={{ width: 170, marginRight: 6 }}>
              <option value="">All sites</option>
              {[...new Set((cams ?? []).map(c => c.site).filter(Boolean))].sort()
                .map(x => <option key={x} value={x}>{x}</option>)}
            </select>
          )}
          <span className="stat-label" style={{ marginRight: 8 }}
                title={`${visible.length} tile(s) on screen, so each is refreshed `
                       + `${fps} times a second — shared out of what this board can `
                       + `encode without taking time from the cameras`}>
            {fps} fps each
          </span>
          <span className="stat-label" style={{ marginRight: 2 }}>Grid</span>
          {[1, 2, 3, 4].map(n => (
            <FilterBtn key={n} active={cols === n} onClick={() => setCols(n)}>{n}×{n}</FilterBtn>
          ))}
        </div>
      </div>

      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      {cams === null ? <div className="muted">Loading…</div>
       : cams.length === 0 ? (
        <Empty icon="camera" title="No cameras are enabled"
               text="Add a camera and it will appear here."
               action={<Link className="btn" to="/cameras">Go to cameras</Link>} />
       ) : (
        <div className="grid" style={{ gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))` }}>
          {visible.map(c => {
            const st = stats[c.id]
            return (
            <Card key={c.id} style={{ padding: 0, overflow: 'hidden', cursor: 'pointer' }}
                  title="Double-click to set up this camera's zones and gate line"
                  // Double-click, not single: a single click on a wall of tiles
                  // is how somebody selects or just rests the pointer, and
                  // navigating away from a wall they are watching would be a
                  // surprise every time.
                  onDoubleClick={() => navigate(`/cameras/${c.id}`)}>
              <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between',
                            gap: 8, padding: '9px 12px', borderBottom: '1px solid var(--line)' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 7, minWidth: 0 }}>
                  <span style={{ color: 'var(--accent)' }}><Ic k="camera" s={15} /></span>
                  <span style={{ fontWeight: 600, fontSize: 13, whiteSpace: 'nowrap',
                                 overflow: 'hidden', textOverflow: 'ellipsis' }}>{c.name}</span>
                </div>
                <div className="row" style={{ gap: 6 }}>
                  {c.role === 'gate' && <Chip kind="info">gate</Chip>}
                  {c.face_enabled && <Chip kind="ok">faces</Chip>}
                  {st && (st.behind
                    ? <Chip kind="warn" title={`the stream sends ${Math.round(st.stream_fps)} fps, `
                        + `this board decodes ${st.decode_fps.toFixed(1)} — the picture runs behind`}>
                        {st.decode_fps.toFixed(1)} fps · behind
                      </Chip>
                    : <Chip kind={st.online ? 'ok' : 'bad'}
                            title={st.stream_fps > 0
                              ? `the stream sends ${Math.round(st.stream_fps)} fps`
                              : 'frames decoded per second'}>
                        {st.decode_fps.toFixed(1)} fps
                      </Chip>)}
                </div>
              </div>
              <LiveTile cameraId={c.id} fps={fps}
                        style={{ width: '100%', aspectRatio: '16 / 9' }} />
              <div style={{ padding: '8px 12px', display: 'flex', justifyContent: 'space-between',
                            alignItems: 'center' }}>
                <span className="muted" style={{ fontSize: 11.5 }}>
                  {c.location || `camera #${c.id}`}
                  {st && st.width > 0 &&
                    <span style={{ color: 'var(--text-3)' }}> · {st.width}×{st.height}</span>}
                </span>
                <Link className="btn ghost" to={`/cameras/${c.id}`}
                      onClick={e => e.stopPropagation()}
                      style={{ padding: '4px 9px', fontSize: 12 }}>Zones</Link>
              </div>
            </Card>
            )
          })}
        </div>
      )}
    </>
  )
}
