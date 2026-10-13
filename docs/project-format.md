# Compositor project format, versions 1–15

## Windows single-file format (versions 12–15)

The Windows implementation writes a ZIP64 `.comp` file. It contains
`manifest.json` and `images/` PNG assets, with the same UUID names and
bottom-to-top layer ordering as the legacy packages described below.
`comp::ProjectManifest::current` is `15` in `windows/src/io/ProjectStore.h`.
The preserved macOS Swift target continues writing its directory-based v11
format; its independent version constant remains 11.

Version 15 adds `lastEditRegionFile` (null or `last-edit-region.png` under
`images/`) and `lastEditTarget` with `layer_id`, `scope` and the layer's
`transform`. The grayscale region records the successful task's actual region,
independently of the current selection. Clearing a selection does not erase
this reference. A changed canvas size or target transform requires selecting
the region again rather than guessing its location.

Two new adjustment kinds preserve non-destructive local deformation. `warp`
stores layer-pixel `x`, `y`, `radius`, `dx`, `dy` and `strength` (-0.5–0.5,
negative contracts). `arm_slim` stores `side` (`left`, `right`, `both`),
`strength` (0–0.2), one to four `[x0,y0,x1,y1,radius]` segments and a
`head: [x,y,radius]` protection circle. Segment endpoints protect joints.
Their optional ordinary adjustment masks freeze pixels outside the region.
Both use premultiplied interpolation and retain original layer PNG pixels.
Source coordinates, masks and parameters survive saving and version restore.

Version 14 adds an optional top-level `selectionFile` (`selection.png`, stored
under `images/`) containing a canvas-sized 8-bit grayscale mask. White is the
editable region; black is protected. Selection survives reopening and version
restoration. `editRecords` contains at most 20 recent successful tool records,
including ordered command arguments, region identifiers and optional
`before_version_id` references to local history. These are execution metadata,
not conversation transcripts or credentials.

Layer `replaceBelow` (default false) blends premultiplied RGBA replacement
coverage into the existing backdrop using the independent layer mask. Unlike
source-over compositing, a transparent generated pixel can erase the selected
backdrop. Pixels outside the replacement mask are unchanged. PSD export bakes
the affected lower stack because Photoshop pixel layers cannot directly express
this replacement operation.

Adjustment nodes may carry `processingBounds: [x,y,width,height]` in their
processing coordinate space. This preserves the original spatial extent of
vignette, grain and neighborhood effects when a canvas is expanded. Original
layers remain visible and editable; expansion adds a separately masked layer.

Normal saves contain the complete current document. Optional bundled history
uses `history/index.json`, content-addressed `history/snapshots/<SHA256>.json`,
shared `history/objects/<SHA256>.png` assets and optional
`history/thumbnails/<version UUID>.png`. Descriptor objects contain `manifest`
and `assets` (project image path to shared PNG filename). Index entries contain
`id`, `title`, UTC `created`, `parent`, `file` and `named`. All history is bounded
and validated before its index is atomically imported into the local store.
Without this option history stays in the current user's application data.
Naming pins a version; default retention is 100 automatic states per project
and 10 GiB shared storage. PNG objects are deduplicated across states. The local
atomic index also records successfully persisted transactions for recovery.

Version 13 adds optional layer `isLocked` (boolean, default false). A locked
group also locks its descendants. Lock state persists in the project. AI
conversations, credentials, connection profiles, personal presets, tutorial
state and workspace layout remain local. A layer can be unlocked without
changing its other properties. Editing transactions validate locked content
before committing, including edits submitted by the AI assistant.

Version 12 adds the optional layer `adjustments` array. Each node has an `id`,
`kind`, `enabled`, `opacity` (0–1), `parameters` object and optional `maskFile`.
Kinds are `tone`, `levels`, `curves`, `invert` and `legacy`. Nodes execute in
array order, starting from the original layer pixels. Disabled nodes retain
their parameters. Original PNG pixels are never overwritten by grading.
An adjustment layer has `isAdjustment: true` and applies its stack to the
composited siblings below it. Node masks use local layer coordinates, or
document coordinates for adjustment layers. Node mask filenames are
`<layer UUID>.<node UUID>.adjustment-mask.png` under `images/`.

