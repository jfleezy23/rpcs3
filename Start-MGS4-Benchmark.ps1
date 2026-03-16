[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SessionName,

    [Parameter(Mandatory = $true)]
    [string]$Variant,

    [string]$Scene = 'unspecified',

    [string]$BinDir,

    [string]$BenchmarkRoot,

    [string]$TitleId = 'NPUB31633',

    [string]$BootPath,

    [string]$ConfigPath,

    [switch]$NoGui,

    [switch]$Fullscreen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $BinDir)
{
    $BinDir = Join-Path $PSScriptRoot 'bin'
}

if (-not $BenchmarkRoot)
{
    $BenchmarkRoot = Join-Path $BinDir 'benchmarks'
}

function Assert-Directory([string]$Name, [string]$Path)
{
    if (-not (Test-Path $Path))
    {
        throw "$Name path does not exist: $Path"
    }
}

Assert-Directory -Name 'BinDir' -Path $BinDir
$null = New-Item -ItemType Directory -Path $BenchmarkRoot -Force

$sessionSafe = ($SessionName -replace '[^A-Za-z0-9._-]+', '-').Trim('-')
$variantSafe = ($Variant -replace '[^A-Za-z0-9._-]+', '-').Trim('-')
$sceneSafe = ($Scene -replace '[^A-Za-z0-9._-]+', '-').Trim('-')

if (-not $sessionSafe)
{
    throw 'SessionName must contain at least one usable character.'
}

if (-not $variantSafe)
{
    throw 'Variant must contain at least one usable character.'
}

$benchRoot = $BenchmarkRoot
$sessionRoot = Join-Path $benchRoot $sessionSafe
$variantRoot = Join-Path $sessionRoot $variantSafe
$null = New-Item -ItemType Directory -Path $variantRoot -Force

$manifestPath = Join-Path $sessionRoot 'benchmark-session.txt'
$manifestLines = @(
    "SessionName: $sessionSafe",
    "Variant: $variantSafe",
    "Scene: $Scene",
    "TitleId: $TitleId",
    "Created: $(Get-Date -Format o)"
)

if (-not (Test-Path $manifestPath))
{
    $null = New-Item -ItemType Directory -Path $sessionRoot -Force
    Set-Content -Path $manifestPath -Value ($manifestLines -join "`r`n") -Encoding UTF8
}

$runLabel = if ($sceneSafe) { "$variantSafe-$sceneSafe" } else { $variantSafe }
$runnerPath = Join-Path $PSScriptRoot 'Run-RPCS3-Logged.ps1'

$invokeParams = @{
    BinDir = $BinDir
    TitleId = $TitleId
    OutputRoot = $variantRoot
    RunName = $runLabel
}

if ($BootPath)
{
    $invokeParams.BootPath = $BootPath
}

if ($ConfigPath)
{
    $invokeParams.ConfigPath = $ConfigPath
}

if ($NoGui)
{
    $invokeParams.NoGui = $true
}

if ($Fullscreen)
{
    $invokeParams.Fullscreen = $true
}

Write-Host ''
Write-Host '== Benchmark session ==' -ForegroundColor Cyan
Write-Host "Session : $sessionSafe"
Write-Host "Variant : $variantSafe"
Write-Host "Scene   : $Scene"
Write-Host "Bench   : $benchRoot"
Write-Host "Output  : $variantRoot"

& $runnerPath @invokeParams
