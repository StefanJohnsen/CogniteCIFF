# CIFF to Falcon 3D Async performance

Measured on 10 October 2026 with the native Windows `Release|x64`
`CogniteCIFF.exe` and `ConverterBenchmark.exe`. Async prepares independent
CIFF geometry on worker threads; it preserves the serial writer's geometry,
instances, hierarchy, materials and form sharing.

## Corpus

Five real CIFF models were generated through the native CadCast executable from
the existing FBX/RVM files in `C:\3D`. An independently authored native primitive
file and the existing `C:\3D\as1-oc-214.ciff` complete the seven-file corpus.

Generated CIFF files are available locally under
`build/ciff-async-3d-20261010/performance/sources/`. The binary corpus and generated
outputs are excluded from Git. The largest CIFF is 1,913,236,547 bytes (1.782 GiB).

| CIFF file | Source | CIFF size |
|---|---|---:|
| `as1-oc-214.ciff` | Existing CIFF in `C:\3D` | 104,469 bytes |
| `Huldra-above-water.ciff` | `Huldra from scratch above water.rvm` | 12,229,493 bytes |
| `Ship.ciff` | `Ship.FBX` | 46,097,617 bytes |
| `FalconGasPlant-Infrastructure.ciff` | `FalconGasPlant-Infrastructure.fbx` | 165,131,432 bytes |
| `IvarAasen-Cellar-Deck.ciff` | `IvarAasen-Cellar-Deck.rvm` | 171,285,552 bytes |
| `99_NOA_PDQ_Special_optimized.ciff` | `Test2\99_NOA_PDQ_Special_optimized.fbx` | 1,913,236,547 bytes |
| `native-primitives.ciff` | Independent CIFF v4 fixture author | 344,922 bytes |

The primitive fixture contains 600 geometry-bearing nodes plus a root, and
3,600 geometry records, including
distinct torus and general-cylinder ratios, shared boxes/spheres, dishes,
transforms and material variants. FBX imports exercise mesh preparation; the
independent fixture ensures native CIFF primitive preparation is covered.

## Measurements

The test system has an Intel Core i7-10850H (6 physical cores, 12 logical
processors) and 16 GiB installed memory. Automatic Async selection uses five
workers on this machine. Each conversion runs as a separate process, with only
one converter running at a time. No builds or other test converters ran during
these measurements.

Elapsed time includes process initialization, CIFF loading and final output
writing. Peak RAM is the converter's Windows `PeakWorkingSetSize`, including
all its threads. Queue reservations limit estimated temporary geometry to
64 MiB and at most 64 outstanding jobs; the loaded scene, writer catalogs,
current coordinator mesh and thread stacks are additional. A job whose estimate
exceeds the reservation budget runs alone, so 64 MiB is not a process memory cap.

Other user applications remained open. The reports recorded approximately
0.88 GiB available physical memory for the smaller corpus, 1.17 GiB for the
Ship retest, 4.85 GiB for the initial largest-model pair and 3.91 GiB for its retest.
A separate sample before
the Ship retest recorded 1,139 MiB available, 1,091 page reads/second and
15,325 pages input/second. These conditions can affect elapsed time and resident
memory; the measurements do not establish performance on an idle machine.

The six smaller files were run three times per mode, Sync followed by Async.
Reported times are medians. Ship and the largest model also have reverse-order
retests, described below.

| CIFF file | Sync seconds | Async seconds | Time reduction | Sync peak RAM | Async peak RAM |
|---|---:|---:|---:|---:|---:|
| `as1-oc-214` | 0.063 | 0.059 | 6.4% | 128.5 MiB | 129.2 MiB |
| `Huldra-above-water` | 0.347 | 0.209 | 39.8% | 147.3 MiB | 154.6 MiB |
| `FalconGasPlant-Infrastructure` | 9.182 | 4.961 | 46.0% | 407.4 MiB | 409.6 MiB |
| `IvarAasen-Cellar-Deck` | 4.388 | 2.571 | 41.4% | 360.4 MiB | 367.5 MiB |
| `native-primitives` | 0.153 | 0.117 | 23.1% | 129.1 MiB | 131.1 MiB |
| `Ship` (isolated reverse-order retest) | 4.294 | 3.922 | 8.7% | 398.2 MiB | 395.1 MiB |
| `99_NOA_PDQ_Special_optimized` (reverse-order retest, two runs/mode) | 68.706 | 25.771 | 62.5% | 3.1 GiB | 3.1 GiB |

The four-millisecond difference on the tiny `as1` file is within ordinary
measurement variation and does not establish a meaningful speedup. Huldra,
Infrastructure, Ivar Aasen and the native
primitive fixture show gains across all three repetitions in their initial
comparison.

### Ship variability

