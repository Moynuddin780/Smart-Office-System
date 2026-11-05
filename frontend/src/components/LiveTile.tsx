import { useEffect, useRef, useState } from 'react'
import { mediaUrl } from '../api'

/**
 * A camera's live picture.
 *
 * WHY THIS POLLS INSTEAD OF USING THE MJPEG STREAM
 * ------------------------------------------------
 * `multipart/x-mixed-replace` is the obvious way to do this and it is what this
 * used, but the server side of it is a synchronous callback that Drogon invokes
 * on one of its event loops. Pacing a stream there means SLEEPING on that loop,
 * and while it sleeps no API request assigned to the same loop is answered.
 * Four tiles on the dashboard were enough to occupy every loop: measured, a
 * request that takes 2 ms with the tiles closed took 11 s with them open.
 *
 * Polling a still instead costs one short request per frame. The camera worker
 * keeps the newest frame already JPEG-encoded, so the server side is a copy.
 *
 * TWO RULES THIS COMPONENT EXISTS TO ENFORCE
 * -------------------------------------------
 * 1. A REQUEST THAT NEVER ANSWERS MUST NOT STOP THE TILE. The first version
 *    cleared its in-flight flag only in onload/onerror. A hung connection fires
 *    neither, so the flag stayed set and the tile froze on whatever frame it had
 *    — permanently. That is exactly what a camera being unplugged and moved
 *    produces, and the symptom was a dashboard still showing the room the
 *    camera used to point at.
 *
 * 2. AN OLD PICTURE MUST NOT PASS AS A LIVE ONE. If no fresh frame has arrived
 *    for a few seconds the tile says so, with the age. Showing a stale frame
 *    silently is worse than showing nothing: it reads as a room being watched
 *    when nobody is watching it.
 */
export default function LiveTile(
  { cameraId, fps = 6, style, className }:
  { cameraId: number; fps?: number; style?: React.CSSProperties; className?: string },
) {
  const [src, setSrc] = useState('')
  const [age, setAge] = useState<number | null>(null)   // seconds since the last frame
  const [everLoaded, setEverLoaded] = useState(false)

  const alive = useRef(true)
  const reqId = useRef(0)          // identifies the request currently owned
  const startedAt = useRef(0)      // when it was issued, for the watchdog
  const pending = useRef(false)
  const lastOk = useRef(0)

  useEffect(() => {
    alive.current = true
    pending.current = false
    lastOk.current = 0
    setSrc(''); setAge(null); setEverLoaded(false)

    const period = Math.max(60, Math.round(1000 / Math.max(1, fps)))
    // Generous enough that a slow-but-working answer is not abandoned, short
    // enough that a dead connection costs a few seconds rather than the session.
    const deadline = Math.max(5000, period * 6)

    const tick = () => {
      if (!alive.current) return

      const now = Date.now()
      if (lastOk.current) setAge(Math.round((now - lastOk.current) / 1000))

      // Only one request at a time — unless the one we have has blown its
      // deadline, in which case it is abandoned and a fresh one goes out.
      if (pending.current && now - startedAt.current < deadline) return

      const mine = ++reqId.current
      pending.current = true
      startedAt.current = now

      const url = mediaUrl(`/api/cameras/${cameraId}/snapshot.jpg?t=${now}`)
      const img = new Image()
      const settle = (ok: boolean) => {
        // A late reply from a request we already gave up on must not be
        // mistaken for the current one, nor unblock the newer request.
        if (mine !== reqId.current || !alive.current) return
        pending.current = false
        if (!ok) return
        lastOk.current = Date.now()
        setSrc(url)
        setAge(0)
        setEverLoaded(true)
      }
      img.onload = () => settle(true)
      img.onerror = () => settle(false)
      img.src = url
    }

    tick()
    const timer = window.setInterval(tick, period)
    return () => {
      // Stopping matters as much as starting: the server only draws overlays
      // and encodes while somebody is asking, and the lease it keeps lapses a
      // few seconds after the last request. Closing the tab stops the work.
      alive.current = false
      reqId.current++          // orphan anything still outstanding
      window.clearInterval(timer)
    }
  }, [cameraId, fps])

  const stale = age !== null && age >= 5

  return (
    <div className={className}
         style={{ position: 'relative', background: '#0b0e12', overflow: 'hidden', ...style }}>
      {src
        ? <img src={src} alt=""
               style={{ width: '100%', height: '100%', display: 'block', objectFit: 'contain',
                        // Drained of colour so a frozen picture cannot be read
                        // as a live one at a glance across a wall of tiles.
                        filter: stale ? 'grayscale(1) brightness(0.65)' : 'none' }} />
        : <div style={{ position: 'absolute', inset: 0, display: 'flex', alignItems: 'center',
                        justifyContent: 'center', color: '#8592a3', fontSize: 12 }}>
            {everLoaded ? 'No picture' : 'Connecting…'}
          </div>}

      {stale && (
        <div style={{ position: 'absolute', left: 0, right: 0, bottom: 0,
                      background: 'rgba(179,38,30,.92)', color: '#fff', fontSize: 11.5,
                      padding: '5px 8px', textAlign: 'center', letterSpacing: '.2px' }}>
          Not live — last picture {age}s ago
        </div>
      )}
    </div>
  )
}
