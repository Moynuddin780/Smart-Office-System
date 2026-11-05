import { useCallback, useEffect, useRef, useState } from 'react'
import { api, mediaUrl } from '../api'
import GuidedCapture from '../components/GuidedCapture'
import { Btn, Card, Chip, Empty, Ic, Modal, MiniStat, SectionTitle } from '../components/ui'

interface Profile { images: number; quality: number; coherence: number | null
                    incoherent: boolean; understrength: boolean }
interface Warning { kind: string; text: string }
interface Row {
  employee_id: number; emp_code: string; name: string; department: string
  active: boolean; enrolled: boolean; profile: Profile | null; warnings: Warning[]
}
interface FaceImg {
  id: number; quality: number; yaw: number; face_px: number; det_score: number
  filename: string; source: string; created_at: string; has_image: boolean
}
interface PhotoResult {
  filename: string; accepted: boolean; quality: number; yaw: number
  face_px: number; other_faces: number; reason?: string
}

/** Colour a coherence number the way it actually behaves. Healthy profiles sit
 *  at 0.65 or above; below 0.5 the profile starts handing its name to other
 *  people, which is a failure rather than a nuance, so it is red and not amber. */
function coherenceColour(c: number | null): string | undefined {
  if (c === null) return undefined
  if (c < 0.50) return 'var(--crit)'
  if (c < 0.62) return 'var(--warn)'
  return 'var(--ok)'
}

export default function FaceRegistry() {
  const [rows, setRows] = useState<Row[] | null>(null)
  const [err, setErr] = useState('')
  const [detail, setDetail] = useState<Row | null>(null)
  const [enrolFor, setEnrolFor] = useState<Row | null>(null)
  const [clearing, setClearing] = useState<number | null>(null)

  // Take away a person's FACES, not the person.
  //
  // Deleting the employee would take their attendance history with them, and
  // the reason to come to this page is almost never that somebody has left —
  // it is that their photos are wrong and recognition has gone bad. So this
  // clears the photos and the profile and leaves the person, their code and
  // every day they were recorded present exactly where they were. Removing the
  // employee outright is on the Employees page, where the consequence is plain.
  const clearFaces = async (r: Row) => {
    if (!confirm(
      `Remove all ${r.profile?.images ?? 0} face photos for ${r.name}?\n\n` +
      'They stay an employee and keep their attendance history — but they will ' +
      'not be recognised again until new photos are enrolled.')) return
    setClearing(r.employee_id)
    try {
      await api.del(`/api/employees/${r.employee_id}/faces`)
      load()
    } catch (e: any) {
      setErr(String(e.message))
    } finally {
      setClearing(null)
    }
  }

  const load = useCallback(() => {
    api.get<Row[]>('/api/faces').then(setRows).catch(e => setErr(String(e.message)))
  }, [])
  useEffect(() => { load() }, [load])

  const enrolled = (rows ?? []).filter(r => r.enrolled)
  const flagged = enrolled.filter(r => r.warnings.length > 0)

  return (
    <>
      <div className="head">
        <div>
          <h1>Face registry</h1>
          <div className="muted">
            SCRFD detection into ArcFace embedding. A profile is one vector per person,
            recomputed from every photo that built it.
          </div>
        </div>
      </div>

      {err && <div className="err" style={{ marginBottom: 14 }}>{err}</div>}

      <div className="grid cols-4" style={{ marginBottom: 16 }}>
        <Card>
          <div className="stat-label">Enrolled</div>
          <div className="stat">{enrolled.length}<span className="muted"
            style={{ fontSize: 15, fontWeight: 400 }}> / {rows?.length ?? 0}</span></div>
        </Card>
        <Card>
          <div className="stat-label">Photos held</div>
          <div className="stat">{enrolled.reduce((a, r) => a + (r.profile?.images ?? 0), 0)}</div>
        </Card>
        <Card>
          <div className="stat-label">Needs attention</div>
          <div className="stat" style={{ color: flagged.length ? 'var(--warn)' : 'var(--text)' }}>
            {flagged.length}
          </div>
        </Card>
        <Card>
          <div className="stat-label">Not enrolled</div>
          <div className="stat" style={{ color: 'var(--text-3)' }}>
            {(rows?.length ?? 0) - enrolled.length}
          </div>
        </Card>
      </div>

      {flagged.length > 0 && (
        <div className="note" style={{ marginBottom: 16 }}>
          <b>{flagged.length} profile{flagged.length === 1 ? '' : 's'} need attention.</b>{' '}
          A profile can be full of individually high-scoring photos and still be unusable —
          open it to see why. Left alone, an incoherent profile does not simply fail to
          match its owner; it starts being proposed for other people.
        </div>
      )}

      {rows === null ? <div className="muted">Loading…</div>
       : rows.length === 0 ? (
        <Empty icon="users" title="No employees yet"
               text="Add employees first, then enrol their faces here." />
       ) : (
        <div className="grid" style={{ gridTemplateColumns: 'repeat(auto-fill,minmax(268px,1fr))' }}>
          {rows.map(r => (
            <Card key={r.employee_id} style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', gap: 8 }}>
                <div style={{ minWidth: 0 }}>
                  <div style={{ fontWeight: 650, fontSize: 14 }}>{r.name}</div>
                  <div className="muted mono" style={{ fontSize: 11.5 }}>{r.emp_code}</div>
                </div>
                {r.enrolled && r.profile
                  ? <Chip kind={r.profile.quality >= 80 ? 'ok' : r.profile.quality >= 60 ? 'warn' : 'bad'}>
                      {r.profile.quality}%
                    </Chip>
                  : <Chip kind="idle">not enrolled</Chip>}
              </div>

              {r.enrolled && r.profile && (
                <div className="grid" style={{ gridTemplateColumns: '1fr 1fr', gap: 8 }}>
                  <MiniStat label="Photos" value={r.profile.images} />
                  <MiniStat label="Coherence" tone={coherenceColour(r.profile.coherence)}
                            value={r.profile.coherence === null ? '—' : r.profile.coherence.toFixed(2)} />
                </div>
              )}

              {r.warnings.map((w, i) => (
                <div key={i} className="note" style={{ fontSize: 12, padding: '8px 10px' }}>
                  {w.text}
                </div>
              ))}

              <div className="row" style={{ marginTop: 'auto' }}>
                <Btn kind="primary" icon="plus" onClick={() => setEnrolFor(r)}>
                  {r.enrolled ? 'Add photos' : 'Enrol'}
                </Btn>
                {r.enrolled && <Btn icon="face" onClick={() => setDetail(r)}>Photos</Btn>}
                {r.enrolled && (
                  <Btn kind="danger" icon="trash" disabled={clearing === r.employee_id}
                       onClick={() => clearFaces(r)}>
                    {clearing === r.employee_id ? 'Removing…' : 'Remove faces'}
                  </Btn>
                )}
              </div>
            </Card>
          ))}
        </div>
      )}

      {detail && <ProfileDetail row={detail} onClose={() => { setDetail(null); load() }} />}
      {enrolFor && (
        <EnrolModal row={enrolFor}
                    onClose={() => { setEnrolFor(null); load() }} />
      )}
    </>
  )
}

