import { useEffect, useRef, useState } from 'react'

export type Point = [number, number]
export type ZoneKind = 'perimeter' | 'restricted' | 'desk' | 'exit'

export interface DrawnZone {
  id?: number
  type: ZoneKind
  name: string
  polygon: Point[]
  employee_id?: number
  employee_name?: string
  enabled?: boolean
}

export interface DrawnLine {
  id?: number
  name: string
  ax: number; ay: number; bx: number; by: number
}

const COLOURS: Record<ZoneKind, string> = {
  perimeter: '#ef4b4b',
  restricted: '#f0a500',
  desk: '#4c8dff',
  // Green, because an exit zone is the one kind where being inside it is
  // entirely normal — the other three are drawn in alarm colours and an
  // operator reads colour before text.
  exit: '#2ea36a',
}

interface Props {
  imageUrl: string
  zones: DrawnZone[]
  lines: DrawnLine[]
  mode: 'idle' | 'zone' | 'line'
  drawKind: ZoneKind
  onZoneComplete(polygon: Point[]): void
  onLineComplete(a: Point, b: Point): void
}

/**
 * Draw zones and a gate line on a still frame from the camera.
 *
 * Coordinates are stored in the SOURCE frame's pixels, not the canvas's. The
 * canvas is scaled to fit the page, so without that conversion a zone drawn on
 * a laptop would sit somewhere else on a phone - and somewhere else again on
 * the camera.
 */
