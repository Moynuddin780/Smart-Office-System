#!/usr/bin/env python3
"""Merge a face-embedding export from the VisionX system into this database.

WHAT THIS DOES NOT DO: it never deletes, never overwrites an existing vector,
and never takes ownership of a person enrolled here. It only ADDS image vectors
and, where a person is genuinely new, creates them. Everything it creates is
marked so it can be removed again exactly (see --remove).

WHY A MAPPING TABLE AND NOT NAME MATCHING: the same person is "Esheta" in one
system and "Eshita" in the other, "Shourav Das Gupta" and "Sourav Das Gupta",
"Sakiul Islam Ramim" and "Ramim". Matching those automatically is a guess, and a
wrong guess files one person's face under another person's attendance. The
mapping below was confirmed by the operator; anything not in it is treated as a
new person.

WHY SOME PEOPLE ARE REFUSED: the export marks four of its twelve unusable. Three
have images that disagree with each other (median cosine 0.32-0.40, where same
person normally sits at 0.64), so their averaged vector points at a place
between people and matches the wrong one. The fourth, Noshin, has no images at
all behind her vector, and that vector sits 0.45 from Esheta - inside the range
this export shows for the SAME person - so importing it risks breaking Esheta's
recognition to add somebody who has never been in front of these cameras.
"""
import argparse, base64, json, os, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TENANT = 5

# export uid -> emp_code of the SAME person already in this database.
# Confirmed by the operator. Absent = a new person.
SAME_PERSON = {
    "EMP-7506": "E2",    # Aslam Sikder      -> Aslam Sikder
    "EMP-9104": "E3",    # Shourav Das Gupta -> Sourav Das Gupta
    "EMP-9704": "E8",    # Esheta            -> Eshita
    "EMP-6751": "E10",   # Sadia             -> Sadia Haque
    "EMP-9603": "E7",    # Sakiul Islam Ramim-> Ramim
}


def psql(sql, want_rows=True):
    cmd = ["docker", "compose", "exec", "-T", "postgres", "psql",
           "-U", "smartoffice", "-d", "smartoffice", "-At", "-F", "|", "-c", sql]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or r.stdout.strip())
    out = r.stdout.strip()
    if not want_rows:
        return out
    return [l.split("|") for l in out.split("\n") if l.strip()]


def b64(vec):
    """A float32 vector in the exact layout the C++ side memcpy()s back out."""
    v = np.asarray(vec, dtype="<f4")
    n = np.linalg.norm(v)
    if not (0.99 < n < 1.01):
        raise ValueError(f"vector is not unit length: {n:.4f}")
    return base64.b64encode(v.tobytes()).decode()


def load_export(d):
    g = np.load(os.path.join(d, "face_gallery.npz"), allow_pickle=True)
    im = np.load(os.path.join(d, "face_gallery_images.npz"), allow_pickle=True)
    return g, im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export_dir")
    ap.add_argument("--apply", action="store_true", help="write; otherwise dry run")
    ap.add_argument("--remove", action="store_true", help="undo a previous import")
    args = ap.parse_args()

    if args.remove:
        n = psql("select count(*) from face_images where source='visionx'")[0][0]
        who = psql("select count(*) from employees where origin='visionx'")[0][0]
        print(f"  would remove {n} imported image vectors and {who} imported people")
        if args.apply:
            psql("delete from face_images where source='visionx'", False)
            psql("delete from face_profiles where employee_id in "
                 "(select id from employees where origin='visionx')", False)
            psql("delete from employees where origin='visionx'", False)
            print("  removed. Re-run the app to reload the gallery.")
        else:
            print("  dry run — add --apply")
        return 0

    g, im = load_export(args.export_dir)
    uids, names, E, usable = g["uids"], g["names"], g["embeddings"], g["usable"]

    existing = {c: (int(i), nm) for i, c, nm in psql(
        f"select id,emp_code,name from employees where tenant_id={TENANT}")}
    by_uid = {u: (int(i), c) for i, u, c in psql(
        f"select id,external_uid,emp_code from employees "
        f"where tenant_id={TENANT} and external_uid is not null")} if psql(
        "select count(*) from information_schema.columns "
        "where table_name='employees' and column_name='external_uid'")[0][0] != "0" else {}

    print(f"  export: {len(uids)} people, {len(im['uids'])} image vectors")
    print(f"  database: {len(existing)} employees\n")

    plan = []
    for i, (uid, nm, ok) in enumerate(zip(uids, names, usable)):
        uid, nm = str(uid), str(nm)
        vecs = im["embeddings"][im["uids"] == uid]
        if not bool(ok):
            print(f"  SKIP   {uid:<10} {nm:<24} export marks it unusable")
            continue
        code = SAME_PERSON.get(uid)
        if code and code in existing:
            eid, ename = existing[code]
            plan.append(("merge", uid, nm, eid, ename, vecs))
            print(f"  MERGE  {uid:<10} {nm:<24} -> {ename} (id {eid}, {code}): "
                  f"+{len(vecs)} vectors")
        elif uid in by_uid:
            eid = by_uid[uid][0]
            plan.append(("merge", uid, nm, eid, nm, vecs))
            print(f"  UPDATE {uid:<10} {nm:<24} -> already imported (id {eid}): "
                  f"+{len(vecs)} vectors")
        else:
            plan.append(("create", uid, nm, None, nm, vecs))
            print(f"  NEW    {uid:<10} {nm:<24} -> create, {len(vecs)} vectors")

    if not args.apply:
        print("\n  dry run — nothing written. Add --apply to commit.")
        return 0

    print()
    for kind, uid, nm, eid, label, vecs in plan:
        if kind == "create":
            code = "VX-" + uid.split("-")[-1]
            eid = int(psql(
                f"insert into employees(tenant_id,emp_code,name,active,external_uid,origin) "
                f"values({TENANT},'{code}',$${nm}$$,true,'{uid}','visionx') returning id")[0][0])
            print(f"  created employee {eid} ({code}) for {nm}")
        else:
            psql(f"update employees set external_uid='{uid}' where id={eid} "
                 f"and external_uid is null", False)

        # Replace only THIS import's rows for the person, so re-running is safe
        # and never touches a vector enrolled here.
        psql(f"delete from face_images where employee_id={eid} and source='visionx'", False)
        q = im["quality"][im["uids"] == uid]
        for k, v in enumerate(vecs):
            psql(f"insert into face_images(tenant_id,employee_id,embedding,dim,quality,"
                 f"yaw,face_px,det_score,image_path,filename,size_bytes,source) values("
                 f"{TENANT},{eid},decode('{b64(v)}','base64'),512,{int(q[k])},"
                 f"0,0,0,'','{uid}-{k:03d}',0,'visionx')", False)
        print(f"  {label}: {len(vecs)} vectors stored")

    print("\n  done. Profiles are rebuilt by the app on the next gallery reload.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
