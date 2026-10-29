# PedFuncs Extended

An ASI plugin for **GTA San Andreas (v1.0 US)** that brings GTA V's ped variation system to SA: one ped
model holds many interchangeable body parts and textures, and every spawned ped gets its own random
combination. It also includes a converter that turns GTA V peds into SA peds.

PedFuncs Extended is a full replacement for the original **PedFuncs**: its `_remap` textures, extra
ped TXDs, gang hands and script exports all keep working. Don't install both.

## Features

- **12 component slots**, as in GTA V: `head`, `berd`, `hair`, `uppr`, `lowr`, `hand`, `feet`, `teef`,
`accs`, `task`, `decl`, `jbib`.
- **Several drawables (meshes) per slot**: each one is its own skinned atomic in the DFF. Only the
chosen ones are drawn; hidden atomics cost no draw calls.
- **Several textures per drawable**, swapped per ped at render time, so peds of the same model share
one model but look different.
- **Skin-tone matching**: textures tagged `_whi`, `_bla`, `_lat`, `_chi`, `_ara`, `_ind` or `_uni`.
Each ped gets one skin tone, and every part is picked to match it.
- **GTA V variation XML**: the ped's `.ymt` exported by CodeWalker gives each texture its GTA V spawn
chance (`distribution`).
- **Works without XML**: components and textures are found from atomic and texture names alone.
- **Multi-clump and modular DFFs**: a DFF that holds several clumps is merged into one, and extra
parts can be added as separate DFFs in a folder named after the model.
- **Script API**: plain C exports for CLEO, MoonLoader and other ASI plugins.
- **GTA V → SA converter** (`tools/gtav_to_sa.py`).

## Installation

Requires an ASI loader. Modloader is recommended.

1. Copy `PedFuncsExtended.asi` and `PedFuncsExtended.ini` to `modloader/PedFuncsExtended/`.
2. Put each ped's `<model>.dff`, `<model>.txd` and (optional) `<model>.xml` in a modloader folder,

   e.g. `modloader/MyPeds/`.
3. Remove the original `PedFuncs.asi` if you have it.

## Configuration (`PedFuncsExtended.ini`)

| Key | Default | Meaning |
|---|---|---|
| `UseLog` | 1 | write `PedFuncsExtended.log` next to the ASI |
| `LogLevel` | 4 | 1 errors, 2 + warnings, 3 + model loading, 4 + every ped spawn's chosen parts |
| `EnableGtaVVariations` | 1 | 0 = legacy PedFuncs remaps only |
| `RandomizeOnSpawn` | 1 | 0 = always drawable 0 / texture 0 |
| `LoadAdditionalPedsTxds` | 1 | load `peds1.txd`–`peds4.txd` and `ganghands.txd` |

## Making a variation ped

### Naming in the DFF and TXD

| What | Name | Example |
|---|---|---|
| Atomic (frame name) | `<slot>_<drawable>_<u\|r>` | `head_000_r`, `uppr_001_u` |
| Texture | `<slot>_diff_<drawable>_<variant>_<tone>` | `head_diff_000_a_whi`, `lowr_diff_001_b_uni` |

- `<drawable>` is a 3-digit index and `<variant>` is a letter (`a`, `b`, `c`, …).
- `<tone>` is `uni` (works with any skin tone) or one of `whi`, `bla`, `lat`, `chi`, `ara`, `ind`.
- A drawable is only chosen for a ped if it has a texture matching that ped's skin tone (or `uni`).

### The variation XML (`<model>.xml`)

This is optional. It is the GTA V ped's `.ymt` exported to XML with CodeWalker, renamed to
`<model>.xml`. When present, each texture's `distribution` value sets how often it is picked.

The plugin looks for it next to the model (modloader folders included), then in `models/`,
`models/generic/` and `PedFuncs/`. The model name comes from `data/peds.ide`, or for peds added
elsewhere, from matching XML file names.

### Modular parts

