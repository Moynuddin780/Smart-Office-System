import { useEffect, useMemo, useState } from 'react'
import { Link, useNavigate } from 'react-router-dom'
import { api } from '../api'
import { Chip, Ic } from '../components/ui'
import ModulePicker, { ModuleRow } from '../components/ModulePicker'

interface Camera {
  id: number; name: string; rtsp_main: string; rtsp_sub: string
  stream_pref: string; role: string; face_enabled: boolean; enabled: boolean
  detect_hz: number; face_hz: number; probed_width: number; probed_height: number
  max_px: number
  max_fps: number
  site: string; location: string
}

/// What a camera says it really sends, from opening the stream rather than
/// from whatever was typed or remembered.
interface StreamFacts {
  ok: boolean; live?: boolean
  width?: number; height?: number; fps?: number
  decode_path?: string; label?: string; error?: string
}
interface ProbeResult { main: StreamFacts; sub: StreamFacts }

/// What a camera says about itself over ONVIF.
///
/// `streams` is in the camera's own order, which is main first and sub second
/// on every device on this site. The names are not usable as keys — one vendor
/// calls them "D01_CH01_Main"/"D01_CH01_Sub", another "MainStream", another
/// "Profile_1" — so position is what the two buttons go by.
interface OnvifStream {
  profile: string; token: string; encoding: string
  width: number; height: number; fps: number
  bitrate: number; gov_length: number
  fps_min: number; fps_max: number
  resolutions: { width: number; height: number }[]
}
interface OnvifResult { ok: boolean; streams: OnvifStream[]; error?: string }

type Draft = Omit<Camera, 'id'>

const blank: Draft = {
  name: '', rtsp_main: '', rtsp_sub: '', stream_pref: 'sub', role: 'area',
  face_enabled: false, enabled: true, detect_hz: 10, face_hz: 6, max_px: 1000, max_fps: 0,
  probed_width: 0, probed_height: 0, site: '', location: '',
}

/// The two roles the product ships with. A site can add its own — see the role
/// field below — but these two are the ones the modules page knows by name.
const BUILTIN_ROLES = ['area', 'gate']

