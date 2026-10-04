param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [ValidateSet('text','images')][string]$Mode = 'images',
    [switch]$FixedGrid,
    [string]$OutputDir,
    [switch]$Child
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $BuildDir "physical-$Mode.exe"
if ($Child) {
    & $exe $OutputDir 2> (Join-Path $OutputDir 'stderr.txt')
    Set-Content -LiteralPath (Join-Path $OutputDir 'exit.txt') -Value $LASTEXITCODE
    exit
}
if (!$OutputDir) {
    $OutputDir = Join-Path ([IO.Path]::GetTempPath()) ('notcurses-zoom-' + [guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$nativeDir = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$compiler = (Get-Command cc.exe).Source
$includeDir = Join-Path (Split-Path (Split-Path $compiler)) 'include'
& $compiler -DWINPTHREAD_STATIC -I "$nativeDir/include" -I "$nativeDir/src" `
    -I "$BuildDir/include" -I $includeDir -I "$includeDir/ncursesw" `
    "$PSScriptRoot/physical-$Mode.c" -L $BuildDir -lnotcurses-core -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed' }
$env:NC_ZOOM_OUTPUT = $OutputDir.Replace('\','/')
$env:NC_ZOOM_FIXED_GRID = if ($FixedGrid) { '1' } else { '0' }
$shell = (Get-Process -Id $PID).Path
Start-Process -FilePath (Get-Command wezterm.exe).Source -WindowStyle Hidden -ArgumentList @(
    '--config-file', "`"$PSScriptRoot/wezterm.lua`"", 'start', '--always-new-process', '--',
    "`"$shell`"", '-NoProfile', '-File', "`"$PSCommandPath`"", '-Child',
    '-BuildDir', "`"$BuildDir`"", '-Mode', $Mode, '-OutputDir', "`"$OutputDir`""
)
$deadline = [DateTime]::UtcNow.AddSeconds(60)
$exitFile = Join-Path $OutputDir 'exit.txt'
while (!(Test-Path -LiteralPath $exitFile)) {
    if ([DateTime]::UtcNow -gt $deadline) { throw "Fixture timed out; inspect $OutputDir" }
    Start-Sleep -Milliseconds 200
}
$fixtureExit = [int](Get-Content -LiteralPath $exitFile)
if ($fixtureExit -ne 0) { throw "Fixture exited $fixtureExit; inspect $OutputDir" }
$captures = if ($Mode -eq 'text') { 5 } else { 4 }
foreach ($i in 1..$captures) {
    $actual = [IO.File]::ReadAllText((Join-Path $OutputDir "actual-$i.txt")).Replace("`r`n","`n").TrimEnd()
    $expected = [IO.File]::ReadAllText((Join-Path $OutputDir "expected-$i.txt")).Replace("`r`n","`n").TrimEnd()
    if ($actual -cne $expected) { throw "Displayed text differs at capture $i; inspect $OutputDir" }
}
Get-Content -LiteralPath (Join-Path $OutputDir "physical-$Mode.log")
Write-Output "PASS: $captures exact WezTerm text captures. Artifacts: $OutputDir"
