import { useCallback, useEffect, useRef, useState } from 'react'
import { api, mediaUrl } from '../api'
import { Btn, Chip, Ic } from './ui'
import LiveTile from './LiveTile'

/* ── Guided live-camera enrolment ──────────────────────────────────────────
   Walks the person through a fixed set of head poses, speaking each one and
   capturing a still at the end of it.

   WHY POSES AT ALL. An embedding built from several near-identical straight-on
   shots describes one viewing angle well and every other angle badly, which is
   why somebody enrolled that way is recognised at the door and missed halfway
   down a corridor. Left, right, up, down and the diagonals give the profile
   some spread.

   WHAT IS SAVED. The preview is the live MJPEG WITH detection drawn, purely so
   the person can see that a face is being found and where. What is captured is
   a clean frame (?clean=1) — a box drawn over a face would otherwise become
   part of the pixels the embedding is computed from.

   Speech is the browser's own speechSynthesis: no dependency, no network, and
   it falls back to the on-screen prompt on a device with no voices.          */

interface Camera { id: number; name: string }
interface Props { employeeId: number; onDone(result: unknown): void; onCancel(): void }

/* Twenty poses, twenty DISTINCT spoken prompts, one image each.
 *
 * Twenty rather than six because the profile vector is the mean of its images
 * and therefore describes only the angles it was given. Six near-frontal shots
 * produce a profile that recognises somebody at the door and misses them three
 * steps later.
 *
 * No two lines are worded alike on purpose: hearing "turn left" twice makes a
 * person think the capture has stalled and hold still, which is precisely the
 * duplicate frame the spread exists to avoid. */
const STEPS = [
  { say: 'Look straight at the camera and keep your face still', label: 'Straight ahead' },
  { say: 'Stay facing forward, and give a natural smile', label: 'Smile' },
  { say: 'Keep looking ahead, and say your own name out loud', label: 'Speaking' },
  { say: 'Turn your face slightly to the left', label: 'Slight left' },
  { say: 'Now turn further to the left, about halfway', label: 'Half left' },
  { say: 'Turn to your left as far as is comfortable', label: 'Full left' },
  { say: 'Come back to the centre and face the camera again', label: 'Back to centre' },
  { say: 'Turn your face slightly to the right', label: 'Slight right' },
  { say: 'Now turn further to the right, about halfway', label: 'Half right' },
  { say: 'Turn to your right as far as is comfortable', label: 'Full right' },
  { say: 'Face forward once more and lower your chin a little', label: 'Chin down' },
  { say: 'Lower your head a bit further, as if reading', label: 'Looking down' },
  { say: 'Now raise your chin slightly above level', label: 'Chin up' },
  { say: 'Lift your head a little more, looking up', label: 'Looking up' },
  { say: 'Tilt your head towards your left shoulder', label: 'Tilt left' },
  { say: 'Now tilt your head towards your right shoulder', label: 'Tilt right' },
  { say: 'Turn left and look down at the same time', label: 'Left and down' },
  { say: 'Turn right and look up at the same time', label: 'Right and up' },
  { say: 'Look away past the camera, to one side', label: 'Glancing away' },
  { say: 'Finally, face the camera straight on and hold still', label: 'Straight ahead again' },
]

const POSE_HOLD_MS = 1500     // long enough to hear and move, ~30s for twenty
const MAX_TRIES_PER_POSE = 4  // then move on, and come back to it at the end
const MAX_TOTAL_SHOTS = 70    // hard stop, so a bad camera cannot loop forever

/** A rejection reason from the server, turned into something a person can act
 *  on. "Quality too low" tells them nothing; "come a little closer" fixes it. */
function coachFor(p: { reason?: string; other_faces?: number }): [string, string] {
  const r = (p.reason || '').toLowerCase()
  if ((p.other_faces ?? 0) > 0) return ['Only one person in frame, please', 'Someone else is in shot']
  if (r.includes('no face')) return ['I cannot see your face — look at the camera', 'No face detected']
  if (r.includes('too small')) return ['Please come closer to the camera', 'Too far away']
  if (r.includes('turned')) return ['Turn a little more towards the camera', 'Head turned too far']
  if (r.includes('unclear')) return ['Move into better light', 'Too dark or unclear']
  return ['Let us try that one again', 'Retaking this pose']
}

