#!/usr/bin/env python3
"""
Render a GTA SA skinned ped DFF (+TXD) posed with a real SA animation, without launching the game.

    python3 tools/preview_ped.py models/male01/male01.dff --ifp "<GTA SA>/anim/ped.ifp" --anim walk_civi -o preview.png

Draws front and side views per pose and prints a skinning-quality metric (how much mesh edges stretch
in the pose versus the bind pose; good skinning stays in the low single-digit percent range).
Use --atomics head_000_r,uppr_000_r,... to choose the visible drawables (default: drawable 000 of each
component, like a default-variation ped) and --weights to colour by dominant bone instead of texture.

Requires numpy and Pillow.
"""

import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

# ----------------------------------------------------------------------------------------------
# RenderWare readers (independent of the converter's writer on purpose)
# ----------------------------------------------------------------------------------------------


def _chunks(b, off, end):
    while off + 12 <= end:
        t, s, _v = struct.unpack_from("<3I", b, off)
        yield t, off + 12, s
        off += 12 + s


def _find(b, off, end, t):
    for ct, co, cs in _chunks(b, off, end):
        if ct == t:
            return co, cs
    return None


def read_dff(path):
    b = open(path, "rb").read()
    _, co, cs = next(_chunks(b, 0, len(b)))
    clump_end = co + cs
    frames, geoms, atomics = [], [], []
    for t, o, s in _chunks(b, co, clump_end):
        if t == 0x0E:
            so, ss = _find(b, o, o + s, 0x01)
            n = struct.unpack_from("<I", b, so)[0]
            for i in range(n):
                v = struct.unpack_from("<12fiI", b, so + 4 + i * 56)
                m = np.eye(4)
                m[0, :3], m[1, :3], m[2, :3], m[3, :3] = v[0:3], v[3:6], v[6:9], v[9:12]
                frames.append({"local": m, "parent": v[12], "name": "", "bone_id": None, "hanim": None})
            fi = 0
            for et, eo, es in _chunks(b, so + ss, o + s):
                if et != 0x03:
                    continue
                for pt, po, ps in _chunks(b, eo, eo + es):
                    if pt == 0x253F2FE:
                        frames[fi]["name"] = b[po:po + ps].decode("ascii", "replace").strip()
                    elif pt == 0x11E:
                        _ver, bid, nb = struct.unpack_from("<3I", b, po)
                        frames[fi]["bone_id"] = bid
                        if nb:
                            frames[fi]["hanim"] = [struct.unpack_from("<3I", b, po + 20 + k * 12)[:2] for k in range(nb)]
                fi += 1
        elif t == 0x1A:
            for gt, go, gs in _chunks(b, o, o + s):
                if gt == 0x0F:
                    geoms.append(_read_geometry(b, go, gs))
        elif t == 0x14:
            so, _ = _find(b, o, o + s, 0x01)
            fidx, gidx = struct.unpack_from("<2I", b, so)
            atomics.append((fidx, gidx))
    return frames, geoms, atomics


