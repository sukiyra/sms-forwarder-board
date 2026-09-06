param(
  [string]$ArduinoCli = '',
  [string]$ConfigFile = ''
)

$ErrorActionPreference = 'Stop'
$factoryRoot = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $factoryRoot '..')).Path
$workspaceRoot = Split-Path $repoRoot -Parent
$buildDir = Join-Path $factoryRoot '.build'
$distDir = Join-Path $factoryRoot 'dist'

if (-not $ArduinoCli) {
  $candidates = @(
    (Join-Path $workspaceRoot 'tools\arduino-cli\arduino-cli.exe'),
    (Join-Path $repoRoot 'tools\arduino-cli\arduino-cli.exe')
  )
  $ArduinoCli = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
  if (-not $ArduinoCli) {
    $command = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($command) { $ArduinoCli = $command.Source }
  }
}
if (-not $ArduinoCli -or -not (Test-Path -LiteralPath $ArduinoCli)) {
  throw '找不到 arduino-cli，请用 -ArduinoCli 指定路径。'
}
if (-not $ConfigFile) {
  $candidate = Join-Path $workspaceRoot 'arduino-cli.yaml'
  if (Test-Path -LiteralPath $candidate) { $ConfigFile = $candidate }
}

New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null
$arguments = @(
  'compile', '--fqbn', 'esp32:esp32:makergo_c3_supermini',
  '--board-options', 'PartitionScheme=no_ota',
  '--output-dir', $buildDir
)
if ($ConfigFile) { $arguments += @('--config-file', $ConfigFile) }
$arguments += (Join-Path $repoRoot 'code')

Write-Host '正在编译量产固件...'
& $ArduinoCli @arguments
if ($LASTEXITCODE -ne 0) { throw "Arduino 编译失败：$LASTEXITCODE" }

py -3 (Join-Path $factoryRoot 'make_manifest.py') $buildDir $distDir --repo $repoRoot
if ($LASTEXITCODE -ne 0) { throw "生成量产包失败：$LASTEXITCODE" }
Write-Host "量产固件已生成：$distDir"