function speak(text: string) {
  try {
    const s = window.speechSynthesis
    if (!s) return
    s.cancel()
    const u = new SpeechSynthesisUtterance(text)
    u.rate = 0.98
    s.speak(u)
  } catch { /* a device with no voices falls back to the on-screen prompt */ }
}

export default function GuidedCapture({ employeeId, onDone, onCancel }: Props) {
  const [cams, setCams] = useState<Camera[]>([])
  const [camId, setCamId] = useState<number | null>(null)
  const [running, setRunning] = useState(false)
  const [step, setStep] = useState(0)
  const [taken, setTaken] = useState<{ label: string; quality: number }[]>([])
  const [prompt, setPrompt] = useState('')
  const [coach, setCoach] = useState('')
  const [err, setErr] = useState('')
  const [voice, setVoice] = useState(true)
  const abort = useRef(false)

  useEffect(() => {
    api.get<Camera[]>('/api/cameras')
      .then(c => { setCams(c); if (c.length) setCamId(c[0].id) })
      .catch(e => setErr(String(e.message)))
  }, [])

  useEffect(() => () => { abort.current = true; try { window.speechSynthesis?.cancel() } catch {} }, [])

  const grabClean = useCallback(async (id: number): Promise<Blob | null> => {
    // ?clean=1 — the undrawn frame. See the note at the top of this file.
    const res = await fetch(mediaUrl(`/api/cameras/${id}/snapshot.jpg?clean=1&n=${Date.now()}`))
    if (!res.ok) return null
    return res.blob()
  }, [])

  const run = useCallback(async () => {
    if (!camId) return
    abort.current = false
    setRunning(true); setErr(''); setTaken([]); setStep(0); setCoach('')

    const pending = STEPS.map((_, i) => i)
    const done: { label: string; quality: number }[] = []
    let shots = 0

    // Poses that keep failing are set aside and retried at the end rather than
    // blocking the run — somebody who cannot hold "tilt right" should still
    // finish with nineteen good frames.
    const queue = [...pending]
    const deferred: number[] = []
    let tries: Record<number, number> = {}

    const attempt = async (idx: number): Promise<boolean> => {
      const s = STEPS[idx]
      setStep(idx)
      setPrompt(s.say)
      if (voice) speak(s.say)
      await new Promise(r => setTimeout(r, POSE_HOLD_MS))
      if (abort.current) return false

      const blob = await grabClean(camId)
      if (!blob) { setCoach('The camera is not delivering a frame'); return false }
      shots += 1

      const fd = new FormData()
      fd.append('mode', 'add')
      fd.append('source', 'capture')
      fd.append('photo', blob, `${s.label.replace(/\s+/g, '-').toLowerCase()}.jpg`)
      try {
        const r = await api.upload<{ photos: { accepted: boolean; quality: number; reason?: string; other_faces?: number }[] }>(
          `/api/employees/${employeeId}/enroll`, fd)
        const p = r.photos?.[0]
        if (p?.accepted) {
          done.push({ label: s.label, quality: p.quality })
          setTaken([...done])
          setCoach('')
          return true
        }
        const [spoken, shown] = coachFor(p || {})
        setCoach(shown)
        if (voice) speak(spoken)
        await new Promise(r2 => setTimeout(r2, 900))
        return false
      } catch (e) {
        // A 400 from the new-profile minimum is expected while the first four
        // photos are still being collected — the server refuses to CREATE a
        // profile under five, so the run keeps going and the last call lands it.
        const msg = e instanceof Error ? e.message : ''
        if (msg.includes('needs at least')) { done.push({ label: s.label, quality: 0 }); setTaken([...done]); return true }
        setCoach(msg || 'that one did not go through')
        return false
      }
    }

    while (queue.length && shots < MAX_TOTAL_SHOTS && !abort.current) {
      const idx = queue.shift()!
      const ok = await attempt(idx)
      if (!ok) {
        tries[idx] = (tries[idx] || 0) + 1
        if (tries[idx] < MAX_TRIES_PER_POSE) queue.push(idx)
        else deferred.push(idx)
      }
    }
    for (const idx of deferred) {
      if (abort.current || shots >= MAX_TOTAL_SHOTS) break
      await attempt(idx)
    }

    setRunning(false)
    setPrompt('')
    if (voice) speak(done.length >= 5 ? 'Enrolment complete, thank you' : 'Enrolment did not collect enough usable photos')
    onDone({ captured: done.length, shots })
  }, [camId, employeeId, grabClean, onDone, voice])

  if (err) return <div className="err">{err}</div>
  if (!cams.length)
    return <div className="note">There are no cameras configured yet. Guided capture needs a
      camera to look at — add one, or use the upload tab instead.</div>

  return (
    <div>
      <div className="row" style={{ marginBottom: 12 }}>
        <label style={{ margin: 0, flex: 1 }}>
          <span>Camera</span>
          <select value={camId ?? ''} disabled={running}
                  onChange={e => setCamId(Number(e.target.value))}>
            {cams.map(c => <option key={c.id} value={c.id}>{c.name}</option>)}
          </select>
        </label>
        <label style={{ margin: 0, alignSelf: 'flex-end', display: 'flex', alignItems: 'center',
                        gap: 7, whiteSpace: 'nowrap' }}>
          <input type="checkbox" checked={voice} disabled={running}
                 onChange={e => setVoice(e.target.checked)} style={{ width: 'auto' }} />
          <span style={{ margin: 0, textTransform: 'none', letterSpacing: 0, fontSize: 13 }}>
            <Ic k="mic" s={13} style={{ display: 'inline-block', verticalAlign: '-2px' }} /> Speak the prompts
          </span>
        </label>
      </div>

      {camId && (
        <div className="canvas-wrap" style={{ width: '100%' }}>
          <LiveTile cameraId={camId} fps={6}
               style={{ width: '100%', display: 'block' }} />
        </div>
      )}

      <div style={{ margin: '14px 0' }}>
        {running ? (
          <>
            <div style={{ fontSize: 16, fontWeight: 600, marginBottom: 4 }}>
              {STEPS[step]?.label}
            </div>
            <div className="muted" style={{ fontSize: 13.5 }}>{prompt}</div>
            {coach && <div className="note" style={{ marginTop: 10 }}>{coach}</div>}
          </>
        ) : (
          <div className="muted" style={{ fontSize: 13 }}>
            Twenty short poses, about thirty seconds. Each one is spoken aloud. Stand so your
            face fills a good part of the frame — below roughly 80&nbsp;px a photo is refused
            and the pose is asked for again.
          </div>
        )}
      </div>

      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 5, marginBottom: 14 }}>
        {STEPS.map((s, i) => {
          const got = taken.find(t => t.label === s.label)
          return (
            <span key={s.label} title={s.say}
                  style={{ fontSize: 10.5, padding: '2px 7px', borderRadius: 'var(--r-sm)',
                           border: '1px solid',
                           borderColor: got ? '#c4e5d3' : running && i === step ? 'var(--accent)' : 'var(--line)',
                           background: got ? 'var(--ok-soft)' : running && i === step ? 'var(--accent-soft)' : 'var(--surface-2)',
                           color: got ? 'var(--ok)' : running && i === step ? 'var(--accent)' : 'var(--text-3)',
                           fontWeight: got || (running && i === step) ? 650 : 500 }}>
              {got ? '✓ ' : ''}{s.label}
            </span>
          )
        })}
      </div>

      <div className="row">
        {running ? (
          <>
            <Btn kind="danger" icon="x" onClick={() => { abort.current = true }}>Stop</Btn>
            <Chip kind="info">{taken.length} of {STEPS.length} captured</Chip>
          </>
        ) : (
          <>
            <Btn kind="primary" icon="play" onClick={run} disabled={!camId}>Start guided capture</Btn>
            <Btn onClick={onCancel}>Cancel</Btn>
            {taken.length > 0 && <Chip kind="ok">{taken.length} captured</Chip>}
          </>
        )}
      </div>
    </div>
  )
}