`tone` stores named numeric controls whose ranges are declared by
`parameterSpecs()` in `windows/src/core/Adjustment.cpp`; absent values are
identity. `levels` uses `rgb`, `r`, `g`, `b` prefixes followed by `Black`,
`White`, `Gamma`, `OutputBlack`, `OutputWhite`. Byte levels are 0–255 and
gamma is 0.01–10. `curves` maps those four channel keys to ordered `[x,y]`
point arrays in normalized 0–1 units, using shape-preserving cubic Hermite
interpolation. `legacy` retains a v7–11 adjustment object without discarding
its original settings. It uses the migrated implementation of that effect.
Windows color objects may also include optional `alpha` (0–1, default 1).
Legacy hue ranges accept Swift's alternating key/value dictionary encoding,
including custom bands, inverted ranges and colorize settings.

Groups add `groupCompositing`: `legacy`, `passThrough`, or `isolated`.
Legacy opacity and masks multiply into each descendant leaf. Pass-through
groups allow children to interact with the parent backdrop, then apply the
group mask and opacity to the change. Isolated groups start with a transparent
backdrop and blend their completed result into the parent. Missing values in
legacy imports mean `legacy`, preserving the previous group behavior.

The processing order is decode and ICC conversion → original pixels → local
adjustment stack → layer/adjustment-layer composition → display or export.
Working space is 8-bit sRGB; intermediate adjustments and compositing use
floating point, with premultiplied resampling and straight RGB grading.
ZIP and PNG assets are bounded, CRC checked, and never extracted to disk.
Duplicate, encrypted, escaping and missing paths are rejected. The Windows
limit is 200 million pixels per surface and a RAM-based aggregate raster
budget (800 MB–3.2 GB), including masks. ZIP64 is forced even for small saves.
Save writes a temporary snapshot and atomically replaces the destination only
after successful finalization. The in-memory undo stack remains session-only;
v14 selection and optional version history are described above.

Windows imports ZIP v12–14 and directory v1–11 projects. Their first normal save
opens Save As with a `-v15.comp` name; original projects remain intact by default.
It never overwrites an old directory package. See
[`windows/README.md`](../windows/README.md) for conversion limitations.

## Legacy macOS directory format (versions 1–11)

A `.comp` file is a macOS document package containing `manifest.json` and an `images/` directory of `<layer UUID>.png` assets.

The manifest identifies `com.compositor.project`, version `11` for new saves (versions `1`–`10` remain readable), and the sRGB working space. It stores document UUID, pixel dimensions, active layer UUID, and layers in bottom-to-top order. Each layer stores its UUID, name, visibility, transform (origin, size, clockwise rotation, flips, sampling), and optional image filename. Blank layers have no image asset.

Embedded PNGs preserve source pixels and transparency; transforms remain separate. Projects survive moving or deleting imported source photos. Saving uses a coordinated atomic package replacement. Unsupported versions, invalid metadata, missing assets, unsafe paths, and oversized data are rejected before replacing the live document.

Limits: 30,000 pixels per canvas/image side, 100 million total source pixels, 10,000 layers, 4 MiB manifest, 512 MiB per encoded asset. See `ProjectStore.swift` for validation.

Undo history and viewport are session-only. Opening fits the canvas, restores selection, and starts with clean history. Future editable features must extend the schema and round-trip tests. PNG export is a flattened derivative and does not mark project edits saved.

Image Size adds optional `resolution` (pixels/inch, 1–9600). Older manifests without it default to 72. This additive field retains version 1 compatibility. Both PNG and JPEG exports include document resolution metadata. Resampling stores the new layer pixels and bounds; undo retains the prior sources only during the current session.