The initial Ship comparison showed a regression: Sync runs were 4.000, 7.275
and 10.845 seconds; Async runs were 12.704, 13.397 and 9.990 seconds. The initial
medians were therefore 7.275 and 12.704 seconds. The unchanged binary was then
tested alone with Async first: Async runs were 4.061, 3.922 and 3.704 seconds,
and Sync runs were 4.041, 4.693 and 4.294 seconds.

The changing Sync times and observed paging suggest an environmental effect;
they do not identify its cause conclusively. Both samples are retained, and the
8.7% retest gain should be read with this variability in mind. Ship has 85 source
meshes; six exceed the queue's reservation budget and run individually. Its two
largest meshes contain approximately half its triangles, limiting parallelism.

### Largest model

The initial single pair took 75.415 seconds in Sync and 21.251 seconds in Async,
a 71.8% time reduction. Peak resident memory was 2.6 and 3.1 GiB respectively.
The reverse-order retest gave Async runs of 29.519 and 22.023 seconds, followed
by Sync runs of 81.231 and 56.180 seconds. The two-run medians were 25.771 and
68.706 seconds: a 62.5% time reduction (2.67x speedup). Peak resident memory was
approximately 3.1 GiB in both modes in this retest. Async was faster in all
three measured runs, including when it ran first, but the spread and limited
repetition count constrain precision. Both initial and retest reports are retained.

## Validation

All seven final Sync/Async output pairs have identical SHA256 values; the
Ship and largest-model retest outputs also match their initial outputs. All
seven CIFF inputs and all five original FBX/RVM sources retain their recorded
SHA256 values. Input and output sizes and hashes are checked in as
[`benchmarks/async-3d-corpus-hashes.json`](../benchmarks/async-3d-corpus-hashes.json).
The native Release CadCast executable was also run on the real Huldra CIFF
with its default Async selection and with `-sync`; both outputs match the
direct converter's output SHA256.

Native Visual Studio Release builds completed for CogniteCIFF and CadCast;
the separate CMake Release builds also completed.

All four CogniteCIFF CTests passed: `CiffAsync3D`, `CiffDwgExport`,
`CiffAsync3DCli` and `CiffDwgCli`. CadCast's `CommandLine` and `AllFormats`
tests passed, including all 77 source/target pairs and CIFF Async selection by
default with explicit `-sync` override.

The independent CIFF regression fixtures cover meshes and all six native
primitives: box, cylinder, torus, sphere, dish and general cylinder. Forced
three-worker tests verify participation by multiple workers, exact serial
geometry and statistics, repeated byte-identical exports, normal reuse for
shared forms, bounded outstanding work and oversized-job progress. Tests also
cover degenerate forms, repeated geometry references, worker errors,
cancellation, target preservation and cleanup after failed publication.
Automatic fallback is checked for a tiny scene and 1,000 instances of a single
box or a single heavy torus. CLI coverage includes aliases, format selection,
batch conversion, invalid sources and source preservation.

## Reproduce

Build the test fixture author and run the CIFF tests from the repository root:

```powershell
cmake -S . -B build/ciff-async-3d-tests-20261010 -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build/ciff-async-3d-tests-20261010 --config Release --parallel 2
ctest --test-dir build/ciff-async-3d-tests-20261010 -C Release --output-on-failure
```

Python 3 enables the two CLI tests. Build the native CadCast and CogniteCIFF
Release executables before generating or benchmarking the real corpus.

```powershell
./benchmarks/GenerateAsync3DCorpus.ps1 -ModelDirectory C:\3D
```

The generator requires a fresh output directory unless `-Force` is supplied.
`-OutputDirectory`, `-CadCastExecutable` and `-FixtureExecutable` override its
defaults. It verifies each original source's SHA256 before and after conversion
and writes a manifest with the generated CIFF sizes and hashes. The independent
fixture can also be authored directly with
`CiffAsync3D.exe --primitive-stress native-primitives.ciff`.

ConverterBenchmark configurations are checked in under `benchmarks/`:

- `async-3d-corpus-repeated.txt`: three runs of each smaller file and mode.
- `async-3d-ship-recheck.txt`: three runs of Ship, Async first.
- `async-3d-corpus-large.txt`: the initial single pair for the largest file.
- `async-3d-corpus-large-recheck.txt`: two runs per mode, Async first.

The configurations use this workspace's absolute paths. Update them for another
checkout. Copy a configuration into the ignored performance directory before
running it to keep generated reports and logs out of Git:

```powershell
Copy-Item benchmarks/async-3d-corpus-repeated.txt build/ciff-async-3d-20261010/performance/corpus-repeated.txt
../ConverterBenchmark/x64/Release/ConverterBenchmark.exe build/ciff-async-3d-20261010/performance/corpus-repeated.txt
```

Raw reports, individual converter logs, source snapshots and generated outputs
remain in `build/ciff-async-3d-20261010/performance/`.
