#!/usr/bin/env python3
"""
GTA V ped -> GTA San Andreas ped converter (PedFuncs Extended)

Takes a GTA V ped exported with CodeWalker ("Export XML" on the .ydd / .yft / .ymt / .ytd)
and produces a GTA SA DFF + TXD + variation XML that PedFuncs Extended loads.

    python3 tools/gtav_to_sa.py "<folder with a_f_y_xxx.ydd.xml etc>" --name male01 --out <dir>

What it does differently from a naive conversion:

  * Skeleton refit: the SA skeleton is re-jointed to the GTA V ped's own joint positions and limb
    directions (keeping SA's bone axes / twist conventions), and fresh inverse bind matrices are
    computed. SA animations only rotate bones, so elbows/knees/shoulders now bend exactly where the
    GTA V mesh expects them to.
  * Weights: every GTA V bone is resolved to SA bones (helper bones like MH_*_Knee / MH_*_Elbow are
    split across both limb segments, unknown bones fall back along their parent chain instead of
    snapping to the pelvis). Cloth meshes (whose blend data is simulation data) are rigged from the
    garment's own artist-skinned lower LOD (else the nearest body surface), keep the real skinning
    their waistband has, and are smoothed over the surface so the legs don't tear them apart.
  * Only the High LOD geometry is exported (lower LODs are only read as the rig of cloth pieces) (merging Medium/Low LODs on top of it is what caused the
    "broken UV" z-fighting look before).
  * Vertex buffers are decoded through their declared layout, not fixed token offsets.
  * Textures keep wrap addressing on both U and V, trilinear filtering, real mip flags, and alpha is
    only kept where a transparent GTA V shader actually needs it (opaque DXT5 is losslessly
    transcoded to DXT1 so SA does not push skin/clothes into its alpha-sorted render pass).

Requires only Python 3.9+ and numpy. Binary .ydd/.yft files are not read directly; export them to
XML in CodeWalker (right click -> Export XML) first.
"""

import argparse
import json
import logging
import os
import re
import shutil
import struct
import sys
import time
import xml.etree.ElementTree as ET

import numpy as np

log = logging.getLogger("gtav_to_sa")

RW_VERSION = 0x1803FFFF  # RenderWare 3.6.0.3 (GTA SA)
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SKELETON = os.path.join(TOOLS_DIR, "data", "sa_ped_skeleton.json")

COMPONENT_PREFIXES = ("head", "berd", "hair", "uppr", "lowr", "hand", "feet", "teef", "accs", "task", "decl", "jbib")

# GTA V position (X right->left side, -Y forward, Z up) -> SA skin mesh space (X up, +Y forward, +Z left)
V2SA = np.array([[0.0, 0.0, 1.0],
                 [0.0, -1.0, 0.0],
                 [1.0, 0.0, 0.0]])

# ----------------------------------------------------------------------------------------------
# Bone mapping
# ----------------------------------------------------------------------------------------------

# GTA V bone -> [(SA bone, share)]. Bones not listed here resolve through their parent chain.
GTAV_TO_SA = {
    "SKEL_ROOT": [("Pelvis", 1.0)],
    "IK_Root": [("Pelvis", 1.0)],
    "SKEL_Pelvis": [("Pelvis", 1.0)],
    "SKEL_Pelvis1": [("Pelvis", 1.0)],
    "SKEL_PelvisRoot": [("Pelvis", 1.0)],

    "SKEL_Spine_Root": [("Spine", 1.0)],
    "SKEL_Spine0": [("Spine", 1.0)],
    "SKEL_Spine1": [("Spine", 1.0)],
    # Spine2 sits below SA's Spine1 joint; blending keeps the mid-torso from folding.
    "SKEL_Spine2": [("Spine", 0.6), ("Spine1", 0.4)],
    "SKEL_Spine3": [("Spine1", 1.0)],

    "SKEL_Neck_1": [("Neck", 1.0)],
    "SKEL_Neck_2": [("Neck", 1.0)],
    "RB_Neck_1": [("Neck", 1.0)],
    "SKEL_Head": [("Head", 1.0)],

    # Face: anything under these follows them through the parent chain.
    "FB_Jaw_001": [("Jaw", 1.0)],
    "FACIAL_jaw": [("Jaw", 1.0)],
    "FB_L_Lip_Corner_001": [("Head", 0.5), ("Jaw", 0.5)],
    "FB_R_Lip_Corner_001": [("Head", 0.5), ("Jaw", 0.5)],
    "FACIAL_L_lipCorner": [("Head", 0.5), ("Jaw", 0.5)],
    "FACIAL_R_lipCorner": [("Head", 0.5), ("Jaw", 0.5)],
    "FB_L_Brow_Out_001": [("L Brow", 1.0)],
    "FB_R_Brow_Out_001": [("R Brow", 1.0)],

    "SPR_L_Breast": [("L breast", 1.0)],
    "SPR_R_Breast": [("R breast", 1.0)],
    "SKEL_L_Breast": [("L breast", 1.0)],
    "SKEL_R_Breast": [("R breast", 1.0)],
}

for side in ("L", "R"):
    GTAV_TO_SA.update({
        f"SKEL_{side}_Clavicle": [(f"Bip01 {side} Clavicle", 1.0)],
        f"SKEL_{side}_UpperArm": [(f"{side} UpperArm", 1.0)],
        # Roll bones only partly follow their limb in GTA V; these shares measured lowest stretch over SA anims.
        f"RB_{side}_ArmRoll": [(f"Bip01 {side} Clavicle", 0.3), (f"{side} UpperArm", 0.7)],
        # Elbow/knee helpers are driven halfway between both segments in GTA V.
        f"MH_{side}_Elbow": [(f"{side} UpperArm", 0.5), (f"{side} ForeArm", 0.5)],
        f"SKEL_{side}_Forearm": [(f"{side} ForeArm", 1.0)],
        f"RB_{side}_ForeArmRoll": [(f"{side} ForeArm", 1.0)],
        f"SKEL_{side}_Hand": [(f"{side} Hand", 1.0)],
        # SA has a single two-segment "mitten" finger; the thumb stays with the hand.
        f"SKEL_{side}_Finger00": [(f"{side} Hand", 1.0)],
        f"SKEL_{side}_Finger01": [(f"{side} Hand", 1.0)],
        f"SKEL_{side}_Finger02": [(f"{side} Hand", 1.0)],
        f"SKEL_{side}_Thigh": [(f"{side} Thigh", 1.0)],
        f"RB_{side}_ThighRoll": [("Pelvis", 0.3), (f"{side} Thigh", 0.7)],
        f"MH_{side}_Knee": [(f"{side} Thigh", 0.5), (f"{side} Calf", 0.5)],
        f"SKEL_{side}_Calf": [(f"{side} Calf", 1.0)],
        f"SKEL_{side}_Foot": [(f"{side} Foot", 1.0)],
        f"SKEL_{side}_Toe0": [(f"{side} Toe0", 1.0)],
        f"SKEL_{side}_Toe1": [(f"{side} Toe0", 1.0)],
    })
    for f in range(1, 5):
        GTAV_TO_SA[f"SKEL_{side}_Finger{f}0"] = [(f"{side} Finger", 1.0)]
        GTAV_TO_SA[f"SKEL_{side}_Finger{f}1"] = [(f"{side} Finger01", 1.0)]
        GTAV_TO_SA[f"SKEL_{side}_Finger{f}2"] = [(f"{side} Finger01", 1.0)]