Version 2 adds optional `parentID` and `isGroup` on layer records. A group has no image file. Root nodes have no parent; children refer to an existing group. Array order defines bottom-to-top sibling order; renderers traverse each group as a contiguous subtree. Visibility is inherited without changing child flags. Cycles, missing/non-group parents, image-bearing groups, and nesting beyond 64 ancestor levels are rejected. Group ancestors permit room for leaf nodes at the deepest level. Group metadata survives image/canvas resizing and cropping. Older app builds reject version 2 rather than misrender grouped documents. Collapse state is not serialized.

Version 3 adds optional per-layer `opacity` (finite 0–1) and `blendMode` (Normal, Multiply, Screen, Overlay, Darken, Lighten, Difference, Color Dodge, Color Burn). Missing fields default to full opacity and Normal. Group records required those defaults until version 8, which lets a folder carry its own opacity; a folder's opacity multiplies into every layer inside it, while its blend mode stays Normal because folders are pass-through. Effects are applied during compositing and retained as metadata when resizing sources. Files declaring older versions cannot contain non-default appearance values.

Version 4 adds optional `maskFile` and `maskEnabled` fields to individual layers. Mask filenames must be `<layer UUID>.mask.png` under `images/`; enabled defaults to true when a mask exists. Records without masks omit both fields. Groups cannot carry masks in this version. Files declaring versions 1–3 cannot contain mask metadata.

Masks store 8-bit grayscale coverage without alpha (white reveals, black hides). Their normalized extent matches the image’s local rectangle, so the same layer transform applies to both. A uniform 1×1 mask is valid and avoids allocating full-resolution pixels before painting. Nonuniform mask pixels and a thumbnail are immutable assets shared by history. Image Size resamples them with the image transform; Canvas Size and Crop preserve their pixels. Up to 100 million mask pixels may be stored in addition to the existing 100 million image pixels; per-side and per-file limits also apply to masks. Disabled masks remain embedded and editable but do not affect compositing. Image-versus-mask target selection is session-only and reopens on image pixels.

Version 5 adds optional `maskSourceID`: the UUID of a non-group layer supplying live alpha in document coordinates. It multiplies the target’s alpha alongside its enabled raster mask. Source pixels, transform, opacity, raster mask and upstream live masks contribute coverage; visibility and RGB color do not. Sources remain independent layers. Missing references, self-links, cycles, group endpoints and chains over 256 nodes are rejected. Deletion can bake the live coverage into dependent image pixels (retaining their raster masks) or remove the links, as one undoable operation. Links survive image/canvas resize and crop. Older versions default to no live mask; older app builds reject v5.

UI terminology: these alpha links are clipping masks. Option-click assigns the lower sibling’s base or releases the connection. Multiple clipped layers share one base, show indented above it, and release when moved outside the contiguous stack. The underlying `maskSourceID` representation is unchanged.

Version 6 allows `maskFile` and `maskEnabled` on group records. A folder has no image, so its mask covers the folder's own transform rectangle (the canvas size when the folder was created); Image Size resamples it through that transform, and Canvas Size and Crop preserve its pixels, exactly as for layer masks. Groups are pass-through, so an enabled folder mask multiplies the coverage of every descendant layer, together with that layer's own mask and any enclosing folders' masks; clipping-mask coverage is unaffected. Files declaring versions 1–5 cannot give a group a mask, and older app builds reject v6.

Version 7 adds adjustment layers: a layer record with an optional `adjustment` object and no `imageFile`. Adjustment layers cannot be groups and cannot carry `text`; like any layer they take a transform, opacity, blend mode, raster mask and clipping link, and they affect everything composited below them within their folder. `kind` is one of `Hue/Saturation`, `Levels`, `Curves`, `Exposure`, `Gradient Map`, `Grain`, `Invert`, `Black & White` and `Color Balance` (version 9 adds three more). The record carries the settings of every kind, each optional and defaulting to an identity adjustment: `hue` (±360), `saturation` and `lightness` (±100), `colorize` and `hsvSettings` for Hue/Saturation; `levels` (four channel ranges, RGB then red, green, blue); `curves` (four channel point lists); `exposureSettings`; `gradientMapSettings` (`shadows`, `highlights`, `reversed`); `grainSettings`; `blackWhiteSettings`; `colorBalanceSettings`. Out-of-range or non-finite values are rejected. Files declaring versions 1–6 cannot contain adjustment records, and older app builds reject v7. See `LayerAdjustment.swift` for the exact ranges.

