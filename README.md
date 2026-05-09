# CogniteCIFF

Converter for the Cognite Reveal **CIFF** format. Parses a `.ciff` file and
emits one of:

| Target | Extension | Status |
|--------|-----------|--------|
| Cognite CIFF (clone) | `.ciff` | Full re-emit using the same writer as `AutodeskFBX` |
| Autodesk FBX        | `.fbx`  | Scaffold (ASCII placeholder) |
| AVEVA RVM           | `.rvm`  | Scaffold (text placeholder) |
| FalconCoding 3D    | `.3d`  | Scaffold |
| Khronos glTF        | `.gltf` | Scaffold (minimal JSON) |
| Wavefront OBJ       | `.obj`  | Working (vertices + faces) |
| Hierarchy text      | `.txt`  | Working |
| Hierarchy JSON      | `.json` | Working |
| Raw geometry data   | `.dat`  | Working |

The project structure mirrors the `AutodeskFBX` and `AvevaRvmDebug`
repositories: header-only converters in `Convert*.h`, format-specific
reader in `ReadCIFF.h` / `StreamCIFF.h`, primitives in `PrimitivesCIFF.h`
and a shared `Convert` base class in `Convert.h`.

## Build

Open `CogniteCIFF.sln` in Visual Studio 2022 (toolset `v143`, C++20) and
build the `x64` configuration of choice.

## Usage

```
CogniteCIFF <source.ciff> [target_file_or_directory] [options]
```

Run `CogniteCIFF -help` for the full list of options. The same flags as
in the sibling repositories are supported (`-bar`, `-statistics`,
`-format`, `-speedtest`, ...).

## Layout

Files map one-to-one to their counterparts in `AutodeskFBX`:

| AutodeskFBX            | CogniteCIFF            |
|------------------------|------------------------|
| `ReadFBX.h`            | `ReadCIFF.h`           |
| `StreamFBX.h`          | `StreamCIFF.h`         |
| `PrimitivesFBX.h`      | `PrimitivesCIFF.h`     |
| `ProcessFBX.h`         | `ProcessCIFF.h`        |
| `Convert.h` (`fbx::`)  | `Convert.h` (`ciff::`) |

All other utility files (`CmdArgs.h`, `CmdBar.h`, `CmdTimer.h`,
`WriteBuffer.h`, `StreamFile.h`, `Util.h`, `Constants.h`, `TempFile.h`)
are copied verbatim from `AutodeskFBX`.

## Notes

- The CIFF reader follows the format produced by `ConvertCIFF.h` in the
  `AutodeskFBX` repository (magic `0x46443343`, version 4, record types
  `1`, `3`, `19`, `23`, footer `0`).
- The FBX, RVM, glTF and 3D writers are scaffolds. They produce valid
  but minimal output so the full pipeline compiles and runs end-to-end.
  Replace them with the full implementations from `AutodeskFBX` /
  `AvevaRvmDebug` when needed.