# SA bone -> GTA V bones whose (averaged) joint position the SA joint is moved to.
SA_JOINT_ANCHORS = {
    "Pelvis": [["SKEL_Pelvis"]],
    "Spine": [["SKEL_Spine0"], ["SKEL_Spine_Root"]],
    "Spine1": [["SKEL_Spine3"], ["SKEL_Spine2"]],
    "Neck": [["SKEL_Neck_1"]],
    "Head": [["SKEL_Head"]],
    "Jaw": [["FB_Jaw_001"], ["FACIAL_jaw"]],
    "L Brow": [["FB_L_Brow_Out_001"]],
    "R Brow": [["FB_R_Brow_Out_001"]],
    "L breast": [["SPR_L_Breast"], ["SKEL_L_Breast"]],
    "R breast": [["SPR_R_Breast"], ["SKEL_R_Breast"]],
}
for side in ("L", "R"):
    SA_JOINT_ANCHORS.update({
        f"Bip01 {side} Clavicle": [[f"SKEL_{side}_Clavicle"]],
        f"{side} UpperArm": [[f"SKEL_{side}_UpperArm"]],
        f"{side} ForeArm": [[f"SKEL_{side}_Forearm"]],
        f"{side} Hand": [[f"SKEL_{side}_Hand"]],
        f"{side} Finger": [[f"SKEL_{side}_Finger{f}0" for f in range(1, 5)]],
        f"{side} Finger01": [[f"SKEL_{side}_Finger{f}1" for f in range(1, 5)]],
        f"{side} Thigh": [[f"SKEL_{side}_Thigh"]],
        f"{side} Calf": [[f"SKEL_{side}_Calf"]],
        f"{side} Foot": [[f"SKEL_{side}_Foot"]],
        f"{side} Toe0": [[f"SKEL_{side}_Toe0"]],
    })

# Bones whose child joint lies on their local +X axis in SA. These get re-aimed at the GTA V child
# joint so animations rotate the limb around the right axis with the right length.
SA_AIM_CHILD = {
    "Spine": "Spine1", "Spine1": "Neck", "Neck": "Head",
}
for side in ("L", "R"):
    SA_AIM_CHILD.update({
        f"Bip01 {side} Clavicle": f"{side} UpperArm",
        f"{side} UpperArm": f"{side} ForeArm",
        f"{side} ForeArm": f"{side} Hand",
        f"{side} Hand": f"{side} Finger",
        f"{side} Finger": f"{side} Finger01",
        f"{side} Thigh": f"{side} Calf",
        f"{side} Calf": f"{side} Foot",
    })
# Leaf bones aimed at a GTA V point instead of an SA child joint.
SA_AIM_POINT = {f"{s} Finger01": [f"SKEL_{s}_Finger{f}2" for f in range(1, 5)] for s in ("L", "R")}

# Shaders whose diffuse alpha is real transparency in GTA V.
ALPHA_SHADER_HINTS = ("alpha", "hair", "decal", "cutout", "fur")


# ----------------------------------------------------------------------------------------------
# Small math helpers
# ----------------------------------------------------------------------------------------------

def quat_to_mat(x, y, z, w):
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def rotation_between(a, b):
    """Smallest rotation (column-vector matrix) taking unit vector a onto unit vector b."""
    a = a / np.linalg.norm(a)
    b = b / np.linalg.norm(b)
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if c > 1.0 - 1e-9:
        return np.eye(3)
    if c < -1.0 + 1e-9:
        axis = np.cross(a, [1.0, 0.0, 0.0])
        if np.linalg.norm(axis) < 1e-6:
            axis = np.cross(a, [0.0, 1.0, 0.0])
        axis /= np.linalg.norm(axis)
        return 2.0 * np.outer(axis, axis) - np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1.0 / (1.0 + c))


def orthonormalize(rows):
    u, _, vt = np.linalg.svd(rows)
    r = u @ vt
    if np.linalg.det(r) < 0:
        u[:, -1] *= -1
        r = u @ vt
    return r


# ----------------------------------------------------------------------------------------------
# GTA V (CodeWalker XML) parsing
# ----------------------------------------------------------------------------------------------

# Float counts of CodeWalker vertex layout components.
LAYOUT_SIZES = {
    "Position": 3, "Position2": 3, "Normal": 3, "Normal2": 3,
    "BlendWeights": 4, "BlendIndices": 4,
    "Colour0": 4, "Colour1": 4, "Colour2": 4,
    "Tangent": 4, "Binormal": 4, "Tangent2": 4,
}
for _i in range(8):
    LAYOUT_SIZES[f"TexCoord{_i}"] = 2


class GtaSkeleton:
    def __init__(self, bones_xml):
        self.names, self.parents, self.world = [], [], []
        for b in bones_xml:
            t = b.find("Translation").attrib
            q = b.find("Rotation").attrib
            s = b.find("Scale").attrib if b.find("Scale") is not None else {"x": 1, "y": 1, "z": 1}
            m = np.eye(4)
            m[:3, :3] = quat_to_mat(*(float(q[k]) for k in "xyzw")) @ np.diag([float(s[k]) for k in "xyz"])
            m[:3, 3] = [float(t[k]) for k in "xyz"]
            parent = int(b.find("ParentIndex").attrib["value"])
            self.names.append(b.findtext("Name"))
            self.parents.append(parent)
            self.world.append(m if parent < 0 else self.world[parent] @ m)
        self.index = {n: i for i, n in enumerate(self.names)}

    def joint(self, name):
        i = self.index.get(name)
        return None if i is None else self.world[i][:3, 3]

    def joint_mesh(self, names):
        pts = [self.joint(n) for n in names]
        pts = [p for p in pts if p is not None]
        if len(pts) != len(names):
            return None
        return V2SA @ np.mean(pts, axis=0)


class GtaGeometry:
    def __init__(self, shader_index, positions, normals, uvs, weights, indices, triangles, cloth_like):
        self.shader_index = shader_index
        self.positions = positions      # (n,3) GTA V model space
        self.normals = normals          # (n,3)
        self.uvs = uvs                  # (n,2)
        self.weights = weights          # (n,4) 0..1 or None
        self.indices = indices          # (n,4) skeleton bone indices or None
        self.triangles = triangles      # (m,3)
        self.cloth_like = cloth_like    # blend data is not usable skinning
        self.valid = None               # (n,) bool: vertices whose own blend data is real skinning


class GtaDrawable:
    def __init__(self, name):
        self.name = name
        self.shaders = []      # [(shader name, diffuse texture name)]
        self.geometries = []
        self.lod_reference = []  # artist-skinned lower-LOD geometries of a cloth drawable (rigid fallback)


def _vertex_data_text(vb):
    for tag in ("Data", "Data1", "Data2"):
        node = vb.find(tag)
        if node is not None and node.text and node.text.strip():
            return node.text
    return ""


def parse_drawable(node, skeleton, name_override=None):
    name = name_override or (node.findtext("Name") or "unnamed").strip()
    if name.startswith("pack:/"):
        name = name[6:]
    d = GtaDrawable(name.lower())

    for sh in node.findall("ShaderGroup/Shaders/Item"):
        diffuse = None
        for p in sh.findall("Parameters/Item"):
            if p.get("name") == "DiffuseSampler":
                diffuse = (p.findtext("Name") or "").strip() or None
        d.shaders.append(((sh.findtext("Name") or "").strip(), diffuse))

    models = node.findall("DrawableModelsHigh/Item")
    if not models:
        log.warning("  %s: no High LOD models, skipping", d.name)
        return d

    d.geometries = _parse_models(d, models, skeleton)
    if any(g.cloth_like for g in d.geometries):
        # GTA V drives the High LOD of a cloth drawable by its cloth simulation, but the lower LODs of
        # the same garment are ordinary meshes skinned by the artist: that is Rockstar's own rigid
        # version of it, and the best reference for SA (which has no cloth simulation).
        for lod in ("Medium", "Low"):
            ref = [g for g in _parse_models(d, node.findall(f"DrawableModels{lod}/Item"), skeleton, quiet=True)
                   if not g.cloth_like]
            if ref:
                d.lod_reference = ref
                log.info("  %s: cloth drawable, rigid skinning taken from its %s LOD (%d vertices)",
                         d.name, lod, sum(len(g.positions) for g in ref))
                break
    return d


