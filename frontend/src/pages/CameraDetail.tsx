import { useCallback, useEffect, useMemo, useState } from 'react'
import { Link, useParams } from 'react-router-dom'
import { api, mediaUrl } from '../api'
import { Chip, FilterBtn } from '../components/ui'
import LiveTile from '../components/LiveTile'
import ModulePicker, { type ModuleRow } from '../components/ModulePicker'
import ZoneCanvas, { type DrawnLine, type DrawnZone, type Point, type ZoneKind } from '../components/ZoneCanvas'

interface Employee { id: number; name: string; emp_code: string }
interface Camera {
  id: number; name: string; site: string; location: string; role: string
  face_enabled: boolean; enabled: boolean
}

/**
 * What each module lets you draw on this camera.
 *
 * THE POINT OF THIS TABLE: the page used to ask "what are you drawing?" and
 * offer desk / perimeter / restricted, which is backwards. An operator does not
 * set out to draw a polygon — they set out to make THIS camera watch for
 * something, and the shape is a consequence. So the question here is "which
 * module are you setting up", and the shape it needs follows from that. A
 * module that needs no shape never appears as a tab at all.
 */
const DRAWABLE: Record<string, {
  label: string
  kinds?: { id: ZoneKind; label: string; hint: string }[]
  line?: boolean
  needsEmployee?: boolean
  hint: string
}> = {
  perimeter: {
    label: 'Perimeter & restricted zones',
    kinds: [
      { id: 'perimeter', label: 'Perimeter', hint: 'alert when somebody stands inside' },
      { id: 'restricted', label: 'Restricted', hint: 'the same, raised as critical' },
    ],
    hint: 'Draw the area nobody should stand in. Entry is decided on the foot point, so ' +
          'somebody walking past the edge is not counted as inside.',
  },
  desk: {
    label: 'Desk on-duty',
    kinds: [{ id: 'desk', label: 'Desk', hint: 'time at a workstation' }],
    needsEmployee: true,
    hint: 'Draw the seat area and bind it to one employee. Time inside it becomes their ' +
          'daily desk record. A short absence does not end the session.',
  },
  out_count: {
    label: 'Out count & breaks',
    kinds: [{ id: 'exit', label: 'Exit', hint: 'the way out of the building' }],
    hint: 'Draw the doorway people leave through. Somebody who stands in it and then ' +
          'walks out of shot is recorded as leaving; somebody who turns round and comes ' +
          'back in is not. Entry is decided on the foot point, so walking past the door ' +
          'does not count. Nothing is recorded for a person who has not already been ' +
          'counted in today — the first in of the day is still their attendance.',
  },
  // ATTENDANCE IS NOT HERE, AND THAT IS THE POINT.
  //
  // It used to be, with a gate line to draw across the doorway: crossing it one
  // way was an entry, the other an exit. Attendance now counts somebody the
  // moment their face is recognised, so there is nothing to draw — the camera
  // on the way in is the whole configuration.
  //
  // This map is also what decides whether a module gets a drawing tab at all,
  // so leaving the entry behind would have gone on offering the line after the
  // server stopped asking for one: a button that changes nothing is worse than
  // no button, because somebody will press it and wait for an effect.
}