Version 8 lets a folder carry its own `opacity`, which multiplies into every layer inside it; a folder's blend mode stays Normal because folders are pass-through (files declaring 1–7 require folders at full opacity). It also adds an optional top-level `guides` array of alignment guides, each with `id`, `axis` (`horizontal` or `vertical`) and `position` in document pixels (finite, at most 1,000,000 in magnitude). At most 1,000 guides are stored; files declaring 1–7 cannot contain guides. Guides survive Canvas Size and Crop by offsetting with the canvas.

Version 9 adds three adjustment kinds that sample neighboring pixels: `Gaussian Blur` (`blurRadius`, 0.1–250 document pixels), `Motion Blur` (`motionAngle`, −90 to 90 degrees, and `motionDistance`, 1–2000) and `Add Noise` (`noiseAmount`, 0.1–400, `noiseGaussian`, `noiseMonochromatic` and `noiseSeed`, so the pattern is stable between sessions). Files declaring 1–8 cannot contain these kinds; the earlier adjustment kinds remain valid at version 7 and up.

Version 10 lets a text layer color some of its letters differently: optional `colorRuns` in its `text` metadata (see Editable text). Files declaring 1–9 cannot contain it.

Version 11 lets those letters use different faces too: optional `fontRuns` in the same metadata. Files declaring 1–10 cannot contain it. `colorRuns` stays valid from version 10.

### Additive layer fields

Later fields are optional and not gated on the version, so older readers ignore them and keep the pixels or the linked mask as they were:

- `maskPlacement` and `maskLinked`: an unlinked mask (`maskLinked` false; missing means linked) keeps its own transform in `maskPlacement`, a document-space rectangle like the layer transform, and no longer follows the layer when it moves. Both require a `maskFile`.
- `shape`: a layer made with the Shape tool keeps its style (`kind`, `red`/`green`/`blue`, `cornerRadius` in document pixels, and for lines `lineWidth` plus `start` and `end` as fractions of the layer box) so it redraws cleanly when scaled. Its PNG is still an ordinary raster; once anything else changes those pixels the metadata is dropped.

### Editable text

Pixel layer records may include optional `text` metadata: content, PostScript font name, font size in pixels, RGB color, alignment, tracking, line spacing and optional `boxSize` paragraph bounds. Text wraps inside these bounds; changing them reflows the text without scaling the font. The PNG remains the display and export fallback. Older readers ignore this metadata. Transforms, duplication, masks and canvas-size changes preserve it; destructive pixel operations rasterize text and omit the metadata on the next save. Missing fonts use the system font when edited, while the saved PNG preserves the original appearance until then. From version 10, optional `colorRuns` lists letters painted in another color than the text's own `red`/`green`/`blue`: each run has `location` and `length` in UTF-16 units of the content, plus `red`, `green` and `blue` (0–1). From version 11, optional `fontRuns` lists letters set in another face than `fontName`: the same `location` and `length`, plus `fontName`. Runs of either kind are sorted, do not overlap, have a positive length and end within the content; letters outside every run use the text's color or face.

### Layer effects

An optional `effects` record contains independent `stroke`, `shadow`, `colorOverlay`, `innerShadow`, `outerGlow` and `innerGlow` records. Stroke carries a size (0–500 layer pixels), a color, an opacity and an `inside` flag choosing which side of the edge it sits on; drop shadow and inner shadow each carry an angle, a distance, a blur, a color and an opacity; color overlay carries a color and an opacity; outer glow and inner glow each carry a size (0–500 layer pixels), a color and an opacity. Each supports optional `enabled` visibility (missing means visible); hidden effects keep all parameters and remain listed under their layer. Effects, including their visibility, are saved and participate in document undo. Canvas previews run on a serial background worker with a shared pixel budget; exports render the full-resolution effects. A record omitting an effect means that layer does not have it, so older readers see the effects they understand and ignore the rest.