def _parse_models(d, models, skeleton, quiet=False):
    geometries = []
    for model in models:
        has_skin = (model.find("HasSkin") is not None and model.find("HasSkin").get("value") not in ("0", "false"))
        model_bone = int(model.find("BoneIndex").get("value")) if model.find("BoneIndex") is not None else 0
        for g in model.findall("Geometries/Item"):
            vb = g.find("VertexBuffer")
            layout = [c.tag for c in vb.find("Layout")]
            text = _vertex_data_text(vb)
            lines = text.strip().splitlines()
            if not lines:
                continue
            width = len(lines[0].split())
            offsets, off = {}, 0
            for comp in layout:
                if comp not in LAYOUT_SIZES:
                    raise ValueError(f"{d.name}: unknown vertex component '{comp}' in layout {layout}")
                offsets[comp] = off
                off += LAYOUT_SIZES[comp]
            if off != width:
                raise ValueError(f"{d.name}: layout {layout} expects {off} values per vertex, data has {width}")
            data = np.array(text.split(), dtype=np.float64).reshape(-1, width)

            def comp(cname):
                o = offsets[cname]
                return data[:, o:o + LAYOUT_SIZES[cname]]

            pos = comp("Position").copy()
            nrm = comp("Normal").copy() if "Normal" in offsets else np.zeros_like(pos)
            uv = comp("TexCoord0").copy() if "TexCoord0" in offsets else np.zeros((len(pos), 2))
            tris = np.array(g.findtext("IndexBuffer/Data").split(), dtype=np.int64).reshape(-1, 3)
            shader_index = int(g.find("ShaderIndex").get("value")) if g.find("ShaderIndex") is not None else 0
            shader_name = d.shaders[shader_index][0] if shader_index < len(d.shaders) else ""

            weights = indices = valid = None
            cloth_like = "cloth" in shader_name.lower()
            if has_skin and "BlendWeights" in offsets and "BlendIndices" in offsets:
                raw_w = comp("BlendWeights")
                raw_i = comp("BlendIndices").astype(np.int64)
                bone_ids_text = g.findtext("BoneIDs") or ""
                bone_ids = np.array([int(x) for x in bone_ids_text.replace(",", " ").split()], dtype=np.int64)
                if len(bone_ids) == 0:
                    bone_ids = np.arange(len(skeleton.names))
                bad = ((raw_i >= len(bone_ids)) & (raw_w > 0)).any(axis=1)
                bad |= np.abs(raw_w.sum(axis=1) - 255.0) > 3.0
                safe_i = np.clip(raw_i, 0, len(bone_ids) - 1)
                indices = bone_ids[safe_i]
                bad |= ((indices >= len(skeleton.names)) & (raw_w > 0)).any(axis=1)
                weights = raw_w / 255.0
                valid = ~bad
                if bad.mean() > 0.02:
                    cloth_like = True
                    if not quiet:
                        log.info("  %s geometry %d: %d/%d vertices have invalid blend data -> weight transfer",
                                 d.name, len(geometries), int(bad.sum()), len(bad))
            else:
                # Rigid model: vertices are in bone space, bake them into model space.
                world = skeleton.world[model_bone] if model_bone < len(skeleton.world) else np.eye(4)
                pos = (world[:3, :3] @ pos.T).T + world[:3, 3]
                nrm = (world[:3, :3] @ nrm.T).T
                weights = np.zeros((len(pos), 4))
                weights[:, 0] = 1.0
                indices = np.full((len(pos), 4), model_bone, dtype=np.int64)

            geo = GtaGeometry(shader_index, pos, nrm, uv, weights, indices, tris, cloth_like)
            geo.valid = valid
            geometries.append(geo)
    return geometries


def load_skeleton(yft_xml, ydd_roots):
    if yft_xml:
        root = ET.parse(yft_xml).getroot()
        bones = root.findall(".//Skeleton/Bones/Item")
        if bones:
            log.info("Skeleton: %s (%d bones)", os.path.basename(yft_xml), len(bones))
            return GtaSkeleton(bones)
    for root in ydd_roots:
        for item in root.iter("Skeleton"):
            bones = item.findall("Bones/Item")
            if bones:
                log.warning("No .yft.xml skeleton, using a skeleton embedded in the drawables (%d bones)", len(bones))
                return GtaSkeleton(bones)
    raise SystemExit("No skeleton found: pass the ped's .yft.xml (CodeWalker export) with --yft")


# ----------------------------------------------------------------------------------------------
# SA skeleton refit
# ----------------------------------------------------------------------------------------------

class SaSkeleton:
    def __init__(self, path):
        data = json.load(open(path))
        self.frames = data["frames"]
        self.hanim = data["hanim"]
        self.names = [f["name"].strip() for f in self.frames]
        self.frame_of = {n: i for i, n in enumerate(self.names)}
        self.num_bones = len(self.hanim)
        self.matrix_index = {}   # stripped SA bone name -> skin matrix (HAnim) index
        self.bind = {}           # frame index -> 4x4 row-vector bone->mesh matrix
        for i, f in enumerate(self.frames):
            if "bind" in f:
                self.matrix_index[self.names[i]] = f["hanim_index"]
                self.bind[i] = np.array(f["bind"], dtype=np.float64)
        self.root_frame = next(i for i, f in enumerate(self.frames) if f.get("bone_id") == 0)
        self.pelvis_frame = self.frame_of["Pelvis"]

    def refit(self, gskel, enabled=True):
        """Move SA joints onto the GTA V joints and re-aim limb bones. Returns new bind matrices."""
        old = self.bind
        new = {}
        positions = {}
        for i in sorted(old):
            name = self.names[i]
            parent = self.frames[i]["parent"]
            pos = None
            if enabled and i != self.root_frame:
                for group in SA_JOINT_ANCHORS.get(name, []):
                    pos = gskel.joint_mesh(group)
                    if pos is not None:
                        break
                if pos is not None and name in ("Jaw", "L Brow", "R Brow"):
                    head = gskel.joint_mesh(["SKEL_Head"])
                    if head is None or np.linalg.norm(pos - head) > 0.25:
                        pos = None
            if pos is None:
                if parent in new and enabled:
                    # Keep the SA offset from the (already refitted) parent.
                    local = old[i] @ np.linalg.inv(old[parent])
                    pos = (local @ new[parent])[3, :3]
                else:
                    pos = old[i][3, :3]
                if enabled and i != self.root_frame:
                    log.debug("  SA joint '%s' has no GTA V anchor, keeping SA offset", name)
            positions[i] = pos
            m = old[i].copy()
            m[3, :3] = pos
            new[i] = m

        if enabled:
            for i in sorted(old):
                name = self.names[i]
                target = None
                if name in SA_AIM_CHILD:
                    target = positions[self.frame_of[SA_AIM_CHILD[name]]]
                elif name in SA_AIM_POINT:
                    target = gskel.joint_mesh(SA_AIM_POINT[name])
                if target is None:
                    continue
                d_new = target - positions[i]
                if np.linalg.norm(d_new) < 1e-4:
                    continue
                axes = old[i][:3, :3]
                q = rotation_between(axes[0], d_new)
                new[i][:3, :3] = orthonormalize(axes @ q.T)
                angle = np.degrees(np.arccos(np.clip(np.dot(axes[0], d_new / np.linalg.norm(d_new)), -1, 1)))
                log.debug("  re-aimed %-18s by %5.1f deg", name, angle)

            for i in sorted(old):
                moved = np.linalg.norm(new[i][3, :3] - old[i][3, :3])
                log.debug("  joint %-18s moved %.3f m", self.names[i], moved)
        return new