/* ── one profile's photos ─────────────────────────────────────────────────
   The repair surface. A bad profile is usually two bad photos, and deleting
   them is far cheaper than re-enrolling a person — but only if you can see
   which two they are.                                                        */

function ProfileDetail({ row, onClose }: { row: Row; onClose: () => void }) {
  const [imgs, setImgs] = useState<FaceImg[] | null>(null)
  const [profile, setProfile] = useState<Profile | null>(row.profile)
  const [warnings, setWarnings] = useState<Warning[]>(row.warnings)
  const [busy, setBusy] = useState<number | null>(null)

  const load = useCallback(() => {
    api.get<{ images: FaceImg[]; profile: Profile | null; warnings: Warning[] }>(
      `/api/employees/${row.employee_id}/faces`)
      .then(r => { setImgs(r.images); setProfile(r.profile); setWarnings(r.warnings) })
      .catch(() => setImgs([]))
  }, [row.employee_id])
  useEffect(() => { load() }, [load])

  const remove = async (id: number) => {
    setBusy(id)
    try { await api.del(`/api/faces/${id}`); load() } finally { setBusy(null) }
  }

  return (
    <Modal title={row.name} width={760}
           sub={<>{row.emp_code}{profile ? ` · ${profile.images} photos · quality ${profile.quality}%` : ''}</>}
           onClose={onClose}>
      {warnings.map((w, i) => (
        <div key={i} className="note" style={{ marginBottom: 12 }}>{w.text}</div>
      ))}

      {imgs === null ? <div className="muted">Loading…</div>
       : imgs.length === 0 ? <div className="muted">No photos.</div>
       : (
        <div className="grid" style={{ gridTemplateColumns: 'repeat(auto-fill,minmax(132px,1fr))' }}>
          {imgs.map(im => (
            <div key={im.id} style={{ border: '1px solid var(--line)', borderRadius: 'var(--r-sm)',
                                      overflow: 'hidden', background: 'var(--surface-2)' }}>
              {im.has_image
                ? <img src={mediaUrl(`/api/faces/${im.id}/image`)} alt=""
                       style={{ width: '100%', aspectRatio: '1', objectFit: 'cover', display: 'block' }} />
                : <div style={{ aspectRatio: '1', display: 'grid', placeItems: 'center',
                                color: 'var(--text-3)' }}><Ic k="face" s={26} /></div>}
              <div style={{ padding: '6px 8px' }}>
                <div style={{ display: 'flex', justifyContent: 'space-between',
                              alignItems: 'center', gap: 6 }}>
                  <Chip kind={im.quality >= 80 ? 'ok' : im.quality >= 60 ? 'warn' : 'bad'}>
                    {im.quality}%
                  </Chip>
                  <button className="ghost" disabled={busy === im.id}
                          onClick={() => remove(im.id)} title="Delete this photo"
                          style={{ padding: '3px 6px' }}>
                    <Ic k="trash" s={13} />
                  </button>
                </div>
                <div className="muted" style={{ fontSize: 10.5, marginTop: 4,
                                                fontVariantNumeric: 'tabular-nums' }}>
                  {Math.round(im.face_px)} px · yaw {im.yaw.toFixed(2)}
                </div>
                <div className="muted" style={{ fontSize: 10, opacity: .8 }}>{im.source}</div>
              </div>
            </div>
          ))}
        </div>
      )}
    </Modal>
  )
}

