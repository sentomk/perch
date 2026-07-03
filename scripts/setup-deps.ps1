# Initialize bootstrap compiler tree for GN (git submodule + junctions).
# Usage: .\scripts\setup-deps.ps1
#        $env:BOOTSTRAP_ROOT = 'F:\code\KPL\bootstrap'; .\scripts\setup-deps.ps1

$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..')

if (-not $env:BOOTSTRAP_ROOT) {
  git -C $Root submodule update --init third_party/bootstrap
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$Bootstrap = if ($env:BOOTSTRAP_ROOT) { $env:BOOTSTRAP_ROOT } else { Join-Path $Root 'third_party\bootstrap' }
$Bootstrap = (Resolve-Path $Bootstrap).Path

if (-not (Test-Path (Join-Path $Bootstrap 'BUILD.gn'))) {
  Write-Error "bootstrap not found at $Bootstrap"
}

function Set-Junction([string]$Name, [string]$Target) {
  $Path = Join-Path $Root $Name
  if (Test-Path $Path) {
    Remove-Item $Path -Recurse -Force
  }
  New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
  Write-Host "linked $Name -> $Target"
}

New-Item -ItemType Directory -Force -Path (Join-Path $Root 'build') | Out-Null

Set-Junction 'build\\config' (Join-Path $Bootstrap 'build\\config')
Set-Junction 'build\\toolchain' (Join-Path $Bootstrap 'build\\toolchain')
# Symlink individual .gni files that build/config/BUILD.gn imports.
foreach ($gni in Get-ChildItem (Join-Path $Bootstrap 'build\\*.gni')) {
  $name = $gni.Name
  $path = Join-Path $Root 'build' $name
  if (Test-Path $path) { Remove-Item $path -Force }
  New-Item -ItemType SymbolicLink -Path $path -Target $gni.FullName | Out-Null
  Write-Host "linked build\\$name -> $($gni.FullName)"
}
# Mirror the compiler tree
# refs (deps + include_dirs) resolve from perch's source root.
Set-Junction 'compiler' (Join-Path $Bootstrap 'compiler')

Write-Host "bootstrap ready at $Bootstrap"
