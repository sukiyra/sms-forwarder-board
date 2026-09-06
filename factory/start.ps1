$ErrorActionPreference = 'Stop'
$factoryRoot = $PSScriptRoot
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$OutputEncoding = [Text.UTF8Encoding]::new($false)

py -3 -c "import serial" 2>$null
if ($LASTEXITCODE -ne 0) {
  Write-Host '首次运行，正在安装 pyserial...'
  py -3 -m pip install --user -r (Join-Path $factoryRoot 'requirements.txt')
  if ($LASTEXITCODE -ne 0) { throw 'pyserial 安装失败。' }
}

py -3 (Join-Path $factoryRoot 'production_tool.py') @args
exit $LASTEXITCODE
