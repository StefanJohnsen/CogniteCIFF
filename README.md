# CogniteCIFF

Converter for the Cognite Reveal **CIFF** format. Parses a `.ciff` file and
emits one of:

| Target | Extension | Status |
|--------|-----------|--------|
| Cognite CIFF (clone) | `.ciff` | Full re-emit using the same writer as `AutodeskFBX` |
| Autodesk FBX        | `.fbx`  | Working (mesh normals and materials) |
| AVEVA RVM           | `.rvm`  | Working (flat facet normals) |
| FalconCoding 3D    | `.3d`  | Working (instanced meshes with normals) |
| Navisworks NWD      | `.nwd`  | Working (hierarchy, normals and materials) |
| Khronos glTF        | `.gltf` | Working (positions, normals and materials) |
| Wavefront OBJ       | `.obj`  | Working (positions, normals and faces) |
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

### Build a Linux executable with VS Code and WSL 2

These steps build the Linux version. Do not open the Visual Studio solution for
this build.

1. Open **Visual Studio Code** in Windows.
2. Press `Ctrl+Shift+P` to open the **Command Palette**.
3. Type `WSL: Connect to WSL using Distro...` and select it.
   If this command is not shown, press `Ctrl+Shift+X`, install the Microsoft
   **WSL** extension, and repeat step 2.
4. Select **Ubuntu**. A new VS Code window opens.
5. In the new window, press `Ctrl+Shift+P` again.
6. Type `File: Open Folder...` and select it.
7. Enter this folder and click **OK**:

   ```text
   /mnt/c/FalconCoding/CogniteCIFF
   ```

8. Check the lower-left corner of VS Code. It must say `WSL: Ubuntu`.
9. On a new Ubuntu installation, select **Terminal > New Terminal** and run:

   ```bash
   sudo apt update
   sudo apt install build-essential cmake ninja-build
   ```

   This only needs to be done once.
10. If VS Code recommends extensions, install **C/C++** and **CMake Tools** in
    `WSL: Ubuntu`.
11. Press `Ctrl+Shift+P`, run `CMake: Select Configure Preset`, and select
    **Linux Release**.
12. Press `Ctrl+Shift+P`, run `CMake: Configure`, and wait until it completes.
13. Press `Ctrl+Shift+P`, run `CMake: Build`, and wait until it completes.

The build is successful when the CMake output ends with exit code `0`. The
Linux executable is created here:

```text
C:\FalconCoding\CogniteCIFF\build\linux-release\CogniteCIFF
```

The same file has this path inside WSL:

```text
/mnt/c/FalconCoding/CogniteCIFF/build/linux-release/CogniteCIFF
```

The file is named `CogniteCIFF`, without `.exe`, because it is a Linux program.
It must be run in Linux or WSL, not directly in Windows.

To verify the file type, select **Terminal > New Terminal** in the WSL window
and run:

```bash
file build/linux-release/CogniteCIFF
```

The result must begin with `ELF 64-bit`.

#### Optional: build from the WSL terminal

The steps above are enough. These commands perform the same build without the
VS Code menus:

```bash
cd /mnt/c/FalconCoding/CogniteCIFF
cmake --preset linux-release
cmake --build --preset linux-release
```


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
`WriteBuffer.h`, `StreamFile.h`, `Util.h`, `Constants.h`, `ConversionOutput.h`)
are copied verbatim from `AutodeskFBX`.

## Notes

- The CIFF reader follows the format produced by `ConvertCIFF.h` in the
  `AutodeskFBX` repository (magic `0x46443343`, version 4, record types
  `1`, `3`, `19`, `23`, footer `0`).
- CIFF has no vertex-normal stream. `MeshNormals.h` exposes `FinalizeMeshNormals`
  to finalize triangle meshes before exporting to formats that support normals. It uses the common
  final-mesh policy: `1e-6` position welding, a 60-degree crease threshold,
  smoothing only across two-use manifold edges, corner-angle weighting and
  deterministic vertex splitting. Degenerate faces are omitted.
- SceneData, FalconCoding 3D, glTF, OBJ and FBX exports use that same finalized
  position/normal/index stream. RVM remains a format-specific exception: each
  facet polygon stores its flat face normal. The CIFF clone and raw DAT formats
  remain unchanged because their current schemas do not contain normals.
- FalconCoding 3D forms store one normal per point and use an FNV-1a hash over
  the exact serialized point count, positions, normals, triangle count and indices.