# ----------------------------------------------------------------------------------------------
# Weights
# ----------------------------------------------------------------------------------------------

class WeightMapper:
    def __init__(self, gskel, sa):
        self.gskel = gskel
        self.sa = sa
        self.unmapped_fallbacks = {}
        self.table = [self._resolve(i) for i in range(len(gskel.names))]

    def _resolve(self, bone):
        chain = []
        b = bone
        while b >= 0:
            name = self.gskel.names[b]
            if name in GTAV_TO_SA:
                if chain:
                    self.unmapped_fallbacks[self.gskel.names[bone]] = name
                return [(self.sa.matrix_index[sa_name], share) for sa_name, share in GTAV_TO_SA[name]]
            chain.append(name)
            b = self.gskel.parents[b]
        self.unmapped_fallbacks[self.gskel.names[bone]] = "Pelvis (no mapped ancestor)"
        return [(self.sa.matrix_index["Pelvis"], 1.0)]

    def map(self, weights, indices):
        """GTA V (n,4) weights/indices -> SA (n,4) weights / (n,4) uint8 matrix indices."""
        n = len(weights)
        acc = np.zeros((n, self.sa.num_bones))
        rows = np.arange(n)
        for k in range(4):
            w = weights[:, k]
            idx = np.clip(indices[:, k], 0, len(self.table) - 1)
            for bone in np.unique(idx[w > 0]):
                sel = (idx == bone) & (w > 0)
                for sa_idx, share in self.table[bone]:
                    acc[rows[sel], sa_idx] += w[sel] * share
        return finalize_weights(acc)


def finalize_weights(acc, min_weight=0.01):
    """Keep the 4 strongest influences, drop specks, renormalize."""
    order = np.argsort(-acc, axis=1)[:, :4]
    top = np.take_along_axis(acc, order, axis=1)
    top[top < min_weight] = 0.0
    total = top.sum(axis=1, keepdims=True)
    empty = total[:, 0] <= 0
    total[empty] = 1.0
    top = top / total
    order = order.astype(np.uint8)
    order[top == 0] = 0  # sorted descending, so unused influences are always the trailing slots
    return top, order


def transfer_weights(query_pos, ref_pos, ref_acc, k=4, chunk=256):
    """Inverse-distance blend of the k nearest reference vertices' SA weight rows."""
    out = np.zeros((len(query_pos), ref_acc.shape[1]))
    ref_sq = (ref_pos ** 2).sum(axis=1)
    for s in range(0, len(query_pos), chunk):
        q = query_pos[s:s + chunk]
        d2 = (q ** 2).sum(axis=1)[:, None] + ref_sq[None, :] - 2.0 * q @ ref_pos.T
        nn = np.argpartition(d2, k, axis=1)[:, :k]
        dist = np.sqrt(np.maximum(np.take_along_axis(d2, nn, axis=1), 1e-12))
        inv = 1.0 / dist
        inv /= inv.sum(axis=1, keepdims=True)
        out[s:s + chunk] = np.einsum("qk,qkb->qb", inv, ref_acc[nn])
    return out


def closest_point_weights(query, ref_pos, ref_tris, ref_acc, chunk=64):
    """Weight rows at the closest point of a reference surface (barycentric blend of that triangle)."""
    a, b, c = ref_pos[ref_tris[:, 0]], ref_pos[ref_tris[:, 1]], ref_pos[ref_tris[:, 2]]
    ab, ac = b - a, c - a
    out = np.zeros((len(query), ref_acc.shape[1]))
    for s in range(0, len(query), chunk):
        p = query[s:s + chunk, None, :]
        # Ericson, Real-Time Collision Detection 5.1.5, vectorised over (query, triangle)
        ap, bp, cp = p - a, p - b, p - c
        d1, d2 = (ab * ap).sum(-1), (ac * ap).sum(-1)
        d3, d4 = (ab * bp).sum(-1), (ac * bp).sum(-1)
        d5, d6 = (ab * cp).sum(-1), (ac * cp).sum(-1)
        va, vb, vc = d3 * d6 - d5 * d4, d5 * d2 - d1 * d6, d1 * d4 - d3 * d2
        denom = np.where(np.abs(va + vb + vc) < 1e-20, 1e-20, va + vb + vc)
        v, w = vb / denom, vc / denom
        with np.errstate(divide="ignore", invalid="ignore"):
            e_ab = np.clip(np.nan_to_num(d1 / (d1 - d3)), 0, 1)
            e_ac = np.clip(np.nan_to_num(d2 / (d2 - d6)), 0, 1)
            e_bc = np.clip(np.nan_to_num((d4 - d3) / ((d4 - d3) + (d5 - d6))), 0, 1)
        regions = [  # (condition, v, w) checked in order, interior last
            ((d1 <= 0) & (d2 <= 0), 0.0, 0.0),
            ((d3 >= 0) & (d4 <= d3), 1.0, 0.0),
            ((d6 >= 0) & (d5 <= d6), 0.0, 1.0),
            ((vc <= 0) & (d1 >= 0) & (d3 <= 0), e_ab, 0.0),
            ((vb <= 0) & (d2 >= 0) & (d6 <= 0), 0.0, e_ac),
            ((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), 1.0 - e_bc, e_bc),
        ]
        rv, rw, done = v.copy(), w.copy(), np.zeros(v.shape, bool)
        for cond, cv, cw in regions:
            sel = cond & ~done
            rv = np.where(sel, cv, rv)
            rw = np.where(sel, cw, rw)
            done |= sel
        q = a + rv[..., None] * ab + rw[..., None] * ac
        best = ((q - p) ** 2).sum(-1).argmin(1)
        rows = np.arange(len(best))
        bv, bw = rv[rows, best][:, None], rw[rows, best][:, None]
        t = ref_tris[best]
        out[s:s + chunk] = (1 - bv - bw) * ref_acc[t[:, 0]] + bv * ref_acc[t[:, 1]] + bw * ref_acc[t[:, 2]]
    return out


def smooth_weights(acc, positions, triangles, fixed, iterations, rate=0.5):
    """Laplacian-smooth weight rows over the surface (UV seams welded), holding `fixed` vertices.
    Neighbouring vertices skinned to different bones (left/right leg) otherwise tear the mesh apart."""
    if iterations <= 0:
        return acc
    _, weld = np.unique(np.round(positions, 4), axis=0, return_inverse=True)
    weld = weld.reshape(-1)
    nw = weld.max() + 1
    w_acc = np.zeros((nw, acc.shape[1]))
    np.add.at(w_acc, weld, acc)
    w_acc /= np.maximum(np.bincount(weld, minlength=nw), 1)[:, None]
    w_fixed = np.zeros(nw, bool)
    np.logical_or.at(w_fixed, weld, fixed)
    t = weld[triangles]
    e = np.unique(np.sort(np.vstack([t[:, [0, 1]], t[:, [1, 2]], t[:, [2, 0]]]), axis=1), axis=0)
    e = e[e[:, 0] != e[:, 1]]
    deg = np.bincount(e.ravel(), minlength=nw).astype(np.float64)[:, None]
    for _ in range(iterations):
        nsum = np.zeros_like(w_acc)
        np.add.at(nsum, e[:, 0], w_acc[e[:, 1]])
        np.add.at(nsum, e[:, 1], w_acc[e[:, 0]])
        avg = np.where(deg > 0, nsum / np.maximum(deg, 1), w_acc)
        w_acc = np.where(w_fixed[:, None], w_acc, (1 - rate) * w_acc + rate * avg)
    return w_acc[weld]