/* ── enrol: upload or guided capture ───────────────────────────────────── */

function EnrolModal({ row, onClose }: { row: Row; onClose: () => void }) {
  const [tab, setTab] = useState<'capture' | 'upload'>(row.enrolled ? 'upload' : 'capture')
  const [mode, setMode] = useState<'add' | 'replace'>('add')
  const [results, setResults] = useState<PhotoResult[] | null>(null)
  const [warnings, setWarnings] = useState<Warning[]>([])
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState('')
  const fileRef = useRef<HTMLInputElement>(null)

  const upload = async (files: FileList) => {
    setBusy(true); setErr(''); setResults(null)
    try {
      const fd = new FormData()
      fd.append('mode', mode)
      fd.append('source', 'upload')
      for (const f of Array.from(files)) fd.append('photos', f)
      const r = await api.upload<{ photos: PhotoResult[]; warnings: Warning[] }>(
        `/api/employees/${row.employee_id}/enroll`, fd)
      setResults(r.photos); setWarnings(r.warnings || [])
    } catch (e) {
      setErr(e instanceof Error ? e.message : 'enrolment failed')
    } finally { setBusy(false) }
  }

  return (
    <Modal title={row.enrolled ? `Add photos — ${row.name}` : `Enrol ${row.name}`} width={700}
           sub={row.enrolled
             ? `${row.profile?.images ?? 0} photos already held`
             : 'A new profile needs at least 5 usable photos'}
           onClose={onClose}>
      <div className="row" style={{ marginBottom: 14 }}>
        <button className={tab === 'capture' ? '' : 'ghost'} onClick={() => setTab('capture')}>
          <Ic k="camera" s={14} style={{ display: 'inline-block', verticalAlign: '-2px', marginRight: 6 }} />
          Guided capture
        </button>
        <button className={tab === 'upload' ? '' : 'ghost'} onClick={() => setTab('upload')}>
          <Ic k="upload" s={14} style={{ display: 'inline-block', verticalAlign: '-2px', marginRight: 6 }} />
          Upload photos
        </button>
      </div>

      {err && <div className="err" style={{ marginBottom: 12 }}>{err}</div>}

      {tab === 'capture' ? (
        <GuidedCapture employeeId={row.employee_id} onCancel={onClose} onDone={() => onClose()} />
      ) : (
        <div>
          {row.enrolled && (
            <label>
              <span>What to do with the photos already held</span>
              <select value={mode} onChange={e => setMode(e.target.value as 'add' | 'replace')}>
                <option value="add">Add to them — the profile combines old and new</option>
                <option value="replace">Replace them — start this profile again</option>
              </select>
            </label>
          )}
          <div className="muted" style={{ fontSize: 13, marginBottom: 12 }}>
            Front-on is not enough on its own. A profile built from several near-identical
            straight-on shots describes one viewing angle well and every other angle badly,
            so include some turned left, turned right, chin up and chin down.
          </div>
          <Btn kind="primary" icon="upload" disabled={busy}
               onClick={() => fileRef.current?.click()}>
            {busy ? 'Working…' : 'Choose photos'}
          </Btn>
          <input ref={fileRef} type="file" accept="image/*" multiple hidden
                 onChange={e => { const f = e.target.files; if (f?.length) upload(f) }} />

          {results && (
            <div style={{ marginTop: 16 }}>
              <SectionTitle>Result</SectionTitle>
              <table>
                <thead><tr><th>Photo</th><th>Outcome</th><th>Face</th><th>Yaw</th></tr></thead>
                <tbody>
                  {results.map((p, i) => (
                    <tr key={i}>
                      <td className="mono" style={{ fontSize: 12 }}>{p.filename}</td>
                      <td>
                        {p.accepted
                          ? <Chip kind={p.quality >= 80 ? 'ok' : 'warn'}>kept · {p.quality}%</Chip>
                          : <><Chip kind="idle">refused</Chip>{' '}
                              <span className="muted" style={{ fontSize: 12 }}>{p.reason}</span></>}
                      </td>
                      <td className="muted">{Math.round(p.face_px)} px</td>
                      <td className="muted">{p.yaw.toFixed(2)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
              {warnings.map((w, i) => (
                <div key={i} className="note" style={{ marginTop: 12 }}>{w.text}</div>
              ))}
            </div>
          )}
        </div>
      )}
    </Modal>
  )
}