def _read_geometry(b, o, s):
    so, ss = _find(b, o, o + s, 0x01)
    flags, nt, nv, _nm = struct.unpack_from("<4I", b, so)
    p = so + 16
    ntex = (flags >> 16) & 0xFF or (1 if flags & 0x04 else 0)
    if flags & 0x08:
        p += nv * 4
    uvs = np.frombuffer(b, "<f4", nv * 2, p).reshape(nv, 2) if ntex else np.zeros((nv, 2))
    p += nv * 8 * ntex
    raw = np.frombuffer(b, "<u2", nt * 4, p).reshape(nt, 4)
    tris = raw[:, [1, 0, 3]].astype(np.int64)
    tri_mat = raw[:, 2].astype(np.int64)
    p += nt * 8 + 24
    verts = np.frombuffer(b, "<f4", nv * 3, p).reshape(nv, 3).astype(np.float64)
    p += nv * 12
    normals = np.frombuffer(b, "<f4", nv * 3, p).reshape(nv, 3).astype(np.float64) if flags & 0x10 else None
    mats = []
    ml = _find(b, so + ss, o + s, 0x08)
    for mt, mo, ms in _chunks(b, ml[0], ml[0] + ml[1]):
        if mt != 0x07:
            continue
        name = None
        tx = _find(b, mo, mo + ms, 0x06)
        if tx:
            st = [c for c in _chunks(b, tx[0], tx[0] + tx[1]) if c[0] == 0x02]
            name = b[st[0][1]:st[0][1] + st[0][2]].split(b"\0")[0].decode().lower()
        mats.append(name)
    skin = None
    ext = [c for c in _chunks(b, so + ss, o + s) if c[0] == 0x03]
    if ext:
        sk = _find(b, ext[0][1], ext[0][1] + ext[0][2], 0x116)
        if sk:
            q = sk[0]
            nb, nused, _maxw, _ = struct.unpack_from("<4B", b, q)
            q += 4 + nused
            idx = np.frombuffer(b, np.uint8, nv * 4, q).reshape(nv, 4).astype(np.int64)
            q += nv * 4
            w = np.frombuffer(b, "<f4", nv * 4, q).reshape(nv, 4).astype(np.float64)
            q += nv * 16
            ibm = []
            for _ in range(nb):
                if nused == 0:
                    q += 4
                ibm.append(np.frombuffer(b, "<f4", 16, q).reshape(4, 4).astype(np.float64))
                q += 64
            skin = (idx, w, ibm)
    return {"uvs": uvs, "tris": tris, "tri_mat": tri_mat, "verts": verts, "normals": normals, "mats": mats, "skin": skin}


def _decode_dxt_color(blk, w, h, mode4):
    c0 = blk[:, 0].astype(np.uint32) | (blk[:, 1].astype(np.uint32) << 8)
    c1 = blk[:, 2].astype(np.uint32) | (blk[:, 3].astype(np.uint32) << 8)

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], 1).astype(np.int32)

    p0, p1 = rgb(c0), rgb(c1)
    four = (c0 > c1) | mode4
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)
    alpha = np.full((len(blk), 4), 255)
    alpha[:, 3] = np.where(four, 255, 0)
    bits = blk[:, 4].astype(np.uint32) | (blk[:, 5].astype(np.uint32) << 8) | (blk[:, 6].astype(np.uint32) << 16) | (blk[:, 7].astype(np.uint32) << 24)
    codes = np.stack([(bits >> (2 * k)) & 3 for k in range(16)], 1).astype(np.int64)
    rgbpx = np.take_along_axis(pal, codes[:, :, None], 1)
    apx = np.take_along_axis(alpha, codes, 1)
    return rgbpx, apx


def _blocks_to_image(px, w, h, channels):
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    img = px.reshape(bh, bw, 4, 4, channels).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, channels)
    return img[:h, :w]