def dense(weights, indices, nbones):
    acc = np.zeros((len(weights), nbones))
    rows = np.arange(len(weights))
    for k in range(4):
        np.add.at(acc, (rows, indices[:, k].astype(np.int64)), weights[:, k])
    return acc


# ----------------------------------------------------------------------------------------------
# DDS -> D3D9 TXD
# ----------------------------------------------------------------------------------------------

FOURCC_DXT1 = struct.unpack("<I", b"DXT1")[0]
FOURCC_DXT3 = struct.unpack("<I", b"DXT3")[0]
FOURCC_DXT5 = struct.unpack("<I", b"DXT5")[0]
D3DFMT_A8R8G8B8 = 21
D3DFMT_X8R8G8B8 = 22


class Texture:
    def __init__(self, name):
        self.name = name
        self.width = self.height = 0
        self.fmt = None          # "DXT1" | "DXT3" | "DXT5" | "BGRA"
        self.levels = []         # list of bytes per mip
        self.has_alpha = False   # actual transparent texels present
        self.keep_alpha = False  # SA should render with alpha


def _mip_size(fmt, w, h):
    if fmt == "DXT1":
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 8
    if fmt in ("DXT3", "DXT5"):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16
    return w * h * 4


def load_dds(path, name):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"DDS ":
        raise ValueError("not a DDS file")
    height, width = struct.unpack_from("<2I", data, 12)
    mip_count = max(1, struct.unpack_from("<I", data, 28)[0])
    pf_flags, fourcc, bits = struct.unpack_from("<3I", data, 80)
    rmask, gmask, bmask, amask = struct.unpack_from("<4I", data, 92)
    offset = 128
    tex = Texture(name)
    tex.width, tex.height = width, height
    swizzle_rgba = False

    if pf_flags & 0x4:  # DDPF_FOURCC
        if fourcc == FOURCC_DXT1:
            tex.fmt = "DXT1"
        elif fourcc == FOURCC_DXT3:
            tex.fmt = "DXT3"
        elif fourcc == FOURCC_DXT5:
            tex.fmt = "DXT5"
        elif fourcc == struct.unpack("<I", b"DX10")[0]:
            dxgi = struct.unpack_from("<I", data, 128)[0]
            offset += 20
            tex.fmt = {71: "DXT1", 72: "DXT1", 74: "DXT3", 75: "DXT3", 77: "DXT5", 78: "DXT5",
                       87: "BGRA", 91: "BGRA", 28: "RGBA", 29: "RGBA"}.get(dxgi)
            if tex.fmt is None:
                raise ValueError(f"DXGI format {dxgi} (e.g. BC7) is not supported by GTA SA; re-save as DXT1/DXT5")
            if tex.fmt == "RGBA":
                tex.fmt, swizzle_rgba = "BGRA", True
        else:
            raise ValueError(f"unsupported FourCC {struct.pack('<I', fourcc)!r}")
    elif bits == 32:
        tex.fmt = "BGRA"
        swizzle_rgba = rmask == 0x000000FF
        if not (pf_flags & 0x1):  # no alpha channel
            amask = 0
    else:
        raise ValueError(f"unsupported uncompressed format ({bits} bpp)")

    w, h = width, height
    for _ in range(mip_count):
        size = _mip_size(tex.fmt, w, h)
        if offset + size > len(data):
            break
        level = data[offset:offset + size]
        if swizzle_rgba:
            px = np.frombuffer(level, np.uint8).reshape(-1, 4)[:, [2, 1, 0, 3]]
            level = px.tobytes()
        if tex.fmt == "BGRA" and pf_flags & 0x40 and not (pf_flags & 0x1):
            px = np.frombuffer(level, np.uint8).reshape(-1, 4).copy()
            px[:, 3] = 255
            level = px.tobytes()
        tex.levels.append(level)
        offset += size
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
    tex.has_alpha = texture_has_alpha(tex)
    return tex


