[CmdletBinding()]
param(
    [ValidateSet('owner','private-beta')]
    [string] $Profile = 'owner'
)

$ErrorActionPreference = 'Stop'
$sourceDir = Split-Path -Parent $PSScriptRoot
$profiles = @{
    owner = @{ Label='Owner' }
    'private-beta' = @{ Label='Private-Beta' }
}
$profileInfo = $profiles[$Profile]
$presetName = "windows-$Profile"
$presets = Get-Content -LiteralPath (Join-Path $sourceDir 'CMakePresets.json') -Raw | ConvertFrom-Json
$preset = $presets.configurePresets | Where-Object { $_.name -eq $presetName } | Select-Object -First 1
if (-not $preset) { throw "Configure preset '$presetName' was not found." }
$version = [string] $preset.cacheVariables.RS_BUILD_VERSION
$channel = [string] $preset.cacheVariables.RS_BUILD_CHANNEL
$expiryDate = [string] $preset.cacheVariables.RS_BUILD_EXPIRY_DATE
$expiryDisplay = [string] $preset.cacheVariables.RS_BUILD_EXPIRY_DISPLAY
$expiryEnabled = if ($preset.cacheVariables.RS_BUILD_EXPIRY_ENABLED) { 'ON' } else { 'OFF' }
if ([string]::IsNullOrWhiteSpace($version) -or [string]::IsNullOrWhiteSpace($channel)) {
    throw "Configure preset '$presetName' must define RS_BUILD_VERSION and RS_BUILD_CHANNEL."
}
$artifactRoot = Join-Path $sourceDir "artifacts\$Profile\$version"
$presetArtifactRoot = [IO.Path]::GetFullPath(([string] $preset.cacheVariables.RS_ARTIFACT_DIR).Replace('${sourceDir}', $sourceDir))
if ($presetArtifactRoot -ne [IO.Path]::GetFullPath($artifactRoot)) {
    throw "Configure preset '$presetName' must identify artifact directory '$artifactRoot'."
}
$appRoot = Join-Path $artifactRoot 'app'
$installerRoot = Join-Path $artifactRoot 'installer'
$prerequisiteRoot = Join-Path $sourceDir '.deps\installer-prerequisites'

$prerequisites = @(
    @{ Name='Microsoft Edge WebView2 Runtime (x64)'; Uri='https://go.microsoft.com/fwlink/?linkid=2124701'; File='MicrosoftEdgeWebView2RuntimeInstallerX64.exe' },
    @{ Name='Microsoft Visual C++ Redistributable (x64)'; Uri='https://aka.ms/vs/17/release/vc_redist.x64.exe'; File='vc_redist.x64.exe' }
)

New-Item -ItemType Directory -Path $prerequisiteRoot -Force | Out-Null
foreach ($prerequisite in $prerequisites) {
    $destination = Join-Path $prerequisiteRoot $prerequisite.File
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        $temporary = "$destination.download"
        try {
            Invoke-WebRequest -Uri $prerequisite.Uri -OutFile $temporary -UseBasicParsing
            Move-Item -LiteralPath $temporary -Destination $destination -Force
        } finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        }
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $destination
    if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
        $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
        throw "$($prerequisite.Name) is not validly signed by Microsoft: $destination"
    }
}

$runtimeFiles = @(
    'RearSilver Avatar Suite.exe',
    'RearSilver-Avatar-Suite-Updater.exe',
    'WebView2Loader.dll',
    'avatar-settings.html',
    'spout2-license.txt',
    'settings-header.png',
    'settings-badge.png',
    'splash.png',
    'Sora-Variable.ttf',
    'default-avatar-idle.png',
    'default-avatar-reaction.png',
    'rail-presets.png',
    'rail-reactions-on.png',
    'rail-reactions-off.png',
    'rail-websocket.png',
    'rail-background.png',
    'rail-tools.png',
    'rail-settings.png'
)
foreach ($runtimeFile in $runtimeFiles) {
    $source = Join-Path $appRoot $runtimeFile
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Required staged runtime file is missing. Run the '$presetName' configure and build presets first: $source"
    }
}
$builtInSource = Join-Path $appRoot 'built-in-layers'
$expectedBuiltInCount = (Get-ChildItem -LiteralPath (Join-Path $sourceDir 'assets\Built In Layers') -Filter '*.png' -File).Count
if (-not (Test-Path -LiteralPath $builtInSource -PathType Container)) {
    throw "Built-in layer directory is missing from the clean build: $builtInSource"
}
$builtInFiles = Get-ChildItem -LiteralPath $builtInSource -Filter '*.png' -File
if ($builtInFiles.Count -ne $expectedBuiltInCount -or $builtInFiles.Count -eq 0) {
    throw "Built-in layer payload is incomplete. Expected $expectedBuiltInCount PNG files, found $($builtInFiles.Count)."
}
$stagedFiles = Get-ChildItem -LiteralPath $appRoot -File -Recurse
if ($stagedFiles.Count -ne ($runtimeFiles.Count + $expectedBuiltInCount)) {
    throw "Staged payload contains an unexpected number of files: $($stagedFiles.Count)."
}

$makensis = @(
    (Get-Command makensis.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
    "$env:ProgramFiles\NSIS\makensis.exe",
    "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -Unique -First 1
if (-not $makensis) { throw 'NSIS 3.x was not found.' }

$vcRuntimeInfo = (Get-Item -LiteralPath (Join-Path $prerequisiteRoot 'vc_redist.x64.exe')).VersionInfo
$vcRuntimeVersion = if ($vcRuntimeInfo.FileVersionRaw) { $vcRuntimeInfo.FileVersionRaw.ToString() } else { '0' }
if ($vcRuntimeVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') { $vcRuntimeVersion = '0' }
$outputFile = Join-Path $installerRoot "RearSilver-Avatar-Suite-$($profileInfo.Label)-$version-Setup.exe"
New-Item -ItemType Directory -Path $installerRoot -Force | Out-Null

Push-Location -LiteralPath $sourceDir
try {
    & $makensis /NOCD "/DRS_ARTIFACT_ROOT=$artifactRoot" "/DRS_PREREQUISITE_ROOT=$prerequisiteRoot" "/DRS_VERSION=$version" "/DRS_CHANNEL=$channel" "/DRS_OUTPUT_FILE=$outputFile" "/DRS_VC_RUNTIME_MIN_VERSION=$vcRuntimeVersion" "/DRS_EXPIRY_ENABLED=$expiryEnabled" "/DRS_EXPIRY_DISPLAY=$expiryDisplay" (Join-Path $sourceDir 'installer.nsi')
    if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }
} finally {
    Pop-Location
}

Write-Output "$channel installer created: $outputFile"