def read_txd(path):
    b = open(path, "rb").read()
    out = {}
    _, co, cs = next(_chunks(b, 0, len(b)))
    for t, o, s in _chunks(b, co, co + cs):
        if t != 0x15:
            continue
        so, _ = _find(b, o, o + s, 0x01)
        (_plat, _filt, _addr, _pad, name, _mask, _raster, d3d, w, h, _depth, _levels, _rt, _flags) = struct.unpack_from("<IBBH32s32sIIHHBBBB", b, so)
        name = name.split(b"\0")[0].decode().lower()
        size = struct.unpack_from("<I", b, so + 88)[0]
        data = np.frombuffer(b, np.uint8, size, so + 92)
        fourcc = struct.pack("<I", d3d)
        if fourcc == b"DXT1":
            rgb, a = _decode_dxt_color(data.reshape(-1, 8), w, h, np.zeros(len(data) // 8, bool))
        elif fourcc in (b"DXT3", b"DXT5"):
            blk = data.reshape(-1, 16)
            rgb, _ = _decode_dxt_color(blk[:, 8:], w, h, np.ones(len(blk), bool))
            a = np.full((len(blk), 16), 255)
        else:
            px = data.reshape(h, w, 4)
            out[name] = np.dstack([px[:, :, 2], px[:, :, 1], px[:, :, 0], px[:, :, 3]]).astype(np.uint8)
            continue
        img = np.concatenate([rgb, a[:, :, None]], 2)
        out[name] = _blocks_to_image(img, w, h, 4).astype(np.uint8)
    return out


def load_ifp(path):
    b = open(path, "rb").read()
    if b[:4] != b"ANP3":
        raise SystemExit("only ANP3 (GTA SA) .ifp files are supported")
    n = struct.unpack_from("<I", b, 32)[0]
    off, anims = 36, {}
    for _ in range(n):
        name = b[off:off + 24].split(b"\0")[0].decode("latin-1")
        nobj = struct.unpack_from("<I", b, off + 24)[0]
        off += 36
        objs = {}
        for _ in range(nobj):
            ftype, nf, bid = struct.unpack_from("<3I", b, off + 24)
            off += 36
            fs = 16 if ftype == 4 else 10
            fr = np.frombuffer(b, "<i2", nf * fs // 2, off).reshape(nf, fs // 2).astype(np.float64)
            off += nf * fs
            objs[bid] = {"q": fr[:, 0:4] / 4096.0, "t": fr[:, 4] / 60.0, "tr": fr[:, 5:8] / 1024.0 if ftype == 4 else None}
        anims[name.lower()] = objs
    return anims


# ----------------------------------------------------------------------------------------------
# Posing
# ----------------------------------------------------------------------------------------------


def quat_rows(x, y, z, w):
    """Quaternion as a RenderWare row-vector rotation matrix."""
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]).T


def sample(obj, t):
    times = obj["t"]
    k = int(np.searchsorted(times, t, side="right") - 1)
    k = max(0, min(k, len(times) - 1))
    k2 = min(k + 1, len(times) - 1)
    f = 0.0 if k2 == k or times[k2] == times[k] else (t - times[k]) / (times[k2] - times[k])
    q0, q1 = obj["q"][k], obj["q"][k2]
    if np.dot(q0, q1) < 0:
        q1 = -q1
    q = q0 * (1 - f) + q1 * f
    q /= np.linalg.norm(q)
    tr = None
    if obj["tr"] is not None:
        tr = obj["tr"][k] * (1 - f) + obj["tr"][k2] * f
    return q, tr


def bone_matrices(frames, anim, t):
    """Per HAnim index: world matrix of the bone in clump space (None anim = bind pose from frames)."""
    root = next(i for i, f in enumerate(frames) if f["hanim"])
    order = frames[root]["hanim"]
    world = {}
    for i, f in enumerate(frames):
        local = f["local"].copy()
        if anim is not None and f["bone_id"] is not None and f["bone_id"] in anim:
            q, tr = sample(anim[f["bone_id"]], t)
            local[:3, :3] = quat_rows(*q)
            if tr is not None:
                local[3, :3] = tr
        parent = f["parent"]
        world[i] = local if parent < 0 else local @ world[parent]
    id_to_frame = {f["bone_id"]: i for i, f in enumerate(frames) if f["bone_id"] is not None}
    return [world[id_to_frame[bid]] for bid, _ in order]


def skin_vertices(g, mats):
    idx, w, ibm = g["skin"]
    vh = np.hstack([g["verts"], np.ones((len(g["verts"]), 1))])
    out = np.zeros((len(vh), 3))
    for k in range(4):
        for bone in np.unique(idx[:, k]):
            sel = idx[:, k] == bone
            if not (w[sel, k] > 0).any():
                continue
            m = ibm[bone] @ mats[bone]
            out[sel] += w[sel, k:k + 1] * (vh[sel] @ m)[:, :3]
    return out


# ----------------------------------------------------------------------------------------------
# Rasterizer
# ----------------------------------------------------------------------------------------------

BONE_COLORS = np.array([[int(c[i:i + 2], 16) for i in (0, 2, 4)] for c in (
    "e6194b 3cb44b ffe119 4363d8 f58231 911eb4 46f0f0 f032e6 bcf60c fabebe 008080 e6beff 9a6324 fffac8 800000 aaffc3 "
    "808000 ffd8b1 000075 808080 ffffff 000000 e6194b 3cb44b ffe119 4363d8 f58231 911eb4 46f0f0 f032e6 bcf60c fabebe").split()])


def render(meshes, view, size, lightdir):
    """meshes: list of (positions(n,3) view space x=right y=up z=towards viewer, tris, colors per tri or uv+tex)."""
    allp = np.concatenate([m["p"] for m in meshes])
    lo, hi = allp[:, :2].min(0), allp[:, :2].max(0)
    scale = (size - 20) / max(hi - lo)
    img = np.full((size, size, 3), 40, np.uint8)
    zbuf = np.full((size, size), -np.inf)
    for m in meshes:
        p = m["p"]
        sx = (p[:, 0] - lo[0]) * scale + 10 + (size - 20 - (hi[0] - lo[0]) * scale) / 2
        sy = size - ((p[:, 1] - lo[1]) * scale + 10)
        tex, uv = m.get("tex"), m.get("uv")
        for ti, (a, b_, c) in enumerate(m["tris"]):
            x0, y0, x1, y1, x2, y2 = sx[a], sy[a], sx[b_], sy[b_], sx[c], sy[c]
            minx, maxx = int(max(0, np.floor(min(x0, x1, x2)))), int(min(size - 1, np.ceil(max(x0, x1, x2))))
            miny, maxy = int(max(0, np.floor(min(y0, y1, y2)))), int(min(size - 1, np.ceil(max(y0, y1, y2))))
            if minx > maxx or miny > maxy:
                continue
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-12:
                continue
            gx, gy = np.meshgrid(np.arange(minx, maxx + 1) + 0.5, np.arange(miny, maxy + 1) + 0.5)
            l0 = ((y1 - y2) * (gx - x2) + (x2 - x1) * (gy - y2)) / den
            l1 = ((y2 - y0) * (gx - x2) + (x0 - x2) * (gy - y2)) / den
            l2 = 1 - l0 - l1
            inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
            if not inside.any():
                continue
            z = l0 * p[a, 2] + l1 * p[b_, 2] + l2 * p[c, 2]
            zs = zbuf[miny:maxy + 1, minx:maxx + 1]
            vis = inside & (z > zs)
            if not vis.any():
                continue
            n = np.cross(p[b_] - p[a], p[c] - p[a])
            shade = 0.35 + 0.65 * abs(np.dot(n / (np.linalg.norm(n) + 1e-12), lightdir))
            if tex is not None:
                u = l0 * uv[a, 0] + l1 * uv[b_, 0] + l2 * uv[c, 0]
                v = l0 * uv[a, 1] + l1 * uv[b_, 1] + l2 * uv[c, 1]
                th, tw = tex.shape[:2]
                tx = (np.mod(u, 1.0) * tw).astype(int) % tw
                ty = (np.mod(v, 1.0) * th).astype(int) % th
                texel = tex[ty, tx]
                keep = vis & (texel[:, :, 3] > 100)
                col = texel[:, :, :3] * shade
            else:
                keep = vis
                col = np.broadcast_to(m["colors"][ti] * shade, (*vis.shape, 3))
            zs[keep] = z[keep]
            region = img[miny:maxy + 1, minx:maxx + 1]
            region[keep] = np.clip(col[keep], 0, 255).astype(np.uint8)
    return img


def triangle_strain(rest, posed, tris):
    worst = np.zeros(len(tris))
    for i, j in ((0, 1), (1, 2), (2, 0)):
        l0 = np.linalg.norm(rest[tris[:, i]] - rest[tris[:, j]], axis=1)
        l1 = np.linalg.norm(posed[tris[:, i]] - posed[tris[:, j]], axis=1)
        worst = np.maximum(worst, np.abs(l1 / np.maximum(l0, 1e-6) - 1.0))
    return worst


def edge_strain(rest, posed, tris):
    e = np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]])
    l0 = np.linalg.norm(rest[e[:, 0]] - rest[e[:, 1]], axis=1)
    l1 = np.linalg.norm(posed[e[:, 0]] - posed[e[:, 1]], axis=1)
    ok = l0 > 1e-5
    s = np.abs(l1[ok] / l0[ok] - 1.0)
    return float(s.mean()), float(np.percentile(s, 99))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dff")
    ap.add_argument("--txd", help="default: next to the DFF")
    ap.add_argument("--ifp", help="GTA SA anim/ped.ifp")
    ap.add_argument("--anim", default="walk_civi", help="comma separated animation names")
    ap.add_argument("--time", type=float, default=None, help="time in seconds (default: middle of the anim)")
    ap.add_argument("--atomics", help="comma separated drawables to show (default: *_000_* of each component)")
    ap.add_argument("--weights", action="store_true", help="colour by dominant bone instead of texture")
    ap.add_argument("--strain", action="store_true", help="colour by how much each triangle stretches in the pose (blue=0, red>=50%%)")
    ap.add_argument("--size", type=int, default=520)
    ap.add_argument("-o", "--output", default="preview.png")
    args = ap.parse_args()

    frames, geoms, atomics = read_dff(args.dff)
    txd_path = args.txd or os.path.splitext(args.dff)[0] + ".txd"
    textures = read_txd(txd_path) if os.path.exists(txd_path) and not (args.weights or args.strain) else {}
    names = [frames[f]["name"] for f, _ in atomics]
    if args.atomics:
        want = set(args.atomics.split(","))
    else:
        seen, want = set(), set()
        for n in sorted(names):
            comp = n.split("_")[0]
            if comp not in seen:
                seen.add(comp)
                want.add(n)
    chosen = [(n, geoms[g]) for (f, g), n in zip(atomics, names) if n in want]
    print("drawables:", ", ".join(n for n, _ in chosen))

    poses = [("bind", None, 0.0)]
    if args.ifp:
        ifp = load_ifp(args.ifp)
        for an in args.anim.split(","):
            anim = ifp[an.lower()]
            tmax = max(o["t"][-1] for o in anim.values())
            poses.append((an, anim, tmax * 0.5 if args.time is None else args.time))

    panels = []
    for label, anim, t in poses:
        mats = bone_matrices(frames, anim, t)
        meshes_world = []
        strains = []
        for n, g in chosen:
            posed = skin_vertices(g, mats)
            rest = skin_vertices(g, bone_matrices(frames, None, 0.0))
            strains.append((n,) + edge_strain(rest, posed, g["tris"]))
            meshes_world.append((n, g, posed, rest))
        print(f"[{label} t={t:.2f}] edge strain mean / p99 per drawable:")
        for n, mean, p99 in strains:
            print(f"   {n:12s} {mean * 100:5.1f}% / {p99 * 100:5.1f}%")
        # SA world: +Z up. Front view looks along the ped's facing direction.
        for view in ("front", "side"):
            meshes = []
            for n, g, posed, rest in meshes_world:
                if view == "front":
                    vp = np.stack([-posed[:, 0], posed[:, 2], -posed[:, 1]], 1)
                else:
                    vp = np.stack([posed[:, 1], posed[:, 2], -posed[:, 0]], 1)
                m = {"p": vp, "tris": g["tris"]}
                if args.strain:
                    k = np.clip(triangle_strain(rest, posed, g["tris"]) / 0.5, 0, 1)[:, None]
                    m["colors"] = (1 - k) * np.array([60.0, 110, 255]) + k * np.array([255.0, 40, 40])
                    meshes.append(m)
                elif args.weights or not textures:
                    idx, w, _ = g["skin"]
                    dom = idx[np.arange(len(idx)), np.argmax(w, 1)]
                    m["colors"] = BONE_COLORS[dom[g["tris"][:, 0]] % len(BONE_COLORS)].astype(np.float64)
                    meshes.append(m)
                else:
                    for mi, tn in enumerate(g["mats"]):
                        sel = g["tri_mat"] == mi
                        mm = dict(m, tris=g["tris"][sel], uv=g["uvs"])
                        if tn in textures:
                            mm["tex"] = textures[tn]
                        else:
                            print(f"   (missing texture {tn} on {n})")
                            mm["colors"] = np.full((sel.sum(), 3), 255.0)
                        meshes.append(mm)
            panels.append((f"{label} {view}", render(meshes, view, args.size, np.array([0.3, 0.5, 0.8]))))

    out = np.concatenate([p for _, p in panels], axis=1)
    Image.fromarray(out).save(args.output)
    print("wrote", args.output, "panels:", ", ".join(l for l, _ in panels))


if __name__ == "__main__":
    sys.exit(main())