/// A live camera, and nothing else.
///
/// This check exists because a recorded file used to be accepted here during
/// development, and a demo clip looping on the wall next to real cameras is
/// worse than an empty wall: it looks like the system is watching a room that
/// nobody is actually watching. The backend rejects non-RTSP URLs too — this
/// only says so before the round trip.
function urlProblem(u: string): string {
  const s = u.trim()
  if (!s) return ''
  if (!/^rtsps?:\/\//i.test(s))
    return 'must start with rtsp:// — a live camera stream, not a file or a recording'
  if (!/^rtsps?:\/\/[^/\s]+/i.test(s)) return 'missing the camera host'
  return ''
}

export default function Cameras() {
  const navigate = useNavigate()
  const [rows, setRows] = useState<Camera[] | null>(null)
  const [form, setForm] = useState<Draft>({ ...blank })
  const [err, setErr] = useState('')
  const [open, setOpen] = useState(false)
  const [editing, setEditing] = useState<number | null>(null)
  const [busy, setBusy] = useState(false)
  const [mods, setMods] = useState<ModuleRow[] | null>(null)
  const [modSel, setModSel] = useState<Record<string, boolean>>({})
  const [siteFilter, setSiteFilter] = useState('')
  // What the camera says it actually sends, once somebody asks it.
  const [probe, setProbe] = useState<ProbeResult | null>(null)
  const [probing, setProbing] = useState(false)
  // What the camera says it COULD send — its own settings, over ONVIF.
  const [caps, setCaps] = useState<OnvifResult | null>(null)
  const [asking, setAsking] = useState(false)
  // Set while a resolution change is in flight, and the camera's own word on
  // how it went once it is not.
  const [applying, setApplying] = useState('')
  const [applied, setApplied] = useState<{ ok: boolean; text: string } | null>(null)

  // Ask the camera, rather than trusting the label beside the dropdown.
  //
  // "Sub-stream (recommended)" was advice given without opening the URL, and
  // the sizes stored next to it were wrong for every camera on this site. The
  // choice decides whether faces are big enough to recognise and whether the
  // board keeps up, so it is worth the few seconds this takes.
  const runProbe = async () => {
    if (!form.rtsp_main && !form.rtsp_sub) return
    setProbing(true); setProbe(null)
    try {
      // editing is the camera's own id, so a running camera answers from its
      // worker instead of being dialled a second time it would refuse
      setProbe(await api.post<ProbeResult>('/api/cameras/probe',
        { rtsp_main: form.rtsp_main, rtsp_sub: form.rtsp_sub,
          camera_id: editing ?? 0, stream_pref: form.stream_pref }))
    } catch (e: any) {
      setErr(String(e.message))
    } finally {
      setProbing(false)
    }
  }

  // Ask the camera for its own configuration.
  //
  // Separate from the probe above, and they answer different questions. The
  // probe opens the stream and sees what is coming out; this reads the camera's
  // settings and the list of what it will accept. A camera can be sending
  // 1920x1080 and still support five larger sizes, and only this call knows.
  const askCamera = async () => {
    if (!form.rtsp_main && !form.rtsp_sub) return
    setAsking(true); setCaps(null)
    try {
      setCaps(await api.post<OnvifResult>('/api/cameras/onvif',
        { rtsp_main: form.rtsp_main, rtsp_sub: form.rtsp_sub }))
    } catch (e: any) {
      setErr(String(e.message))
    } finally {
      setAsking(false)
    }
  }

  // Ask the camera to change, then show what it actually did.
  //
  // The reply reports the camera read back AFTER the write, not the write's own
  // answer — this hardware returns success for changes it does not make, and a
  // page that repeated that answer would be worse than one that cannot change
  // anything at all.
  const applyResolution = async (which: 'main' | 'sub', w: number, h: number) => {
    const c = capFor(which)
    if (!c || applying) return
    const key = `${w}x${h}`
    setApplying(key); setApplied(null)
    try {
      const r = await api.post<{ ok: boolean; error?: string; width?: number; height?: number }>(
        '/api/cameras/onvif/apply',
        { rtsp_main: form.rtsp_main, rtsp_sub: form.rtsp_sub,
          token: (c as any).token, width: w, height: h })
      setApplied({ ok: r.ok, text: r.ok ? `camera is now ${r.width}×${r.height}` : (r.error || 'the camera did not change') })
      // Re-read rather than assuming: the chips must show the camera, not
      // what was asked of it.
      await askCamera()
    } catch (e: any) {
      setApplied({ ok: false, text: String(e.message) })
    } finally {
      setApplying('')
    }
  }

  // The camera lists its profiles main-first; that order is what the two
  // buttons are. Index, not name, because the names are the vendor's
  // ("D01_CH01_Main", "MainStream", "Profile_1") and no two agree.
  const capFor = (which: 'main' | 'sub') =>
    caps?.streams?.[which === 'main' ? 0 : 1]

  const streamLabel = (which: 'main' | 'sub') => {
    const p = probe?.[which]
    if (!p) return ''
    // A dropdown option is no place for a paragraph. The full reason stays
    // under the control; here it is only ever "busy" or "no answer", because a
    // camera already streaming to this system simply refuses a second
    // connection, and that is not a fault to shout about.
    if (!p.ok) return ' — could not read'
    // The camera's OWN size, which is the thing being chosen between. What the
    // frame limit below cuts it down to is a separate decision and must not be
    // shown here: a 3200x1800 camera under a 1000 px limit once read back
    // "1000x562", offering our own setting as if it were the camera's.
    return ` — ${p.width}x${p.height} at ${p.fps} fps${p.live ? ' (in use now)' : ''}`
  }

  const load = () => api.get<Camera[]>('/api/cameras').then(setRows)
    .catch(e => setErr(String(e.message)))
  useEffect(() => { load() }, [])

  /// Every site in use, so the list can be narrowed to one building.
  const sites = useMemo(() => {
    const seen = new Set<string>()
    for (const r of rows ?? []) if (r.site) seen.add(r.site)
    return [...seen].sort()
  }, [rows])

  /// Every role already in use, so a role added once is offered again.
  const roles = useMemo(() => {
    const seen = new Set(BUILTIN_ROLES)
    for (const r of rows ?? []) if (r.role) seen.add(r.role)
    return [...seen]
  }, [rows])

  /// The catalogue when adding, this camera's real state when editing — the
  /// two replies have the same shape so one picker renders both.
  const loadModules = (cameraId?: number) => {
    setMods(null); setModSel({})
    api.get<ModuleRow[]>(cameraId === undefined ? '/api/modules'
                                                : `/api/cameras/${cameraId}/modules`)
      .then(setMods).catch(() => setMods([]))
  }

  const startAdd = () => {
    setForm({ ...blank }); setEditing(null); setOpen(true); setErr(''); loadModules(); setProbe(null)
  }
  const startEdit = (c: Camera) => {
    const { id, ...rest } = c
    setForm({ ...rest }); setEditing(id); setOpen(true); setErr(''); loadModules(id); setProbe(null)
    window.scrollTo({ top: 0, behavior: 'smooth' })
  }
  const close = () => {
    setOpen(false); setEditing(null); setForm({ ...blank }); setErr(''); setProbe(null)
    setMods(null); setModSel({})
  }

  const mainProblem = urlProblem(form.rtsp_main)
  const subProblem = urlProblem(form.rtsp_sub)
  const chosenMissing =
    form.stream_pref === 'sub' ? !form.rtsp_sub.trim() : !form.rtsp_main.trim()

  const save = async (e: React.FormEvent) => {
    e.preventDefault()
    setErr('')
    if (mainProblem || subProblem) { setErr('Fix the stream URL first.'); return }
    if (!form.name.trim()) { setErr('The camera needs a name.'); return }
    if (chosenMissing) {
      setErr(`You chose the ${form.stream_pref}-stream, so that URL cannot be empty.`)
      return
    }
    setBusy(true)
    try {
      // One request, so a camera is never briefly live with the wrong module
      // set. Only what the operator actually changed is sent.
      const body = Object.keys(modSel).length ? { ...form, modules: modSel } : form
      if (editing === null) {
        // Straight into the new camera's setup. Half the modules need a shape
        // drawn before they do anything, and a camera that lands back in a list
        // gives no hint that it is switched on and silent.
        const made = await api.post<{ id: number }>('/api/cameras', body)
        close(); load()
        navigate(`/cameras/${made.id}`)
        return
      }
      await api.patch(`/api/cameras/${editing}`, body)
      close(); load()
    } catch (x) {
      setErr(x instanceof Error ? x.message : 'could not save the camera')
    } finally { setBusy(false) }
  }

  const remove = async (c: Camera) => {
    // Deleting a camera takes its zones, gate lines and module settings with it
    // (the database cascades), so this asks once and says what goes.
    if (!confirm(
      `Delete "${c.name}"?\n\nIts zones, gate lines and module settings are deleted too. ` +
      `Recorded events and snapshots are kept.`)) return
    setErr('')
    try {
      await api.del(`/api/cameras/${c.id}`)
      if (editing === c.id) close()
      load()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not delete the camera') }
  }

  const toggleEnabled = async (c: Camera) => {
    try { await api.patch(`/api/cameras/${c.id}`, { enabled: !c.enabled }); load() }
    catch (x) { setErr(x instanceof Error ? x.message : 'could not change the camera') }
  }

  return (
    <>
      <div className="head">
        <div><h1>Cameras</h1>
          <div className="muted">
            Live RTSP streams only. Sub-streams by default — that is what lets several
            cameras share one NPU.
          </div></div>
        <div className="row">
          {sites.length > 0 && (
            <select value={siteFilter} onChange={e => setSiteFilter(e.target.value)}
                    style={{ width: 190 }}>
              <option value="">All sites</option>
              {sites.map(x => <option key={x} value={x}>{x}</option>)}
            </select>
          )}
          {open ? <button className="ghost" onClick={close}>Cancel</button>
                : <button onClick={startAdd}>Add camera</button>}
        </div>
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      {open && (
        <form className="card" style={{ marginBottom: 16 }} onSubmit={save}>
          <h2>{editing === null ? 'New camera' : `Edit camera #${editing}`}</h2>
          <div className="grid cols-2">
            <label><span>Name</span>
              <input value={form.name} autoFocus
                     onChange={e => setForm({ ...form, name: e.target.value })} /></label>

            <label><span>Role</span>
              <input list="so-roles" value={form.role} placeholder="area, gate, or your own"
                     onChange={e => setForm({ ...form, role: e.target.value })} />
              <datalist id="so-roles">
                {roles.map(r => <option key={r} value={r} />)}
              </datalist>
              <small className="muted">
                Pick one or type a new name. <b>gate</b> counts attendance across a gate
                line; every other role is a plain label — what a camera actually does is
                set per camera under <Link to="/modules">AI modules</Link>.
              </small></label>

            <label><span>Site</span>
              <input list="so-sites" value={form.site} placeholder="Banani Office"
                     onChange={e => setForm({ ...form, site: e.target.value })} />
              <datalist id="so-sites">{sites.map(x => <option key={x} value={x} />)}</datalist>
              <small className="muted">
                The building or branch. Cameras are grouped and filtered by it, so keep the
                spelling the same across one office. Leave it blank if there is only one.
              </small></label>

            <label><span>Location</span>
              <input value={form.location} placeholder="Ground floor, reception desk"
                     onChange={e => setForm({ ...form, location: e.target.value })} />
              <small className="muted">
                Where exactly it points. This is what an alert shows beside the camera name,
                so write it for somebody who has to walk there.
              </small></label>

            <label><span>Main stream URL</span>
              <input value={form.rtsp_main} placeholder="rtsp://user:pass@192.168.1.64:554/Streaming/Channels/101"
                     onChange={e => setForm({ ...form, rtsp_main: e.target.value })} />
              {mainProblem && <small className="bad">{mainProblem}</small>}</label>

            <label><span>Sub-stream URL</span>
              <input value={form.rtsp_sub} placeholder="rtsp://user:pass@192.168.1.64:554/Streaming/Channels/102"
                     onChange={e => setForm({ ...form, rtsp_sub: e.target.value })} />
              {subProblem && <small className="bad">{subProblem}</small>}</label>

            <label><span>Stream type</span>
              <div className="row" style={{ gap: 8, alignItems: 'stretch' }}>
                {(['main', 'sub'] as const).map(which => {
                  const c = capFor(which)
                  const on = form.stream_pref === which
                  return (
                    <button key={which} type="button"
                            onClick={() => setForm({ ...form, stream_pref: which })}
                            className={on ? 'mod-chip on' : 'mod-chip'}>
                      <span style={{ display: 'block' }}>
                        <span className="mod-name">
                          {which === 'main' ? 'Main stream' : 'Sub-stream'}
                        </span>
                        <span className="mod-sub">
                          {c
                            ? `${c.encoding} · ${c.width}×${c.height} · ${c.fps} fps`
                            : streamLabel(which).replace(/^ — /, '') || 'not read yet'}
                        </span>
                      </span>
                    </button>
                  )
                })}
              </div>

              {/* What the SELECTED camera stream will accept, from the camera. */}
              {(() => {
                const c = capFor(form.stream_pref === 'main' ? 'main' : 'sub')
                if (!c) return null
                return (
                  <div style={{ marginTop: 10 }}>
                    <span className="muted" style={{ fontSize: 12 }}>
                      This camera accepts, on the{' '}
                      {form.stream_pref === 'main' ? 'main' : 'sub'}-stream:
                    </span>
                    <div className="row" style={{ flexWrap: 'wrap', gap: 6, marginTop: 4 }}>
                      {c.resolutions.map(r => {
                        const now = r.width === c.width && r.height === c.height
                        const key = `${r.width}x${r.height}`
                        return (
                          <button key={key} type="button"
                                  disabled={!!applying || now}
                                  onClick={() => applyResolution(
                                    form.stream_pref === 'main' ? 'main' : 'sub',
                                    r.width, r.height)}
                                  className={now ? 'res-chip on' : 'res-chip'}>
                            {r.width}×{r.height}
                            {now && ' ✓'}
                            {applying === key && ' …'}
                          </button>
                        )
                      })}
                      {c.fps_max > 0 && (
                        <span className="res-chip">{c.fps_min}–{c.fps_max} fps</span>
                      )}
                    </div>
                    {applied && (
                      <small className={applied.ok ? 'muted' : 'bad'}>{applied.text}</small>
                    )}
                    <small className="muted">
                      Read from the camera, and ticked where it is set now. Click a
                      size to change the camera itself — the camera is then read
                      back, and what you see is what it really did, not what it
                      answered. <b>Frame rate is not offered here</b> because this
                      camera accepts a frame-rate change, reports success and keeps
                      sending the old rate; use <i>Frame rate limit</i> below, which
                      bounds it on this board instead.
                    </small>
                  </div>
                )
              })()}
              <div className="row" style={{ marginTop: 6, alignItems: 'center', gap: 8 }}>
                <button type="button" className="ghost" onClick={askCamera}
                        disabled={asking || (!form.rtsp_main && !form.rtsp_sub)}>
                  {asking ? 'Asking the camera…' : 'Read camera settings'}
                </button>
                <button type="button" className="ghost" onClick={runProbe}
                        disabled={probing || (!form.rtsp_main && !form.rtsp_sub)}>
                  {probing ? 'Opening the stream…' : 'Check what the camera sends'}
                </button>
                {caps && !caps.ok && (
                  <span className="muted" style={{ fontSize: 12 }}>{caps.error}</span>
                )}
                {probe && (probe.main?.error || probe.sub?.error) && (
                  <span className="muted" style={{ fontSize: 12 }}>
                    {probe.main?.error || probe.sub?.error}
                  </span>
                )}
                {probe && (probe.main?.ok || probe.sub?.ok) && (
                  <span className="muted" style={{ fontSize: 12 }}>
                    {probe.sub?.ok && probe.main?.ok &&
                      (probe.sub.height! >= probe.main.height!
                        ? 'both the same size — the sub-stream costs no less here'
                        : 'the sub-stream is cheaper; pick main only if faces are too small')}
                  </span>
                )}
              </div>
              <small className="muted">
                These are the sizes the <b>camera itself</b> sends, read by opening
                each stream — not what was typed, and not what the frame limit below
                cuts them down to. Pick the stream; the limit then decides how much
                of it this board carries. A face needs about 24&nbsp;px to be matched
                at all, and a person is roughly 7× their face, so a corridor camera
                needs far more frame than one people walk up to.
              </small></label>

            <label><span>Detection rate</span>
              <div className="row">
                <input type="number" min={1} max={60} value={form.detect_hz}
                       onChange={e => setForm({ ...form, detect_hz: Number(e.target.value) })} />
                <span className="muted" style={{ whiteSpace: 'nowrap' }}>per second</span>
              </div>
              <small className="muted">
                How often the NPU looks at this camera — <b>not</b> the video rate, which
                stays whatever the camera sends. 10/s catches a walking person and leaves
                the NPU free for other cameras; raise it for a busy gate, lower it for a
                quiet corridor.
              </small></label>

            <label><span>Face rate</span>
              <div className="row">
                <input type="number" min={1} max={30} value={form.face_hz}
                       onChange={e => setForm({ ...form, face_hz: Number(e.target.value) })} />
                <span className="muted" style={{ whiteSpace: 'nowrap' }}>per second</span>
              </div>
              <small className="muted">
                How often this camera looks for FACES, separately from detection above.
                Naming somebody needs several square-on looks, not one, so this is what
                decides whether a person walking through is recognised before they are
                past. Raise it when people are missed in a group; it cannot exceed the
                rate the board actually decodes, so check that first.
              </small></label>

            <label><span>Frame size limit</span>
              <div className="row">
                <input type="number" min={320} max={4096} step={40} value={form.max_px}
                       onChange={e => setForm({ ...form, max_px: Number(e.target.value) })} />
                <span className="muted" style={{ whiteSpace: 'nowrap' }}>px, long side</span>
              </div>
              <small className="muted">
                Only used when face recognition is on. The frame is scaled to this before
                anything looks at it, keeping its shape. Lower it if this camera is behind
                real time; raise it if faces here are too small to recognise. A camera
                people walk close to needs far less than a corridor camera.
              </small></label>

            <label><span>Frame rate limit</span>
              <div className="row">
                <input type="number" min={0} max={60} step={1} value={form.max_fps}
                       onChange={e => setForm({ ...form, max_fps: Number(e.target.value) })} />
                <span className="muted" style={{ whiteSpace: 'nowrap' }}>fps, 0 = no limit</span>
              </div>
              <small className="muted">
                Use this when the camera will not lower its own frame rate. Frames above
                this rate are thrown away right after decoding, before any scaling or
                colour conversion, so they cost almost nothing — the front-door camera
                here insists on sending 25/s however it is configured, and this is what
                stops the board paying for all of them. It does not reduce the decoding
                itself. Below the detection rate above it will starve recognition, so keep
                it equal or higher.
              </small></label>
          </div>

          <label className="row" style={{ marginTop: 4, alignItems: 'flex-start' }}>
            <input type="checkbox" style={{ width: 'auto', marginTop: 3 }} checked={form.face_enabled}
                   onChange={e => setForm({ ...form, face_enabled: e.target.checked })} />
            <span style={{ margin: 0 }}>
              Run face recognition on this camera
              <small className="muted" style={{ fontWeight: 400 }}>
                Identifies <i>who</i>, not just that somebody is there — needed for attendance
                and for unrecognised-person alerts. It costs more NPU time and stops this
                camera's frames being downscaled, so leave it off where you only need to know
                that a person is present.
              </small>
            </span>
          </label>
          <label className="row" style={{ alignItems: 'flex-start' }}>
            <input type="checkbox" style={{ width: 'auto', marginTop: 3 }} checked={form.enabled}
                   onChange={e => setForm({ ...form, enabled: e.target.checked })} />
            <span style={{ margin: 0 }}>
              Enabled — decode and run inference on it
              <small className="muted" style={{ fontWeight: 400 }}>
                Off keeps the camera and everything drawn on it, but stops opening the
                stream — for a camera that is away for repair, so you do not lose its zones.
              </small>
            </span>
          </label>

          {!form.enabled && (
            <div className="note" style={{ margin: '8px 0' }}>
              <b>This camera will not show a picture.</b> With <i>Enabled</i> off it is saved
              but never opened — no live view, no detection, no alerts. Tick it unless you are
              deliberately parking this camera.
            </div>
          )}

          {form.role === 'gate' && form.face_enabled && form.stream_pref === 'sub' && (
            <div className="note" style={{ margin: '8px 0' }}>
              A gate camera identifies people, and that needs roughly 60&nbsp;px of face — about
              450&nbsp;px of person. On a 640×360 sub-stream that is taller than the frame, so
              recognition cannot work there at any distance. Set the camera's own sub-stream to
              1280×720, or switch this to the main stream.
            </div>
          )}

          <div style={{ margin: '14px 0 4px', paddingTop: 12, borderTop: '1px solid var(--line-soft)' }}>
            <ModulePicker rows={mods} value={modSel} onChange={setModSel}
                          cameraId={editing ?? undefined} />
          </div>

          <div className="row" style={{ marginTop: 8 }}>
            <button disabled={busy}>{busy ? 'Saving…' : editing === null ? 'Add camera' : 'Save changes'}</button>
            <button type="button" className="ghost" onClick={close}>Cancel</button>
          </div>
        </form>
      )}

      <div className="card flush">
        <table>
          <thead><tr>
            <th>Name</th><th>Where</th><th>Role</th><th>Stream</th><th>Faces</th><th>Detect</th>
            <th style={{ textAlign: 'right' }}>Actions</th>
          </tr></thead>
          <tbody>
            {(rows ?? []).filter(c => !siteFilter || c.site === siteFilter).map(c => (
              <tr key={c.id} style={c.enabled ? undefined : { opacity: 0.55 }}>
                <td>
                  <Link to={`/cameras/${c.id}`}>{c.name}</Link>
                  {!c.enabled && <> <Chip kind="idle">disabled</Chip></>}
                </td>
                <td className="muted" style={{ maxWidth: 230 }}>
                  {c.location || <span style={{ color: 'var(--text-3)' }}>—</span>}
                  {c.site && <div style={{ fontSize: 11, color: 'var(--text-3)' }}>{c.site}</div>}
                </td>
                <td><span className={`pill ${c.role === 'gate' ? 'info' : 'absent'}`}>{c.role}</span></td>
                <td className="muted">
                  {c.stream_pref}
                  {c.probed_width > 0 && ` · ${c.probed_width}×${c.probed_height}`}
                </td>
                <td>{c.face_enabled ? 'on' : <span className="muted">off</span>}</td>
                <td className="muted">{c.detect_hz}/s</td>
                <td>
                  <div className="row" style={{ justifyContent: 'flex-end', gap: 6 }}>
                    <Link className="btn ghost" to={`/cameras/${c.id}`}>Set up</Link>
                    <button type="button" className="ghost" onClick={() => startEdit(c)}>Edit</button>
                    <button type="button" className="ghost" onClick={() => toggleEnabled(c)}>
                      {c.enabled ? 'Disable' : 'Enable'}
                    </button>
                    <button type="button" className="ghost danger" onClick={() => remove(c)}>
                      <span style={{ display: 'flex', alignItems: 'center', gap: 5 }}>
                        <Ic k="trash" s={14} /> Delete
                      </span>
                    </button>
                  </div>
                </td>
              </tr>))}
            {rows !== null && rows.length === 0 && (
              <tr><td colSpan={7} className="muted">
                No cameras yet — add one and its live feed appears on the dashboard and the wall.
              </td></tr>)}
            {rows === null && <tr><td colSpan={7} className="muted">Loading…</td></tr>}
          </tbody>
        </table>
      </div>
    </>
  )
}