export default function CameraDetail() {
  const { id } = useParams()
  const [cam, setCam] = useState<Camera | null>(null)
  const [mods, setMods] = useState<ModuleRow[] | null>(null)
  const [zones, setZones] = useState<DrawnZone[]>([])
  const [lines, setLines] = useState<DrawnLine[]>([])
  const [employees, setEmployees] = useState<Employee[]>([])

  const [tab, setTab] = useState<string>('')          // which module is being set up
  const [kind, setKind] = useState<ZoneKind>('perimeter')
  const [drawing, setDrawing] = useState(false)
  const [zoneName, setZoneName] = useState('')
  const [employeeId, setEmployeeId] = useState(0)
  const [err, setErr] = useState('')
  const [nonce, setNonce] = useState(0)

  const loadShapes = useCallback(() => {
    api.get<DrawnZone[]>(`/api/cameras/${id}/zones`).then(setZones).catch(e => setErr(String(e.message)))
    api.get<DrawnLine[]>(`/api/cameras/${id}/gate-lines`).then(setLines).catch(() => {})
  }, [id])

  const loadModules = useCallback(() => {
    api.get<ModuleRow[]>(`/api/cameras/${id}/modules`).then(setMods).catch(() => setMods([]))
  }, [id])

  useEffect(() => {
    loadShapes(); loadModules()
    api.get<Employee[]>('/api/employees').then(setEmployees).catch(() => {})
    api.get<Camera[]>('/api/cameras')
      .then(all => setCam(all.find(c => String(c.id) === id) ?? null)).catch(() => {})
  }, [id, loadShapes, loadModules])

  /// Tabs: assigned modules that actually take a shape, in catalogue order.
  const tabs = useMemo(
    () => (mods ?? []).filter(m => m.enabled && DRAWABLE[m.id]),
    [mods])

  // Keep the selected tab valid as modules are switched on and off.
  useEffect(() => {
    if (tabs.length === 0) { setTab(''); setDrawing(false); return }
    if (!tabs.some(t => t.id === tab)) {
      const first = tabs[0]
      setTab(first.id)
      setKind(DRAWABLE[first.id].kinds?.[0].id ?? 'perimeter')
      setDrawing(false)
    }
  }, [tabs, tab])

  const spec = tab ? DRAWABLE[tab] : undefined

  const toggleModules = async (v: Record<string, boolean>) => {
    setErr('')
    try {
      await api.patch(`/api/cameras/${id}`, { modules: v })
      loadModules()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not change the modules') }
  }

  const saveZone = async (polygon: Point[]) => {
    setErr('')
    if (spec?.needsEmployee && !employeeId) {
      setErr('Pick the employee this desk belongs to first.'); return
    }
    try {
      await api.post(`/api/cameras/${id}/zones`, {
        type: kind,
        name: zoneName || (kind === 'desk' ? 'Desk' : 'Zone'),
        polygon, employee_id: kind === 'desk' ? employeeId : 0, enabled: true,
      })
      setZoneName(''); setDrawing(false); loadShapes(); loadModules()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not save the zone') }
  }

  const saveLine = async (a: Point, b: Point) => {
    setErr('')
    try {
      await api.post(`/api/cameras/${id}/gate-lines`,
                     { name: zoneName || 'Entrance', ax: a[0], ay: a[1], bx: b[0], by: b[1] })
      setZoneName(''); setDrawing(false); loadShapes(); loadModules()
    } catch (x) { setErr(x instanceof Error ? x.message : 'could not save the line') }
  }

  const removeZone = async (z: DrawnZone) => {
    if (!confirm(`Remove "${z.name}"? The drawing and its binding go; recorded history is kept.`)) return
    await api.del(`/api/zones/${z.id}`); loadShapes(); loadModules()
  }
  const removeLine = async (l: DrawnLine) => {
    if (!confirm(`Remove the gate line "${l.name}"? Nothing uses it — attendance now \n`
                 + `counts a person the moment their face is recognised.`)) return
    await api.del(`/api/gate-lines/${l.id}`); loadShapes(); loadModules()
  }

  // BOTH OF THESE USED TO END IN A FALL-THROUGH, and the fall-through is what
  // made an exit zone show up red and described as "feeds Perimeter &
  // restricted" — a zone the server had stored correctly as type 'exit'. A
  // default branch quietly absorbs every kind added later, so each one is
  // spelled out and an unknown kind is visibly unknown instead of silently
  // mislabelled as the first thing on the list.
  const colour = (t: ZoneKind) =>
    t === 'desk' ? '#4c8dff'
    : t === 'restricted' ? '#f0a500'
    : t === 'exit' ? '#2ea36a'
    : t === 'perimeter' ? '#ef4b4b'
    : '#9aa3ad'
  /// Which module a drawn shape feeds, so the list explains itself.
  const feeds = (t: ZoneKind | 'line') =>
    t === 'desk' ? 'Desk on-duty'
    : t === 'exit' ? 'Out count & breaks'
    : t === 'line' ? 'Attendance'
    : t === 'perimeter' || t === 'restricted' ? 'Perimeter & restricted'
    : 'nothing — this kind is not known to this page'

  // WHICH SHAPES BELONG TO THE TAB BEING SET UP.
  //
  // This named the two tabs that existed and returned nothing for any other,
  // so adding a third left its zones invisible under "already drawn" — and the
  // Remove button lives in that list, so a zone that could be created could not
  // be deleted. Driving it off the tab's own `kinds` means a new module gets
  // both for free, which is what should have happened the first time.
  const mine = (() => {
    const kinds = DRAWABLE[tab]?.kinds?.map(k => k.id)
    return kinds ? zones.filter(z => kinds.includes(z.type)) : []
  })()

  return (
    <>
      <div className="head">
        <div>
          <h1>{cam ? cam.name : 'Camera'}</h1>
          <div className="muted">
            {cam?.location || 'Set up what this camera watches for, then draw what each module needs.'}
            {cam?.site && <> · {cam.site}</>}
            {' · '}<Link to="/cameras">back to cameras</Link>
          </div>
        </div>
        <button className="ghost" onClick={() => setNonce(n => n + 1)}>Refresh frame</button>
      </div>
      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      <div className="grid" style={{ gridTemplateColumns: 'minmax(0,2fr) minmax(300px,1fr)' }}>
        <div className="card">
          <ZoneCanvas
            imageUrl={mediaUrl(`/api/cameras/${id}/snapshot.jpg?n=${nonce}`)}
            zones={zones} lines={lines}
            mode={drawing ? (spec?.line ? 'line' : 'zone') : 'idle'}
            drawKind={kind}
            onZoneComplete={saveZone} onLineComplete={saveLine}
          />
        </div>

        <div>
          <div className="card" style={{ marginBottom: 14 }}>
            <h2>Modules on this camera</h2>
            <ModulePicker rows={mods} value={{}} onChange={toggleModules} cameraId={Number(id)} />
          </div>

          <div className="card" style={{ marginBottom: 14 }}>
            <h2>Set up</h2>
            {mods === null ? <div className="muted">Loading…</div>
             : tabs.length === 0 ? (
              <div className="muted" style={{ fontSize: 12.5 }}>
                None of the modules switched on above need anything drawn.
                {(mods ?? []).some(m => m.enabled)
                  ? ' This camera is already doing its job.'
                  : ' Switch a module on to configure it.'}
              </div>
             ) : (
              <>
                {/* One tab per module that needs a shape — never a list of shapes. */}
                <div className="row" style={{ flexWrap: 'wrap', gap: 6, marginBottom: 10 }}>
                  {tabs.map(t => (
                    <FilterBtn key={t.id} active={tab === t.id}
                               onClick={() => {
                                 setTab(t.id); setDrawing(false)
                                 setKind(DRAWABLE[t.id].kinds?.[0].id ?? 'perimeter')
                               }}>
                      {DRAWABLE[t.id].label}
                    </FilterBtn>
                  ))}
                </div>

                {spec && <>
                  <div className="muted" style={{ fontSize: 12, marginBottom: 10 }}>{spec.hint}</div>

                  {spec.kinds && spec.kinds.length > 1 && (
                    <label><span>Alert level</span>
                      <select value={kind} onChange={e => setKind(e.target.value as ZoneKind)}>
                        {spec.kinds.map(k => (
                          <option key={k.id} value={k.id}>{k.label} — {k.hint}</option>
                        ))}
                      </select></label>
                  )}

                  <label><span>{spec.line ? 'Line name' : 'Zone name'}</span>
                    <input value={zoneName} onChange={e => setZoneName(e.target.value)}
                           placeholder={spec.line ? 'Main entrance'
                                      : kind === 'desk' ? 'Desk 1' : 'Server room'} /></label>

                  {spec.needsEmployee && (
                    <label><span>Employee at this desk</span>
                      <select value={employeeId} onChange={e => setEmployeeId(Number(e.target.value))}>
                        <option value={0}>— choose —</option>
                        {employees.map(e => (
                          <option key={e.id} value={e.id}>{e.name} ({e.emp_code})</option>
                        ))}
                      </select></label>
                  )}

                  <div className="row" style={{ marginTop: 6 }}>
                    <button onClick={() => setDrawing(d => !d)}>
                      {drawing ? 'Stop drawing'
                               : spec.line ? 'Draw gate line'
                               : `Draw ${kind === 'desk' ? 'desk' : kind} zone`}
                    </button>
                  </div>

                  {/* What already exists FOR THIS MODULE, next to the tool that
                      made it — rather than in one undifferentiated list. */}
                  <div style={{ marginTop: 12 }}>
                    <div className="stat-label" style={{ marginBottom: 6 }}>
                      Already drawn for this module
                    </div>
                    <ul className="zone-list" style={{ listStyle: 'none', margin: 0, padding: 0 }}>
                      {spec.line
                        ? lines.map(l => (
                            <li key={`l${l.id}`}>
                              <span className="swatch" style={{ background: '#2fbf71' }} />
                              <span style={{ flex: 1 }}>{l.name}</span>
                              <button className="ghost danger" onClick={() => removeLine(l)}>Remove</button>
                            </li>))
                        : mine.map(z => (
                            <li key={z.id}>
                              <span className="swatch" style={{ background: colour(z.type) }} />
                              <span style={{ flex: 1 }}>
                                {z.name}
                                {z.type === 'desk' && z.employee_name &&
                                  <span className="muted"> — {z.employee_name}</span>}
                                {z.type === 'restricted' && <> <Chip kind="warn">critical</Chip></>}
                              </span>
                              <button className="ghost danger" onClick={() => removeZone(z)}>Remove</button>
                            </li>))}
                      {((spec.line && lines.length === 0) || (!spec.line && mine.length === 0)) &&
                        <li className="muted">Nothing yet — draw one above.</li>}
                    </ul>
                  </div>
                </>}
              </>
            )}
          </div>

          {(zones.length > 0 || lines.length > 0) && (
            <div className="card" style={{ marginBottom: 14 }}>
              <h2>Everything on this camera</h2>
              <ul className="zone-list" style={{ listStyle: 'none', margin: 0, padding: 0 }}>
                {/* Removable from HERE as well as from the tab above. This list
                    is the only one that shows every shape regardless of which
                    module is selected, so it is the only place a zone belonging
                    to a switched-off module can be deleted at all. */}
                {zones.map(z => (
                  <li key={`a${z.id}`}>
                    <span className="swatch" style={{ background: colour(z.type) }} />
                    <span style={{ flex: 1 }}>{z.name}
                      <span className="muted" style={{ fontSize: 12 }}> · feeds {feeds(z.type)}</span>
                    </span>
                    <button className="ghost danger" onClick={() => removeZone(z)}>Remove</button>
                  </li>))}
                {/* Gate lines left over from when attendance counted crossings.
                    Nothing reads them now, and saying what they feed would be a
                    lie, so they are listed as what they are: removable. */}
                {lines.map(l => (
                  <li key={`al${l.id}`}>
                    <span className="swatch" style={{ background: '#9aa3ad' }} />
                    <span style={{ flex: 1 }}>{l.name}
                      <span className="muted" style={{ fontSize: 12 }}>
                        {' '}· an old gate line — attendance no longer uses one
                      </span>
                    </span>
                    <button className="ghost" onClick={() => removeLine(l)}>Remove</button>
                  </li>))}
              </ul>
            </div>
          )}

          <div className="card">
            <h2>Live</h2>
            <LiveTile cameraId={Number(id)} fps={5}
                      style={{ width: '100%', aspectRatio: '16 / 9', borderRadius: 4 }} />
            <div className="muted" style={{ fontSize: 12, marginTop: 6 }}>
              The camera only draws overlays while this is open.
            </div>
          </div>
        </div>
      </div>
    </>
  )
}
