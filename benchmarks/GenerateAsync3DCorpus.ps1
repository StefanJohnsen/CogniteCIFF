param(
    [string]$ModelDirectory = 'C:\3D',
    [string]$OutputDirectory,
    [string]$CadCastExecutable,
    [string]$FixtureExecutable,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ciffRepository = Split-Path -Parent $PSScriptRoot
$ciffWorkspace = Split-Path -Parent $ciffRepository
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $ciffRepository 'build\ciff-async-3d-20261010\performance\sources'
}
if (-not $CadCastExecutable) {
    $CadCastExecutable = Join-Path $ciffWorkspace 'CadCast\CadCast\x64\Release\CadCast.exe'
}
if (-not $FixtureExecutable) {
    $FixtureExecutable = Join-Path $ciffRepository 'build\ciff-async-3d-tests-20261010\Release\CiffAsync3D.exe'
}

$ciffModels = @(
    @{Source = 'Ship.FBX'; Target = 'Ship.ciff'},
    @{Source = 'FalconGasPlant-Infrastructure.fbx'; Target = 'FalconGasPlant-Infrastructure.ciff'},
    @{Source = 'Huldra from scratch above water.rvm'; Target = 'Huldra-above-water.ciff'},
    @{Source = 'IvarAasen-Cellar-Deck.rvm'; Target = 'IvarAasen-Cellar-Deck.ciff'},
    @{Source = 'Test2\99_NOA_PDQ_Special_optimized.fbx'; Target = '99_NOA_PDQ_Special_optimized.ciff'}
)
foreach ($ciffExecutable in @($CadCastExecutable, $FixtureExecutable)) {
    if (-not (Test-Path -LiteralPath $ciffExecutable -PathType Leaf)) {
        throw "Build the executable before generating the corpus: $ciffExecutable"
    }
}
foreach ($ciffModel in $ciffModels) {
    $ciffInput = Join-Path $ModelDirectory $ciffModel.Source
    if (-not (Test-Path -LiteralPath $ciffInput -PathType Leaf)) {
        throw "Missing source model: $ciffInput"
    }
}
$ciffTargets = @($ciffModels | ForEach-Object { $_.Target }) + @('native-primitives.ciff')
foreach ($ciffTarget in $ciffTargets) {
    $ciffTargetPath = Join-Path $OutputDirectory $ciffTarget
    if ((Test-Path -LiteralPath $ciffTargetPath) -and -not $Force) {
        throw "Target already exists: $ciffTargetPath. Use a fresh directory or -Force."
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$ciffManifest = @()
foreach ($ciffModel in $ciffModels) {
    $ciffInput = Join-Path $ModelDirectory $ciffModel.Source
    $ciffTargetPath = Join-Path $OutputDirectory $ciffModel.Target
    $ciffSourceHash = (Get-FileHash -LiteralPath $ciffInput -Algorithm SHA256).Hash
    & $CadCastExecutable $ciffInput $ciffTargetPath
    if ($LASTEXITCODE -ne 0) { throw "Conversion failed: $ciffInput" }
    if ((Get-FileHash -LiteralPath $ciffInput -Algorithm SHA256).Hash -ne $ciffSourceHash) {
        throw "Original source changed: $ciffInput"
    }
    $ciffResult = Get-Item -LiteralPath $ciffTargetPath
    if ($ciffResult.Length -eq 0) { throw "Empty generated CIFF: $ciffTargetPath" }
    $ciffManifest += [PSCustomObject]@{
        Source = $ciffInput
        SourceSHA256 = $ciffSourceHash
        Output = $ciffResult.FullName
        Bytes = $ciffResult.Length
        SHA256 = (Get-FileHash -LiteralPath $ciffResult.FullName -Algorithm SHA256).Hash
    }
    Write-Output "Created $($ciffModel.Target): $($ciffResult.Length) bytes"
}
$ciffPrimitiveTarget = Join-Path $OutputDirectory 'native-primitives.ciff'
& $FixtureExecutable '--primitive-stress' $ciffPrimitiveTarget
if ($LASTEXITCODE -ne 0) { throw 'Native primitive fixture generation failed' }
$ciffPrimitiveFile = Get-Item -LiteralPath $ciffPrimitiveTarget
$ciffManifest += [PSCustomObject]@{
    Source = 'Independent CIFF primitive fixture'
    SourceSHA256 = $null
    Output = $ciffPrimitiveFile.FullName
    Bytes = $ciffPrimitiveFile.Length
    SHA256 = (Get-FileHash -LiteralPath $ciffPrimitiveFile.FullName -Algorithm SHA256).Hash
}
$ciffManifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'corpus-manifest.json') -Encoding UTF8