Extra DFFs placed in `models/<model>/` or `PedFuncs/<model>/` are merged into the ped when it loads.

### Editing in Blender (DragonFF)

Peds, including multi-clump ones, can be imported and exported with
[Edited-DragonFF](https://github.com/Joe-Shield-s/DragonFF)

- **Import:** a multi-clump DFF is split into one collection per clump, each with its own armature.
- **Export:** set each clump's collection to DragonFF type **Clump** with a clump index (0, 1, …) in its
collection properties. Each of those collections is written as one clump, in index order.
- Each drawable must stay a separate object, with a frame named as in the table above  
(`head_000_r`, …).

## Converting a GTA V ped

1. In CodeWalker, right-click the ped's `.ydd`, `.yft` and `.ymt` → **Export XML**. Export the `.ytd`

   too, to get its DDS textures.
2. Run the converter (Python 3.9+ and numpy):

```bash
python3 tools/gtav_to_sa.py "<folder with the exported files>" --name male01 \
    --install "<GTA SA>/modloader/Peds"
```

This writes `male01.dff`, `male01.txd` and `male01.xml` to `models/male01/` (and to `--install` if
given), plus a detailed `male01_convert.log`.

Options:

- `--out DIR`: output folder.
- `--textures DIR`: extra DDS folders.
- `--max-texture-size N`: shrink large textures.
- `--yft` / `--ymt`: choose the input files instead of auto-detecting them.
- `-q`: quiet.

What the conversion does:

- **Skeleton:** fits SA's ped skeleton to the GTA V body, so elbows, knees and shoulders bend in the
right place with SA animations.
- **Weights:** maps every GTA V bone (helper and extra bones included) to SA's bones.
- **Cloth (skirts, sarongs):** GTA V simulates these; SA has no cloth simulation, so they are skinned
instead, from the garment's own non-simulated LOD and smoothed so the legs don't tear them.
- **Textures:** converts them to an SA TXD with correct wrapping, mipmaps and alpha. Every texture
variant is included.
- **Mesh:** exports only the High LOD, one skinned atomic per drawable.

Preview a result posed with real SA animations, without starting the game (needs Pillow):

```bash
python3 tools/preview_ped.py "<path to dff>" --ifp "<GTA SA>/anim/ped.ifp" --anim walk_civi,run_civi
```

## Script API

These are exported from `PedFuncsExtended.asi` (`__cdecl`). `ped` is a `CPed*`, `compId` is 0–11 in
the slot order above, and `anchorId` is a prop anchor (0 `p_head`, 1 `p_eyes`, 2 `p_ears`,
6 `p_lwrist`, 7 `p_rwrist`).

```cpp
int32_t     Ext_GetPedComponentDrawable(CPed* ped, int compId);
int32_t     Ext_GetPedComponentTexture(CPed* ped, int compId);
bool        Ext_SetPedComponentVariation(CPed* ped, int compId, int drawableId, int textureId);
int32_t     Ext_GetPedPropIndex(CPed* ped, int anchorId);          // -1 = none
int32_t     Ext_GetPedPropTexture(CPed* ped, int anchorId);
bool        Ext_SetPedPropVariation(CPed* ped, int anchorId, int propId, int textureId);
void        Ext_RandomizePedVariations(CPed* ped);
bool        Ext_PedHasVariations(CPed* ped);
const char* Ext_GetPedSkinTone(CPed* ped);                         // "whi", "lat", ...
bool        Ext_SetPedSkinTone(CPed* ped, const char* skinTone);   // re-matches every part

// Original PedFuncs API
int32_t     Ext_GetPedRemap(CPed* ped, int index);
void        Ext_SetPedRemap(CPed* ped, int index, int num);
void        Ext_SetAllPedRemaps(CPed* ped, int num);
```

## Not supported yet

- **Props:** they are selected and stored, and the API reports them, but they aren't drawn.
- **Variation XML rules:** `inclusions` / `exclusions` are read but not applied.