def texture_has_alpha(tex, threshold=250):
    lvl = np.frombuffer(tex.levels[0], np.uint8)
    if tex.fmt == "BGRA":
        return bool((lvl.reshape(-1, 4)[:, 3] < threshold).any())
    if tex.fmt == "DXT1":
        blk = lvl.reshape(-1, 8)
        c0 = blk[:, 0].astype(np.uint16) | (blk[:, 1].astype(np.uint16) << 8)
        c1 = blk[:, 2].astype(np.uint16) | (blk[:, 3].astype(np.uint16) << 8)
        three = c0 <= c1
        if not three.any():
            return False
        idx = blk[three, 4:8]
        codes = np.stack([(idx >> s) & 3 for s in (0, 2, 4, 6)], axis=-1)
        return bool((codes == 3).any())
    blk = lvl.reshape(-1, 16)
    if tex.fmt == "DXT3":
        nib = blk[:, :8]
        alpha = np.minimum(nib & 0x0F, nib >> 4) * 17
        return bool((alpha < threshold).any())
    # DXT5
    a0 = blk[:, 0].astype(np.int32)
    a1 = blk[:, 1].astype(np.int32)
    if ((a0 >= threshold) & (a1 >= threshold)).all():
        return False
    bits = np.zeros(len(blk), dtype=np.uint64)
    for i in range(6):
        bits |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    codes = np.stack([((bits >> np.uint64(3 * k)) & np.uint64(7)).astype(np.int32) for k in range(16)], axis=1)
    pal = np.zeros((len(blk), 8), dtype=np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    eight = a0 > a1
    for i in range(2, 8):
        pal[:, i] = np.where(eight, ((8 - i) * a0 + (i - 1) * a1) // 7, 0)
    for i in range(2, 6):
        pal[:, i] = np.where(eight, pal[:, i], ((6 - i) * a0 + (i - 1) * a1) // 5)
    pal[:, 6] = np.where(eight, pal[:, 6], 0)
    pal[:, 7] = np.where(eight, pal[:, 7], 255)
    texel_alpha = np.take_along_axis(pal, codes, axis=1)
    return bool((texel_alpha < threshold).any())


def strip_alpha(tex):
    """Losslessly turn DXT3/DXT5 into opaque DXT1 (same color blocks), or BGRA into XRGB."""
    if tex.fmt == "BGRA":
        tex.levels = [np.frombuffer(l, np.uint8).reshape(-1, 4).copy() for l in tex.levels]
        for px in tex.levels:
            px[:, 3] = 255
        tex.levels = [px.tobytes() for px in tex.levels]
        return
    if tex.fmt not in ("DXT3", "DXT5"):
        return
    out = []
    for lvl in tex.levels:
        col = np.frombuffer(lvl, np.uint8).reshape(-1, 16)[:, 8:16].copy()
        c0 = col[:, 0:2].copy().view(np.uint16).ravel()
        c1 = col[:, 2:4].copy().view(np.uint16).ravel()
        idx = col[:, 4:8].copy().view(np.uint32).ravel()
        # DXT1 treats c0 <= c1 as 3-colour+transparent mode; DXT3/5 colour blocks are always 4-colour.
        swap = c0 < c1
        c0s, c1s = c0.copy(), c1.copy()
        c0s[swap], c1s[swap] = c1[swap], c0[swap]
        idx = np.where(swap, idx ^ np.uint32(0x55555555), idx)
        idx = np.where(c0 == c1, np.uint32(0), idx)
        res = np.empty((len(col), 8), np.uint8)
        res[:, 0:2] = c0s.astype("<u2").view(np.uint8).reshape(-1, 2)
        res[:, 2:4] = c1s.astype("<u2").view(np.uint8).reshape(-1, 2)
        res[:, 4:8] = idx.astype("<u4").view(np.uint8).reshape(-1, 4)
        out.append(res.tobytes())
    tex.levels = out
    tex.fmt = "DXT1"


def limit_texture_size(tex, max_size):
    while max_size and max(tex.width, tex.height) > max_size and len(tex.levels) > 1:
        tex.levels.pop(0)
        tex.width, tex.height = max(1, tex.width // 2), max(1, tex.height // 2)


# ----------------------------------------------------------------------------------------------
# RenderWare stream writers
# ----------------------------------------------------------------------------------------------

def rw_chunk(chunk_type, payload):
    return struct.pack("<3I", chunk_type, len(payload), RW_VERSION) + payload


def rw_string(s):
    raw = s.encode("ascii", "replace") + b"\0"
    raw += b"\0" * (-len(raw) % 4)
    return rw_chunk(0x02, raw)


def rw_struct(payload):
    return rw_chunk(0x01, payload)


def rw_ext(*children):
    return rw_chunk(0x03, b"".join(children))


def write_txd(path, textures):
    natives = []
    for t in textures:
        mips = len(t.levels) > 1
        if t.fmt == "DXT1":
            d3d, depth, raster = FOURCC_DXT1, 16, (0x0100 if t.keep_alpha else 0x0200)
        elif t.fmt == "DXT3":
            d3d, depth, raster = FOURCC_DXT3, 16, 0x0300
        elif t.fmt == "DXT5":
            d3d, depth, raster = FOURCC_DXT5, 16, 0x0300
        else:
            d3d, depth, raster = (D3DFMT_A8R8G8B8, 32, 0x0500) if t.keep_alpha else (D3DFMT_X8R8G8B8, 32, 0x0600)
        if mips:
            raster |= 0x8000
        flags = (0x1 if t.keep_alpha else 0) | (0x8 if t.fmt.startswith("DXT") else 0)
        header = struct.pack("<IBBH32s32sIIHHBBBB", 9, 6 if mips else 2, 0x11, 0,
                             t.name.encode()[:31], b"", raster, d3d, t.width, t.height,
                             depth, len(t.levels), 4, flags)
        body = header + b"".join(struct.pack("<I", len(l)) + l for l in t.levels)
        natives.append(rw_chunk(0x15, rw_struct(body) + rw_ext()))
    payload = rw_struct(struct.pack("<HH", len(textures), 0)) + b"".join(natives) + rw_ext()
    with open(path, "wb") as fh:
        fh.write(rw_chunk(0x16, payload))


class OutAtomic:
    def __init__(self, name, positions, normals, uvs, tris, tri_mat, materials, bone_w, bone_i):
        self.name = name
        self.positions = positions
        self.normals = normals
        self.uvs = uvs
        self.tris = tris
        self.tri_mat = tri_mat
        self.materials = materials  # texture names
        self.bone_w = bone_w
        self.bone_i = bone_i


def _frame_record(local, parent, flags):
    m = np.asarray(local, dtype=np.float64)
    return struct.pack("<12fiI", *m[0, :3], *m[1, :3], *m[2, :3], *m[3, :3], parent, flags)


def write_dff(path, sa, binds, atomics):
    frames = []          # (local 4x4 rows, parent, flags, extension bytes)
    for i, f in enumerate(sa.frames):
        if i in binds and f["parent"] in binds:
            local = binds[i] @ np.linalg.inv(binds[f["parent"]])
        else:
            local = np.eye(4)
            local[:4, :3] = np.array(f["local"])
        ext = b""
        if "bone_id" in f:
            if f["bone_id"] == 0:
                body = struct.pack("<3I", 0x100, 0, len(sa.hanim)) + struct.pack("<2I", 0, 36)
                body += b"".join(struct.pack("<3I", h["id"], h["index"], h["flags"]) for h in sa.hanim)
            else:
                body = struct.pack("<3I", 0x100, f["bone_id"], 0)
            ext += rw_chunk(0x11E, body)
        if f["name"] and i > 0:
            ext += rw_chunk(0x253F2FE, f["name"].encode())
        frames.append((local, f["parent"], f["flags"], ext))

    atomic_frames = []
    for a in atomics:
        atomic_frames.append(len(frames))
        frames.append((np.eye(4), sa.pelvis_frame, 3, rw_chunk(0x253F2FE, a.name.encode())))

    frame_struct = struct.pack("<I", len(frames)) + b"".join(_frame_record(l, p, fl) for l, p, fl, _ in frames)
    frame_list = rw_chunk(0x0E, rw_struct(frame_struct) + b"".join(rw_ext(e) if e else rw_ext() for *_, e in frames))

    ibms = [None] * sa.num_bones
    for i, b in binds.items():
        ibms[sa.frames[i]["hanim_index"]] = np.linalg.inv(b)
    ibm_bytes = b"".join(struct.pack("<16f", *m.ravel()) for m in ibms)

    geoms = []
    for a in atomics:
        nv, nt = len(a.positions), len(a.tris)
        center = (a.positions.min(axis=0) + a.positions.max(axis=0)) * 0.5
        radius = float(np.sqrt(((a.positions - center) ** 2).sum(axis=1)).max())
        gs = struct.pack("<4I", 0x00010036, nt, nv, 1)
        gs += a.uvs.astype("<f4").tobytes()
        raw_tris = np.stack([a.tris[:, 1], a.tris[:, 0], a.tri_mat, a.tris[:, 2]], axis=1).astype("<u2")
        gs += raw_tris.tobytes()
        gs += struct.pack("<4f2I", *center, radius, 1, 1)
        gs += a.positions.astype("<f4").tobytes() + a.normals.astype("<f4").tobytes()

        mats = []
        for tex_name in a.materials:
            ms = struct.pack("<I4BiI3f", 0, 255, 255, 255, 255, 4, 1, 1.0, 0.0, 1.0)
            tex = rw_chunk(0x06, rw_struct(struct.pack("<BBH", 6, 0x11, 1)) + rw_string(tex_name) + rw_string("") + rw_ext())
            mats.append(rw_chunk(0x07, rw_struct(ms) + tex + rw_ext()))
        mat_list = rw_chunk(0x08, rw_struct(struct.pack("<I", len(mats)) + struct.pack("<i", -1) * len(mats)) + b"".join(mats))

        bin_meshes = []
        total = 0
        for mi in range(len(a.materials)):
            idx = a.tris[a.tri_mat == mi].ravel().astype("<u4")
            if len(idx):
                bin_meshes.append(struct.pack("<2I", len(idx), mi) + idx.tobytes())
                total += len(idx)
        binmesh = rw_chunk(0x50E, struct.pack("<3I", 0, len(bin_meshes), total) + b"".join(bin_meshes))

        used = sorted(set(int(b) for b in np.unique(a.bone_i[a.bone_w > 0])))
        max_w = int((a.bone_w > 0).sum(axis=1).max()) if nv else 0
        skin = struct.pack("<4B", sa.num_bones, len(used), max_w, 0) + bytes(used)
        skin += a.bone_i.astype(np.uint8).tobytes() + a.bone_w.astype("<f4").tobytes()
        skin += ibm_bytes + b"\0" * 12
        skin_chunk = rw_chunk(0x116, skin)

        geoms.append(rw_chunk(0x0F, rw_struct(gs) + mat_list + rw_ext(binmesh, skin_chunk)))

    geom_list = rw_chunk(0x1A, rw_struct(struct.pack("<I", len(geoms))) + b"".join(geoms))
    atomic_chunks = []
    for gi, fi in enumerate(atomic_frames):
        atomic_chunks.append(rw_chunk(0x14, rw_struct(struct.pack("<4I", fi, gi, 5, 0)) +
                                      rw_ext(rw_chunk(0x1F, struct.pack("<2I", 0x116, 1)))))
    clump = rw_struct(struct.pack("<3I", len(atomics), 0, 0)) + frame_list + geom_list + b"".join(atomic_chunks) + rw_ext()
    with open(path, "wb") as fh:
        fh.write(rw_chunk(0x10, clump))


# ----------------------------------------------------------------------------------------------
# Conversion driver
# ----------------------------------------------------------------------------------------------

def find_inputs(args):
    src = os.path.abspath(args.input)
    folder = src if os.path.isdir(src) else os.path.dirname(src)
    ydds = []
    if os.path.isdir(src):
        for fn in sorted(os.listdir(src)):
            low = fn.lower()
            if low.endswith(".ydd.xml") or low.endswith(".ydr.xml"):
                ydds.append(os.path.join(src, fn))
    else:
        ydds.append(src)
    if not ydds:
        bins = [f for f in os.listdir(folder) if f.lower().endswith((".ydd", ".ydr"))]
        hint = f" Found binary {bins[:3]} - export them to XML in CodeWalker first." if bins else ""
        raise SystemExit(f"No .ydd.xml/.ydr.xml in {folder}.{hint}")

    def pick(ext, explicit):
        if explicit:
            return explicit
        cands = [os.path.join(folder, f) for f in sorted(os.listdir(folder)) if f.lower().endswith(ext)]
        if len(cands) > 1:
            base = os.path.basename(ydds[0]).split(".")[0].lower()
            same = [c for c in cands if os.path.basename(c).lower().startswith(base)]
            cands = same or cands
        return cands[0] if cands else None

    yft = pick(".yft.xml", args.yft)
    ymt = pick(".ymt.xml", args.ymt)
    tex_dirs = [folder] + list(args.textures or [])
    return folder, ydds, yft, ymt, tex_dirs


def index_textures(dirs):
    found = {}
    for d in dirs:
        for root, _, files in os.walk(d):
            for fn in files:
                if fn.lower().endswith(".dds"):
                    found.setdefault(fn[:-4].lower(), os.path.join(root, fn))
    return found


def default_out_dir(args, name):
    return os.path.abspath(args.out or os.path.join(TOOLS_DIR, "..", "models", name))


# Smoothing passes over a cloth piece's rigid weights. Measured on the a_f_y_topless_02 sarong over 15
# SA anims (walk/run/sprint/jump/crouch/sit): 8 passes take the worst p99 edge stretch 230% -> 158% with
# no more leg poke-through than GTA V's own LOD skinning; more passes stretch less but let the legs
# through the fabric (40 passes: 122% stretch, poke-through 23% -> 28%).
CLOTH_SMOOTH_PASSES = 8


def rig_cloth_geometry(d, g, mapper, sa, body_pos, body_acc):
    """Rigid skinning for a cloth-simulated GTA V mesh (SA has no cloth simulation).
    Vertices take the weights at the closest point of the garment's own artist-skinned lower LOD
    (else: of the body surface); vertices whose GTA V blend data is real skinning (the waistband)
    keep it; then the weights are smoothed over the surface, holding the waistband."""
    n = len(g.positions)
    if d.lod_reference:
        rp, rt, ra, base = [], [], [], 0
        for r in d.lod_reference:
            w, i = mapper.map(r.weights, r.indices)
            rp.append(r.positions); rt.append(r.triangles + base); ra.append(dense(w, i, sa.num_bones))
            base += len(r.positions)
        acc = closest_point_weights(g.positions, np.concatenate(rp), np.concatenate(rt), np.concatenate(ra))
    else:
        acc = transfer_weights(g.positions, body_pos, body_acc)
    fixed = np.zeros(n, bool)
    if g.valid is not None:
        fixed = g.valid.copy()
        own_w, own_i = mapper.map(np.where(g.valid[:, None], g.weights, 0), g.indices)
        acc[fixed] = dense(own_w, own_i, sa.num_bones)[fixed]
    acc = smooth_weights(acc, g.positions, g.triangles, fixed, CLOTH_SMOOTH_PASSES)
    return finalize_weights(acc)


def convert(args):
    t0 = time.time()
    folder, ydd_paths, yft_path, ymt_path, tex_dirs = find_inputs(args)
    name = args.name or os.path.basename(ydd_paths[0]).split(".")[0]
    out_dir = default_out_dir(args, name)
    os.makedirs(out_dir, exist_ok=True)

    log.info("Input folder : %s", folder)
    for p in ydd_paths:
        log.info("Drawables    : %s", os.path.basename(p))
    log.info("Skeleton     : %s", yft_path or "-")
    log.info("Variations   : %s", ymt_path or "- (runtime will auto-discover from DFF/TXD)")
    log.info("Output       : %s/%s.{dff,txd,xml}", out_dir, name)

    roots = [ET.parse(p).getroot() for p in ydd_paths]
    gskel = load_skeleton(yft_path, roots)
    sa = SaSkeleton(args.skeleton)

    drawables = []
    for path, root in zip(ydd_paths, roots):
        if root.tag == "DrawableDictionary":
            items = root.findall("Item")
        else:
            items = [root]
        for item in items:
            override = None
            if root.tag != "DrawableDictionary":
                override = os.path.basename(path).split(".")[0].split("^")[-1]
            d = parse_drawable(item, gskel, override)
            if not d.geometries:
                continue
            if not d.name.startswith(COMPONENT_PREFIXES):
                log.warning("Drawable '%s' is not a ped component name (head_000_u, uppr_001_r, ...). "
                            "PedFuncs Extended will treat it as always visible.", d.name)
            drawables.append(d)
    drawables.sort(key=lambda d: d.name)
    log.info("Parsed %d drawables (High LOD only)", len(drawables))

    binds = sa.refit(gskel, enabled=not args.no_refit)
    if args.no_refit:
        log.warning("--no-refit: using SA joint positions as-is (expect bad elbows/knees)")

    mapper = WeightMapper(gskel, sa)
    for bone, target in sorted(mapper.unmapped_fallbacks.items()):
        log.debug("  bone %-24s -> via %s", bone, target)

    # Pass 1: map weights of all skinnable geometry; it also serves as reference for cloth transfer.
    ref_pos, ref_acc = [], []
    mapped = {}
    for d in drawables:
        for gi, g in enumerate(d.geometries):
            if g.cloth_like:
                continue
            w, i = mapper.map(g.weights, g.indices)
            mapped[(d.name, gi)] = (w, i)
            if d.name.startswith(("head", "uppr", "lowr", "hand", "feet", "accs", "jbib", "task")):
                ref_pos.append(g.positions)
                ref_acc.append(dense(w, i, sa.num_bones))
    ref_pos = np.concatenate(ref_pos) if ref_pos else np.zeros((0, 3))
    ref_acc = np.concatenate(ref_acc) if ref_acc else np.zeros((0, sa.num_bones))

    tex_index = index_textures(tex_dirs)
    used_textures = {}   # lower name -> needs alpha (bool)
    atomics = []
    for d in drawables:
        mat_names, mat_lookup = [], {}
        pos_l, nrm_l, uv_l, tri_l, tm_l, w_l, i_l = [], [], [], [], [], [], []
        base = 0
        for gi, g in enumerate(d.geometries):
            shader_name, diffuse = d.shaders[g.shader_index] if g.shader_index < len(d.shaders) else ("", None)
            tex_name = (diffuse or f"{d.name}_diff").lower()[:31]
            if tex_name not in mat_lookup:
                mat_lookup[tex_name] = len(mat_names)
                mat_names.append(tex_name)
            alpha = any(h in shader_name.lower() for h in ALPHA_SHADER_HINTS)
            used_textures[tex_name] = used_textures.get(tex_name, False) or alpha

            if g.cloth_like:
                if d.lod_reference or len(ref_pos):
                    w, i = rig_cloth_geometry(d, g, mapper, sa, ref_pos, ref_acc)
                    log.info("  %s geometry %d (%s): rigid cloth skinning from %s", d.name, gi, shader_name,
                             "its lower LOD" if d.lod_reference else f"{len(ref_pos)} body vertices")
                else:
                    log.error("  %s: cloth geometry but no body geometry to transfer weights from", d.name)
                    w, i = finalize_weights(np.eye(sa.num_bones)[[sa.matrix_index["Pelvis"]] * len(g.positions)])
            else:
                w, i = mapped[(d.name, gi)]

            tris = g.triangles
            degenerate = (tris[:, 0] == tris[:, 1]) | (tris[:, 1] == tris[:, 2]) | (tris[:, 0] == tris[:, 2])
            tris = tris[~degenerate]
            pos_l.append(g.positions)
            nrm = g.normals / np.maximum(np.linalg.norm(g.normals, axis=1, keepdims=True), 1e-8)
            nrm_l.append(nrm)
            uv_l.append(g.uvs)
            tri_l.append(tris + base)
            tm_l.append(np.full(len(tris), mat_lookup[tex_name]))
            w_l.append(w)
            i_l.append(i)
            base += len(g.positions)

        if base > 65535:
            raise SystemExit(f"{d.name}: {base} vertices exceeds RenderWare's 65535 per geometry limit")
        pos = np.concatenate(pos_l) @ V2SA.T
        nrm = np.concatenate(nrm_l) @ V2SA.T
        a = OutAtomic(d.name, pos, nrm, np.concatenate(uv_l), np.concatenate(tri_l), np.concatenate(tm_l),
                      mat_names, np.concatenate(w_l), np.concatenate(i_l))
        atomics.append(a)
        bones_used = sorted(set(int(b) for b in np.unique(a.bone_i[a.bone_w > 0])))
        names = {v: k for k, v in sa.matrix_index.items()}
        log.info("  %-12s %6d verts %6d tris  materials=%s  bones=%s", d.name, len(pos), len(a.tris),
                 ",".join(mat_names), ",".join(names[b] for b in bones_used))

    # Variation textures (_b, _c, ... and other skin tones) are not referenced by shaders but are
    # swapped in at runtime, so ship every <comp>_diff_<NNN>_* texture of the exported drawables.
    families = {}
    for d in drawables:
        m = re.match(r"([a-z_]+?)_(\d{3})", d.name)
        if m:
            families[f"{m.group(1)}_diff_{m.group(2)}"] = d.name
    alpha_families = {re.sub(r"_[a-z]_[a-z]{3}$", "", t) for t, a in used_textures.items() if a}
    wanted = dict(used_textures)
    for tname in tex_index:
        fam = re.sub(r"_[a-z]_[a-z]{3}$", "", tname)
        if fam in families and tname not in wanted:
            wanted[tname] = fam in alpha_families

    textures = []
    for tname in sorted(wanted):
        path = tex_index.get(tname)
        if not path:
            log.warning("Texture '%s' is referenced but no %s.dds was found (pass --textures <dir>)", tname, tname)
            continue
        try:
            tex = load_dds(path, tname)
        except ValueError as e:
            log.error("Texture '%s': %s", tname, e)
            continue
        limit_texture_size(tex, args.max_texture_size)
        orig_fmt = tex.fmt
        tex.keep_alpha = tex.has_alpha and wanted[tname]
        if not tex.keep_alpha and tex.fmt in ("DXT3", "DXT5", "BGRA"):
            strip_alpha(tex)
        textures.append(tex)
        log.info("  tex %-26s %4dx%-4d %d mips  %s -> %s%s", tname, tex.width, tex.height, len(tex.levels), orig_fmt,
                 tex.fmt, " (alpha)" if tex.keep_alpha else "")

    dff_path = os.path.join(out_dir, f"{name}.dff")
    txd_path = os.path.join(out_dir, f"{name}.txd")
    write_dff(dff_path, sa, binds, atomics)
    write_txd(txd_path, textures)
    log.info("Wrote %s (%d bytes, %d atomics)", dff_path, os.path.getsize(dff_path), len(atomics))
    log.info("Wrote %s (%d bytes, %d textures)", txd_path, os.path.getsize(txd_path), len(textures))
    outputs = [dff_path, txd_path]
    if ymt_path:
        ymt_out = os.path.join(out_dir, f"{name}.xml")
        shutil.copyfile(ymt_path, ymt_out)
        outputs.append(ymt_out)
        log.info("Wrote %s (copied from %s)", ymt_out, os.path.basename(ymt_path))

    if args.install:
        os.makedirs(args.install, exist_ok=True)
        for stale in (f"{name}.ymt.xml",):
            p = os.path.join(args.install, stale)
            if os.path.exists(p):
                os.remove(p)
                log.info("Removed stale %s (superseded by %s.xml)", p, name)
        for p in outputs:
            shutil.copy2(p, os.path.join(args.install, os.path.basename(p)))
        log.info("Installed into %s", args.install)

    log.info("Done in %.1fs", time.time() - t0)


def main():
    ap = argparse.ArgumentParser(description="Convert a GTA V ped (CodeWalker XML export) to a GTA SA DFF/TXD for PedFuncs Extended.")
    ap.add_argument("input", help="folder containing <ped>.ydd.xml/.yft.xml/.ymt.xml (+ DDS textures), or a .ydd.xml file")
    ap.add_argument("--name", help="output model name (e.g. male01); default: the GTA V ped name")
    ap.add_argument("--out", help="output directory (default: models/<name>/ in this project)")
    ap.add_argument("--install", metavar="DIR", help="also copy the results into this folder (e.g. modloader/MyPed)")
    ap.add_argument("--yft", help="skeleton .yft.xml (default: auto-detect next to the .ydd.xml)")
    ap.add_argument("--ymt", help="variation .ymt.xml (default: auto-detect)")
    ap.add_argument("--textures", action="append", metavar="DIR", help="extra folder to search for .dds textures (repeatable)")
    ap.add_argument("--max-texture-size", type=int, default=0, help="drop top mip levels until textures fit (e.g. 512)")
    ap.add_argument("--skeleton", default=DEFAULT_SKELETON, help="SA skeleton template JSON")
    ap.add_argument("--no-refit", action="store_true", help="debug: keep SA joint positions instead of fitting to GTA V")
    ap.add_argument("--log", help="log file (default: <out>/<name>_convert.log)")
    ap.add_argument("-q", "--quiet", action="store_true", help="only warnings and errors on the console")
    args = ap.parse_args()

    name = args.name or "gtav_to_sa"
    out_dir = default_out_dir(args, name)
    os.makedirs(out_dir, exist_ok=True)
    log_path = args.log or os.path.join(out_dir, f"{name}_convert.log")

    log.setLevel(logging.DEBUG)
    console = logging.StreamHandler(sys.stdout)
    console.setLevel(logging.WARNING if args.quiet else logging.INFO)
    console.setFormatter(logging.Formatter("%(levelname)-7s %(message)s"))
    fileh = logging.FileHandler(log_path, mode="w", encoding="utf-8")
    fileh.setLevel(logging.DEBUG)
    fileh.setFormatter(logging.Formatter("%(asctime)s %(levelname)-7s %(message)s"))
    log.addHandler(console)
    log.addHandler(fileh)
    log.info("Full debug log: %s", log_path)
    try:
        convert(args)
    except SystemExit as e:
        if isinstance(e.code, str):
            log.error("%s", e.code)
            sys.exit(1)
        raise
    except Exception:
        log.exception("Conversion failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
