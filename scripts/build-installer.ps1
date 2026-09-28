[CmdletBinding()]
param(
    [ValidateSet('owner')]
    [string] $Profile = 'owner'
)

$ErrorActionPreference = 'Stop'
$sourceDir = Split-Path -Parent $PSScriptRoot
$version = '1.0.0-owner.1'
$channel = 'Owner Build'
$artifactRoot = Join-Path $sourceDir "artifacts\owner\$version"
$appRoot = Join-Path $artifactRoot 'app'
$installerRoot = Join-Path $artifactRoot 'installer'
$buildRoot = Join-Path $sourceDir 'out\build\x64-Owner'
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

$developerCommandPrompt = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $developerCommandPrompt)) { throw "Visual Studio developer prompt was not found: $developerCommandPrompt" }
$buildScript = Join-Path $env:TEMP 'rearsilver-avatar-owner-build.cmd'
Set-Content -LiteralPath $buildScript -Encoding ascii -Value @(
    "@call `"$developerCommandPrompt`"",
    "@cmake -S `"$sourceDir`" -B `"$buildRoot`" -G Ninja -DCMAKE_BUILD_TYPE=Release",
    '@if errorlevel 1 exit /b %errorlevel%',
    "@cmake --build `"$buildRoot`" --target RearSilverAvatarSuite",
    '@exit /b %errorlevel%'
)
try {
    & $buildScript
    if ($LASTEXITCODE -ne 0) { throw "Avatar Suite Owner build failed with exit code $LASTEXITCODE." }
} finally {
    Remove-Item -LiteralPath $buildScript -Force -ErrorAction SilentlyContinue
}

if (Test-Path -LiteralPath $artifactRoot) { Remove-Item -LiteralPath $artifactRoot -Recurse -Force }
New-Item -ItemType Directory -Path $appRoot, $installerRoot -Force | Out-Null
$runtimeExtensions = @('.exe','.dll','.html','.txt','.png','.ttf')
Get-ChildItem -LiteralPath $buildRoot -File | Where-Object { $runtimeExtensions -contains $_.Extension.ToLowerInvariant() } |
    Where-Object { $_.Name -ne 'CMakeCache.txt' } |
    Copy-Item -Destination $appRoot
$requiredExecutable = Join-Path $appRoot 'RearSilver Avatar Suite.exe'
if (-not (Test-Path -LiteralPath $requiredExecutable -PathType Leaf)) { throw "Staged executable is missing: $requiredExecutable" }

$makensis = @(
    (Get-Command makensis.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1),
    "$env:ProgramFiles\NSIS\makensis.exe",
    "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -Unique -First 1
if (-not $makensis) { throw 'NSIS 3.x was not found.' }

$vcRuntimeInfo = (Get-Item -LiteralPath (Join-Path $prerequisiteRoot 'vc_redist.x64.exe')).VersionInfo
$vcRuntimeVersion = if ($vcRuntimeInfo.FileVersionRaw) { $vcRuntimeInfo.FileVersionRaw.ToString() } else { '0' }
if ($vcRuntimeVersion -notmatch '^\d+\.\d+\.\d+\.\d+$') { $vcRuntimeVersion = '0' }
$outputFile = Join-Path $installerRoot "RearSilver-Avatar-Suite-Owner-$version-Setup.exe"

Push-Location -LiteralPath $sourceDir
try {
    & $makensis /NOCD "/DRS_ARTIFACT_ROOT=$artifactRoot" "/DRS_PREREQUISITE_ROOT=$prerequisiteRoot" "/DRS_VERSION=$version" "/DRS_CHANNEL=$channel" "/DRS_OUTPUT_FILE=$outputFile" "/DRS_VC_RUNTIME_MIN_VERSION=$vcRuntimeVersion" (Join-Path $sourceDir 'installer.nsi')
    if ($LASTEXITCODE -ne 0) { throw "NSIS failed with exit code $LASTEXITCODE." }
} finally {
    Pop-Location
}

Write-Output "Owner installer created: $outputFile"
