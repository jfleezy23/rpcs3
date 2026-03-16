[CmdletBinding()]
param(
    [string]$BinDir,

    [string[]]$TitleIds = @(
        'BLUS30109',
        'BLES00246',
        'NPUB31633',
        'NPEB02134'
    ),

    [ValidateSet('Operating System', 'RPCS3 Scheduler', 'RPCS3 Alternative Scheduler')]
    [string]$ThreadSchedulerMode = 'RPCS3 Scheduler',

    [ValidateRange(0, 16)]
    [int]$ShaderCompilerThreads = 0,

    [ValidateSet('Default', 'High', 'Ultra')]
    [string]$VisualPreset = 'High',

    [switch]$EnablePerfReport,

    [switch]$EnablePerfOverlay
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $BinDir)
{
    $BinDir = Join-Path $PSScriptRoot 'bin'
}

function Assert-Directory([string]$Name, [string]$Path)
{
    if (-not (Test-Path $Path))
    {
        throw "$Name path does not exist: $Path"
    }
}

Assert-Directory -Name 'BinDir' -Path $BinDir

$customConfigDir = Join-Path $BinDir 'config\custom_configs'
$null = New-Item -ItemType Directory -Path $customConfigDir -Force

$resolutionScale = 100
$anisotropicOverride = 0
$textureLodBias = 0
$vsyncEnabled = $false
$outputScalingMode = 'Bilinear'
$rcasSharpeningIntensity = 50

switch ($VisualPreset)
{
    'High'
    {
        $resolutionScale = 200
        $anisotropicOverride = 16
        $vsyncEnabled = $true
        $rcasSharpeningIntensity = 30
    }

    'Ultra'
    {
        $resolutionScale = 300
        $anisotropicOverride = 16
        $textureLodBias = -0.5
        $vsyncEnabled = $true
        $outputScalingMode = 'FidelityFX Super Resolution'
        $rcasSharpeningIntensity = 30
    }
}

$coreLines = @(
    'Core:',
    "  Thread Scheduler Mode: $ThreadSchedulerMode",
    '  SPU Reservation Busy Waiting Enabled: false',
    '  SPU loop detection: true',
    '  Sleep Timers Accuracy: Usleep Only'
)

if ($EnablePerfReport)
{
    $coreLines += '  Enable Performance Report: true'
}

$videoLines = @(
    'Video:',
    '  Renderer: Vulkan',
    '  Aspect ratio: 16:9',
    '  Stretch To Display Area: false',
    '  MSAA: Auto',
    "  VSync: $($vsyncEnabled.ToString().ToLowerInvariant())",
    '  Shader Mode: Async Shader Recompiler',
    '  Shader Precision: High',
    '  Multithreaded RSX: true',
    "  Shader Compiler Threads: $ShaderCompilerThreads",
    '  Use full RGB output range: true',
    '  Force Hardware MSAA Resolve: false',
    "  Resolution Scale: $resolutionScale",
    "  Anisotropic Filter Override: $anisotropicOverride",
    "  Texture LOD Bias Addend: $textureLodBias",
    '  Minimum Scalable Dimension: 16',
    "  Output Scaling Mode: $outputScalingMode"
)

if ($EnablePerfOverlay)
{
    $videoLines += '  Performance Overlay:'
    $videoLines += '    Enabled: true'
}

$videoLines += '  Vulkan:'
$videoLines += "    FidelityFX CAS Sharpening Intensity: $rcasSharpeningIntensity"
$videoLines += '    Asynchronous Texture Streaming 2: true'
$videoLines += '    Asynchronous Queue Scheduler: Safe'
$videoLines += '    Use Re-BAR for GPU uploads: true'

$miscLines = @(
    'Miscellaneous:',
    '  Show shader compilation hint: false',
    '  Show PPU compilation hint: false'
)

$configBody = (($coreLines + $videoLines + $miscLines) -join "`r`n") + "`r`n"

foreach ($titleId in $TitleIds)
{
    if (-not $titleId)
    {
        continue
    }

    $normalizedTitleId = $titleId.Trim().ToUpperInvariant()

    if ($normalizedTitleId -notmatch '^[A-Z0-9]{9}$')
    {
        throw "Title ID '$titleId' is not in the expected PS3 title-id format."
    }

    $configPath = Join-Path $customConfigDir "config_$normalizedTitleId.yml"
    Set-Content -Path $configPath -Value $configBody -Encoding UTF8
    Write-Host "Wrote $configPath" -ForegroundColor Green
}

Write-Host ''
Write-Host 'Profile summary:' -ForegroundColor Cyan
Write-Host "  Thread Scheduler Mode: $ThreadSchedulerMode"
Write-Host "  Visual Preset: $VisualPreset"
Write-Host '  Renderer: Vulkan'
Write-Host "  Resolution Scale: $resolutionScale"
Write-Host "  Anisotropic Filter Override: $anisotropicOverride"
Write-Host "  Output Scaling Mode: $outputScalingMode"
Write-Host "  VSync: $($vsyncEnabled.ToString().ToLowerInvariant())"
Write-Host '  Shader Mode: Async Shader Recompiler'
Write-Host '  Shader Precision: High'
Write-Host '  Multithreaded RSX: true'
Write-Host "  Shader Compiler Threads: $ShaderCompilerThreads"
Write-Host '  Use full RGB output range: true'
Write-Host '  SPU loop detection: true'
Write-Host "  Vulkan.FidelityFX CAS Sharpening Intensity: $rcasSharpeningIntensity"
Write-Host '  Vulkan.Asynchronous Texture Streaming 2: true'
Write-Host '  Vulkan.Asynchronous Queue Scheduler: Safe'
Write-Host '  Vulkan.Use Re-BAR for GPU uploads: true'
Write-Host '  Miscellaneous.Show shader compilation hint: false'
Write-Host '  Miscellaneous.Show PPU compilation hint: false'
Write-Host "  Performance Report: $($EnablePerfReport.IsPresent)"
Write-Host "  Performance Overlay: $($EnablePerfOverlay.IsPresent)"
