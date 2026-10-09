# Native CIFF to DWG

The CLI, `ciff::ConvertToFile` library API, and CadCast dispatch accept `.dwg` targets.
Example: `CogniteCIFF model.ciff model.dwg`. DWG export is synchronous; explicit
asynchronous requests fail without replacing the target.

`ConvertDWG.h` contains the native AC1015 / AutoCAD 2000 serializer and CIFF adapter.
`DefaultDWG.h` contains the small empty drawing data resource. Neither requires FBX,
an installed Autodesk application, or an external converter at runtime.

Stored meshes already contain world-space coordinates. Analytic primitives use the
existing canonical tessellators and a DWG-specific double precision placement frame,
including reflected winding; the float matrices used by other outputs are unchanged.
DWG export applies no additional scale. CIFF reader data has no source unit
declaration: numeric world coordinates remain unchanged and `INSUNITS` is 0 (unitless).

Surfaces become triangular `3DFACE` entities in model space. Nodes become distinct layers
with indexed ASCII names, sanitized to DWG layer rules. Layer assignment conveys node
membership; DWG layers do not reproduce the source hierarchy. CIFF nodes carry no hidden
flag. Mesh material RGB is multiplied by the first triangle corner's vertex color when
present, then approximated with the ACI palette. Display colors can depend on the
consumer's palette and background. Transparency, textures, color gradients and smooth
shading are not preserved. RGB reduction and transparency produce warnings.

Source point-only mesh records are counted by `ReadCIFF` and omitted with a warning.
Type-23 empty placeholders carry no geometry. Nonempty point meshes passed directly to
the adapter, unsupported geometry types, invalid indices and nonfinite positions fail
before publication. A model with only empty or omitted geometry produces an empty
drawing. Existing output is replaced atomically after serialization and validation
succeed. The first version holds faces and output bytes in memory and supports DWG
section addresses below 4 GiB.

The serializer and palette mapping were reused from the approved local
`AutodeskFBX/ConvertDWG.h`, adapted to `ciff::dwg::write` to avoid cross-library symbol
collisions. That independently authored serializer follows the Open Design Specification
for .dwg files v5.4.1, sections 2, 3, 9, 20.4.1, 20.4.32, 20.4.52–54, 23.1, 26 and 27
([specification](https://www.opendesign.com/files/guestdownloads/OpenDesign_Specification_for_.dwg_files.pdf),
accessed 2026-10-09), cross-checked with the approved local `AutocadDWG` reader.
No external serializer implementation was imported. The embedded 3183-byte empty seed
was generated during development by unmodified LibreDWG 0.14.8597, commit
`34f02f54b9aacb5708c1d3d2070efb3e4b2d8c43`; it is data, not a linked runtime dependency.
The seed has SHA-256
`24169190ADE41108393169904C06B02353E7F98D04B450B5E52C19BB76528682`.

`CiffDwgExport` exercises numeric world coordinates, reflected triangle winding,
parametric box placement at large fractional coordinates, mirrored analytic dimensions,
vertex/material colors, layers, malformed geometry rejection,
empty output and target preservation. The generated
`ciff-dwg-regression/mirrored-red.dwg` fixture permits independent reader verification.
