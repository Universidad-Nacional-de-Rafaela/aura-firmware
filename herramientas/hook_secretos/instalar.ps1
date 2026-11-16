# Instala la revisión de secretos de AURA en todos los repos git de esta PC (Windows).
#   powershell -ExecutionPolicy Bypass -File instalar.ps1                 # instala o actualiza
#   powershell -ExecutionPolicy Bypass -File instalar.ps1 -Desinstalar
param([switch]$Desinstalar)
$ErrorActionPreference = "Stop"

$origen = $PSScriptRoot
$destino = Join-Path $env:USERPROFILE ".aura\git-hooks"
# git prefiere barras comunes, también en Windows.
$destinoGit = $destino -replace '\\', '/'

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "No encuentro git. Instalá Git para Windows y volvé a correr este script."
}

$actual = (git config --global --get core.hooksPath) 2>$null
if ($actual) { $actual = $actual.Trim() }

if ($Desinstalar) {
    if ($actual -eq $destinoGit) { git config --global --unset core.hooksPath }
    if (Test-Path $destino) { Remove-Item -Recurse -Force $destino }
    Write-Host "Listo: se quitó la revisión de secretos de AURA."
    exit 0
}

if ($actual -and $actual -ne $destinoGit) {
    Write-Host "Ya tenés hooks globales en: $actual"
    Write-Host "No los piso. Consultá a la cátedra para combinarlos."
    exit 1
}

# Python 3: el hook prueba "py -3", "python3" y "python", en ese orden.
$hayPython = $false
foreach ($cmd in @(@("py", "-3"), @("python3"), @("python"))) {
    $exe = $cmd[0]
    $args_ = @($cmd | Select-Object -Skip 1) + @("-c", "import sys; sys.exit(sys.version_info < (3, 8))")
    if (Get-Command $exe -ErrorAction SilentlyContinue) {
        & $exe @args_ 2>$null
        if ($LASTEXITCODE -eq 0) { $hayPython = $true; break }
    }
}
if (-not $hayPython) {
    Write-Error "Hace falta Python 3.8 o más nuevo (viene con ESP-IDF, o desde python.org)."
}

New-Item -ItemType Directory -Force -Path $destino | Out-Null
# Los hooks los corre el sh de Git para Windows: tienen que quedar con fin de línea LF, aunque
# el repo se haya clonado con CRLF.
foreach ($f in @("pre-commit", "pre-push", "comun.sh", "revisar_secretos.py")) {
    $texto = [IO.File]::ReadAllText((Join-Path $origen $f)) -replace "`r`n", "`n"
    [IO.File]::WriteAllText((Join-Path $destino $f), $texto, (New-Object Text.UTF8Encoding($false)))
}
git config --global core.hooksPath $destinoGit

Write-Host "Listo: desde ahora git revisa cada commit y cada push, en todos tus repos."
Write-Host "Hooks en: $destino"