export default function ZoneCanvas(props: Props) {
  const { imageUrl, zones, lines, mode, drawKind, onZoneComplete, onLineComplete } = props
  const canvasRef = useRef<HTMLCanvasElement>(null)
  const imgRef = useRef<HTMLImageElement | null>(null)
  const [draft, setDraft] = useState<Point[]>([])
  const [hover, setHover] = useState<Point | null>(null)
  const [ready, setReady] = useState(false)
  const [why, setWhy] = useState('')

  // KEEP ASKING UNTIL THERE IS A PICTURE.
  //
  // A camera only draws and encodes while somebody is watching it, so the first
  // request after this page opens routinely answers 503 — there is genuinely no
  // frame yet, and one arrives a second or two later. The first version asked
  // once, gave up, and left the operator looking at an error beside a live tile
  // that was working perfectly; the only way out was the Refresh button.
  //
  // Worse, its own diagnostic lied. It asked the server WHY only after the load
  // had failed, and by then the frame existed — so it reported "HTTP 200, this
  // is a fault" for the one case that is not a fault at all. Retrying removes
  // both the error and the false explanation.
  useEffect(() => {
    let cancelled = false
    let timer: number | undefined
    let attempt = 0
    setWhy('')
    setReady(false)

    const give_up_after = 12          // ~20 s at the delays below

    const load = () => {
      if (cancelled) return
      attempt += 1
      // A fresh query each time: a 503 is cacheable in principle, and reusing
      // the URL would re-read the failure instead of asking again.
      const url = imageUrl + (imageUrl.includes('?') ? '&' : '?') + 'try=' + attempt
      const img = new Image()
      img.onload = () => {
        if (cancelled) return
        imgRef.current = img
        setWhy('')
        setReady(true)
      }
      img.onerror = () => {
        if (cancelled) return
        imgRef.current = null
        setReady(false)
        if (attempt < give_up_after) {
          // Quick at first — the common case is a camera warming up — then
          // slower, so a camera that is genuinely gone is not hammered.
          timer = window.setTimeout(load, attempt < 4 ? 700 : 2000)
          return
        }
        // Out of patience: now ask what the server actually says, and report it.
        fetch(url)
          .then(r => {
            if (cancelled) return
            setWhy(r.status === 503
              ? 'the camera has not produced a picture in the last 20 seconds'
              : `the server answered HTTP ${r.status} for this frame`)
          })
          .catch(() => { if (!cancelled) setWhy('the frame request did not reach the server') })
      }
      img.src = url
    }

    load()
    return () => {
      cancelled = true
      if (timer) window.clearTimeout(timer)
    }
  }, [imageUrl])

  useEffect(() => { setDraft([]) }, [mode, drawKind])

  useEffect(() => {
    const cv = canvasRef.current
    const img = imgRef.current
    if (!cv || !img) return
    cv.width = img.naturalWidth
    cv.height = img.naturalHeight
    const g = cv.getContext('2d')!
    g.drawImage(img, 0, 0)

    const poly = (pts: Point[], colour: string, label: string, close: boolean) => {
      if (pts.length === 0) return
      g.beginPath()
      g.moveTo(pts[0][0], pts[0][1])
      for (const p of pts.slice(1)) g.lineTo(p[0], p[1])
      if (close) g.closePath()
      g.strokeStyle = colour
      g.lineWidth = Math.max(2, cv.width / 480)
      g.stroke()
      if (close) { g.fillStyle = colour + '22'; g.fill() }
      for (const p of pts) {
        g.beginPath()
        g.arc(p[0], p[1], Math.max(3, cv.width / 300), 0, Math.PI * 2)
        g.fillStyle = colour
        g.fill()
      }
      if (label) {
        g.font = `${Math.max(13, cv.width / 55)}px system-ui`
        g.fillStyle = colour
        g.fillText(label, pts[0][0] + 8, pts[0][1] - 8)
      }
    }

    for (const z of zones)
      poly(z.polygon, COLOURS[z.type],
           z.type === 'desk' && z.employee_name ? `${z.name} — ${z.employee_name}` : z.name,
           true)

    for (const l of lines) {
      g.beginPath()
      g.moveTo(l.ax, l.ay)
      g.lineTo(l.bx, l.by)
      g.strokeStyle = '#2fbf71'
      g.lineWidth = Math.max(3, cv.width / 400)
      g.stroke()
      // An arrow showing which way counts as IN, so the direction is visible
      // rather than something the operator has to remember.
      const mx = (l.ax + l.bx) / 2, my = (l.ay + l.by) / 2
      const dx = l.bx - l.ax, dy = l.by - l.ay
      const n = Math.hypot(dx, dy) || 1
      const px = -dy / n, py = dx / n
      const len = Math.max(24, cv.width / 24)
      g.beginPath()
      g.moveTo(mx, my)
      g.lineTo(mx + px * len, my + py * len)
      g.stroke()
      g.font = `${Math.max(13, cv.width / 55)}px system-ui`
      g.fillStyle = '#2fbf71'
      g.fillText('IN', mx + px * len + 6, my + py * len + 4)
    }

    if (draft.length) {
      const pts = hover && mode !== 'idle' ? [...draft, hover] : draft
      poly(pts, mode === 'line' ? '#2fbf71' : COLOURS[drawKind], '', false)
    }
  }, [zones, lines, draft, hover, ready, mode, drawKind])

  const toSource = (e: React.MouseEvent<HTMLCanvasElement>): Point => {
    const cv = canvasRef.current!
    const r = cv.getBoundingClientRect()
    // The canvas is displayed scaled; the stored point must be in the source
    // frame's own pixels.
    return [
      Math.round(((e.clientX - r.left) / r.width) * cv.width),
      Math.round(((e.clientY - r.top) / r.height) * cv.height),
    ]
  }

  const click = (e: React.MouseEvent<HTMLCanvasElement>) => {
    if (mode === 'idle') return
    const p = toSource(e)
    if (mode === 'line') {
      if (draft.length === 0) setDraft([p])
      else { onLineComplete(draft[0], p); setDraft([]) }
      return
    }
    setDraft(d => [...d, p])
  }

  const finish = () => {
    if (mode === 'zone' && draft.length >= 3) onZoneComplete(draft)
    setDraft([])
  }

  if (!ready)
    return why
      ? <div className="err">
          <b>No picture to draw on — {why}.</b> The camera has to be delivering video
          before a zone or a line can be placed on it. Check it on the live wall, then
          press <b>Refresh frame</b>.
        </div>
      : <div className="note">
          Waiting for a picture from this camera — it starts sending one a second or
          two after this page opens. This keeps trying on its own; nothing to press.
        </div>

  return (
    <div>
      <div className="canvas-wrap">
        <canvas
          ref={canvasRef}
          onClick={click}
          onDoubleClick={finish}
          onMouseMove={e => mode !== 'idle' && setHover(toSource(e))}
          onMouseLeave={() => setHover(null)}
        />
      </div>
      {mode === 'zone' && (
        <div className="row" style={{ marginTop: 10 }}>
          <span className="muted">
            Click to add a point ({draft.length} so far). Double-click to close the shape.
          </span>
          <button className="ghost" onClick={() => setDraft([])} disabled={!draft.length}>
            Clear points
          </button>
          <button onClick={finish} disabled={draft.length < 3}>Save zone</button>
        </div>
      )}
      {mode === 'line' && (
        <div className="row" style={{ marginTop: 10 }}>
          <span className="muted">
            {draft.length === 0 ? 'Click one side of the doorway.' : 'Now click the other side.'}
          </span>
          <button className="ghost" onClick={() => setDraft([])} disabled={!draft.length}>
            Start over
          </button>
        </div>
      )}
    </div>
  )
}
