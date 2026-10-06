param(
    [string]$InstallRoot = $PSScriptRoot
)

$ErrorActionPreference = "Stop"
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$remainingEntries = @()
foreach ($entry in ($userPath -split ";" | Where-Object { $_ })) {
    $normalizedEntry = [System.IO.Path]::GetFullPath($entry.Trim().Trim('"')).TrimEnd('\')
    if (-not [string]::Equals(
        $normalizedEntry,
        $InstallRoot.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase
    )) {
        $remainingEntries += $entry
    }
}
[Environment]::SetEnvironmentVariable("Path", ($remainingEntries -join ";"), "User")

foreach ($name in @("chia.exe", "CHIA.md")) {
    $file = Join-Path $InstallRoot $name
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        Remove-Item -LiteralPath $file -Force
    }
}

$example = Join-Path $InstallRoot "examples\Hello.chia"
if (Test-Path -LiteralPath $example -PathType Leaf) {
    Remove-Item -LiteralPath $example -Force
}
$examplesDirectory = Join-Path $InstallRoot "examples"
$exampleItems = @()
if (Test-Path -LiteralPath $examplesDirectory -PathType Container) {
    $exampleItems = @(Get-ChildItem -LiteralPath $examplesDirectory -Force)
}
if ((Test-Path -LiteralPath $examplesDirectory -PathType Container) -and $exampleItems.Count -eq 0) {
    Remove-Item -LiteralPath $examplesDirectory -Force
}

$uninstaller = Join-Path $InstallRoot "uninstall.ps1"
if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
    Remove-Item -LiteralPath $uninstaller -Force
}
$installItems = @()
if (Test-Path -LiteralPath $InstallRoot -PathType Container) {
    $installItems = @(Get-ChildItem -LiteralPath $InstallRoot -Force)
}
if ((Test-Path -LiteralPath $InstallRoot -PathType Container) -and $installItems.Count -eq 0) {
    Remove-Item -LiteralPath $InstallRoot -Force
}

Write-Output "Chia was uninstalled from $InstallRoot. User files were left untouched."
