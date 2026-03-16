[CmdletBinding()]
param(
    [string]$BinDir,

    [string]$TitleId = 'NPUB31633',

    [string]$BootPath,

    [string]$ConfigPath,

    [string]$OutputRoot,

    [string]$RunName,

    [switch]$NoGui,

    [switch]$Fullscreen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $BinDir)
{
    $BinDir = Join-Path $PSScriptRoot 'bin'
}

if (-not $OutputRoot)
{
    $OutputRoot = Join-Path $BinDir 'run_logs'
}

function Assert-Directory([string]$Name, [string]$Path)
{
    if (-not (Test-Path $Path))
    {
        throw "$Name path does not exist: $Path"
    }
}

function Copy-IfExists([string]$SourcePath, [string]$DestinationPath)
{
    if (Test-Path $SourcePath)
    {
        Copy-Item -Path $SourcePath -Destination $DestinationPath -Force
    }
}

Assert-Directory -Name 'BinDir' -Path $BinDir

$rpcs3Exe = Join-Path $BinDir 'rpcs3.exe'
if (-not (Test-Path $rpcs3Exe))
{
    throw "Could not find rpcs3.exe at $rpcs3Exe"
}

$normalizedTitleId = $TitleId.Trim().ToUpperInvariant()
if ($normalizedTitleId -notmatch '^[A-Z0-9]{9}$')
{
    throw "Title ID '$TitleId' is not in the expected PS3 title-id format."
}

if (-not $BootPath)
{
    $candidateBootPath = Join-Path $BinDir "dev_hdd0\game\$normalizedTitleId\USRDIR\EBOOT.BIN"
    if (Test-Path $candidateBootPath)
    {
        $BootPath = $candidateBootPath
    }
    else
    {
        $BootPath = "%RPCS3_GAMEID%:$normalizedTitleId"
    }
}

if (-not $ConfigPath)
{
    $candidateConfigPath = Join-Path $BinDir "config\custom_configs\config_$normalizedTitleId.yml"
    if (Test-Path $candidateConfigPath)
    {
        $ConfigPath = $candidateConfigPath
    }
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$runRoot = $OutputRoot
$safeRunName = ''

if ($RunName)
{
    $safeRunName = ($RunName -replace '[^A-Za-z0-9._-]+', '-').Trim('-')
}

if ($safeRunName)
{
    $runDir = Join-Path $runRoot "$timestamp-$safeRunName-$normalizedTitleId"
}
else
{
    $runDir = Join-Path $runRoot "$timestamp-$normalizedTitleId"
}

$null = New-Item -ItemType Directory -Path $runDir -Force

$stdoutPath = Join-Path $runDir 'stdout.log'
$stderrPath = Join-Path $runDir 'stderr.log'
$summaryPath = Join-Path $runDir 'run-summary.txt'

$arguments = New-Object System.Collections.Generic.List[string]
$arguments.Add('--stdout')
$arguments.Add('--stderr')

if ($NoGui)
{
    $arguments.Add('--no-gui')
}

if ($Fullscreen)
{
    $arguments.Add('--fullscreen')
}

if ($ConfigPath)
{
    $arguments.Add('--config')
    $arguments.Add($ConfigPath)
}

$arguments.Add($BootPath)

$summaryLines = @(
    "Run started: $(Get-Date -Format o)",
    "Executable: $rpcs3Exe",
    "BinDir: $BinDir",
    "TitleId: $normalizedTitleId",
    "BootPath: $BootPath",
    "ConfigPath: $ConfigPath",
    "OutputRoot: $OutputRoot",
    "RunName: $safeRunName",
    "Arguments: $($arguments -join ' ')"
)

Set-Content -Path $summaryPath -Value ($summaryLines -join "`r`n") -Encoding UTF8

Write-Host "`n== Launching RPCS3 ==" -ForegroundColor Cyan
Write-Host "Run artifacts: $runDir"

$process = Start-Process -FilePath $rpcs3Exe `
    -ArgumentList $arguments `
    -WorkingDirectory $BinDir `
    -RedirectStandardOutput $stdoutPath `
    -RedirectStandardError $stderrPath `
    -PassThru `
    -Wait

$exitSummary = @(
    '',
    "Run finished: $(Get-Date -Format o)",
    "ExitCode: $($process.ExitCode)"
)

Add-Content -Path $summaryPath -Value ($exitSummary -join "`r`n")

$logDir = Join-Path $BinDir 'log'
Copy-IfExists -SourcePath (Join-Path $logDir 'RPCS3.log') -DestinationPath (Join-Path $runDir 'RPCS3.log')
Copy-IfExists -SourcePath (Join-Path $logDir 'RPCS3.log.gz') -DestinationPath (Join-Path $runDir 'RPCS3.log.gz')
Copy-IfExists -SourcePath (Join-Path $logDir 'TTY.log') -DestinationPath (Join-Path $runDir 'TTY.log')
Copy-IfExists -SourcePath (Join-Path $BinDir 'RPCS3.buf') -DestinationPath (Join-Path $runDir 'RPCS3.buf')
Copy-IfExists -SourcePath (Join-Path $BinDir 'config\config.yml') -DestinationPath (Join-Path $runDir 'config.yml')
Copy-IfExists -SourcePath (Join-Path $BinDir 'GuiConfigs\CurrentSettings.ini') -DestinationPath (Join-Path $runDir 'CurrentSettings.ini')

if ($ConfigPath)
{
    Copy-IfExists -SourcePath $ConfigPath -DestinationPath (Join-Path $runDir ([System.IO.Path]::GetFileName($ConfigPath)))
}

Write-Host "`n== Run complete ==" -ForegroundColor Cyan
Write-Host "Exit code : $($process.ExitCode)"
Write-Host "Artifacts : $runDir"
