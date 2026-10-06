param(
    [string]$CompilerPath = (Join-Path $PSScriptRoot "dist\chia.exe"),
    [string]$OutputPath = (Join-Path $PSScriptRoot "dist\ChiaSetup.exe")
)

$ErrorActionPreference = "Stop"
$CompilerPath = [System.IO.Path]::GetFullPath($CompilerPath)
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$iexpress = Join-Path $env:WINDIR "System32\iexpress.exe"
$sourceFiles = @(
    $CompilerPath,
    (Join-Path $PSScriptRoot "CHIA.md"),
    (Join-Path $PSScriptRoot "install.ps1"),
    (Join-Path $PSScriptRoot "uninstall.ps1"),
    (Join-Path $PSScriptRoot "examples\Hello.chia")
)

if (-not (Test-Path -LiteralPath $iexpress -PathType Leaf)) {
    throw "Windows IExpress was not found: $iexpress"
}
if (-not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw "Build Chia first; compiler not found: $CompilerPath"
}
foreach ($file in $sourceFiles | Select-Object -Skip 1) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Installer input is missing: $file"
    }
}

$workDirectory = Join-Path $env:TEMP ("chia-installer-" + [Guid]::NewGuid().ToString("N"))
$packageDirectory = Join-Path $workDirectory "package"
$buildDirectory = Join-Path $workDirectory "build"
$null = New-Item -ItemType Directory -Path $packageDirectory, $buildDirectory -Force
$buildSucceeded = $false

try {
    foreach ($file in $sourceFiles) {
        Copy-Item -LiteralPath $file -Destination $packageDirectory
    }
    $sedPath = Join-Path $workDirectory "ChiaSetup.sed"
    $setupPath = Join-Path $buildDirectory "ChiaSetup.exe"
    $sed = @"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=0
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=%InstallPrompt%
DisplayLicense=%DisplayLicense%
FinishMessage=%FinishMessage%
TargetName=build\ChiaSetup.exe
FriendlyName=%FriendlyName%
AppLaunched=%AppLaunched%
PostInstallCmd=%PostInstallCmd%
AdminQuietInstCmd=%AdminQuietInstCmd%
UserQuietInstCmd=%UserQuietInstCmd%
SourceFiles=SourceFiles
[Strings]
InstallPrompt=Install Chia for the current user?
DisplayLicense=
FinishMessage=Chia installation is complete.
FriendlyName=Chia Language Installer
AppLaunched=powershell.exe -NoProfile -ExecutionPolicy Bypass -File install.ps1
PostInstallCmd=<None>
AdminQuietInstCmd=
UserQuietInstCmd=
FILE0="chia.exe"
FILE1="CHIA.md"
FILE2="install.ps1"
FILE3="uninstall.ps1"
FILE4="Hello.chia"
[SourceFiles]
SourceFiles0=package
[SourceFiles0]
%FILE0%=
%FILE1%=
%FILE2%=
%FILE3%=
%FILE4%=
"@
[System.IO.File]::WriteAllText($sedPath, $sed, [System.Text.Encoding]::Default)
Push-Location $workDirectory
try {
    & $iexpress /N (Split-Path -Leaf $sedPath)
}
finally {
    Pop-Location
}

$previousLength = -1L
$stableReads = 0
for ($attempt = 0; $attempt -lt 60 -and $stableReads -lt 3; $attempt++) {
    Start-Sleep -Seconds 1
    if (Test-Path -LiteralPath $setupPath -PathType Leaf) {
        $currentLength = (Get-Item -LiteralPath $setupPath).Length
        if ($currentLength -gt 0 -and $currentLength -eq $previousLength) {
            $stableReads += 1
        }
        else {
            $stableReads = 0
        }
        $previousLength = $currentLength
    }
}
if ($stableReads -lt 3) {
    throw "IExpress did not finish creating the installer: $setupPath"
}

    $outputDirectory = Split-Path -Parent $OutputPath
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    Copy-Item -LiteralPath $setupPath -Destination $OutputPath -Force
    $buildSucceeded = $true
    Write-Output "Created Chia installer: $OutputPath"
}
finally {
    if ($buildSucceeded -and (Test-Path -LiteralPath $workDirectory)) {
        Remove-Item -LiteralPath $workDirectory -Recurse -Force
    }
    elseif (Test-Path -LiteralPath $workDirectory) {
        Write-Output "Installer build files retained for diagnosis: $workDirectory"
    }
}
