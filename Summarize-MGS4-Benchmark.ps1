[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SessionName,

    [string]$BinDir,

    [string]$BenchmarkRoot,

    [string]$TitleId = 'NPUB31633'
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

function Parse-KeyValueFile([string]$Path)
{
    $map = @{}

    foreach ($line in Get-Content $Path)
    {
        if ($line -match '^\s*([^:]+):\s*(.*)$')
        {
            $map[$matches[1].Trim()] = $matches[2].Trim()
        }
    }

    return $map
}

function Parse-Run([string]$RunDir)
{
    $summaryPath = Join-Path $RunDir 'run-summary.txt'
    $logPath = Join-Path $RunDir 'RPCS3.log'

    $summary = @{}
    if (Test-Path $summaryPath)
    {
        $summary = Parse-KeyValueFile -Path $summaryPath
    }

    $started = $null
    $finished = $null
    $durationSeconds = $null

    if ($summary.ContainsKey('Run started'))
    {
        $started = [datetimeoffset]::Parse($summary['Run started'])
    }

    if ($summary.ContainsKey('Run finished'))
    {
        $finished = [datetimeoffset]::Parse($summary['Run finished'])
    }

    if ($started -and $finished)
    {
        $durationSeconds = [math]::Round(($finished - $started).TotalSeconds, 2)
    }

    $result = [ordered]@{
        RunDir = $RunDir
        Variant = Split-Path (Split-Path $RunDir -Parent) -Leaf
        RunName = if ($summary.ContainsKey('RunName')) { $summary['RunName'] } else { '' }
        Started = $started
        DurationSeconds = $durationSeconds
        ExitCode = if ($summary.ContainsKey('ExitCode')) { [int]$summary['ExitCode'] } else { $null }
        Crash = $false
        CrashLine = ''
        AutoWorkers = $null
        HostThreads = $null
        ShaderJobs = $null
        ShaderAvgUs = $null
        ShaderMaxUs = $null
        ShaderPeakPending = $null
        QueueSubmits = $null
        QueueSubmitHoldAvgUs = $null
        QueueSubmitWaitTotalUs = $null
        QueueFlushWaits = $null
    }

    if (Test-Path $logPath)
    {
        $content = Get-Content $logPath

        $autoLine = $content | Select-String -Pattern 'Async pipeline compiler auto-selected (\d+) worker\(s\) for (\d+) host thread\(s\)\.' | Select-Object -Last 1
        if ($autoLine)
        {
            $match = [regex]::Match($autoLine.Line, 'auto-selected (\d+) worker\(s\) for (\d+) host thread\(s\)')
            if ($match.Success)
            {
                $result.AutoWorkers = [int]$match.Groups[1].Value
                $result.HostThreads = [int]$match.Groups[2].Value
            }
        }

        $shaderLine = $content | Select-String -Pattern 'Async pipeline compiler stats: workers=(\d+), jobs=(\d+), avg=(\d+)us, max=(\d+)us, peak_pending=(\d+)' | Select-Object -Last 1
        if ($shaderLine)
        {
            $match = [regex]::Match($shaderLine.Line, 'workers=(\d+), jobs=(\d+), avg=(\d+)us, max=(\d+)us, peak_pending=(\d+)')
            if ($match.Success)
            {
                $result.ShaderJobs = [int]$match.Groups[2].Value
                $result.ShaderAvgUs = [int]$match.Groups[3].Value
                $result.ShaderMaxUs = [int]$match.Groups[4].Value
                $result.ShaderPeakPending = [int]$match.Groups[5].Value
            }
        }

        $queueLine = $content | Select-String -Pattern 'Vulkan queue diagnostics: submits=(\d+), submit_wait_total=(\d+)us, submit_wait_avg=(\d+)us, submit_hold_total=(\d+)us, submit_hold_avg=(\d+)us, flush_waits=(\d+),' | Select-Object -Last 1
        if ($queueLine)
        {
            $match = [regex]::Match($queueLine.Line, 'submits=(\d+), submit_wait_total=(\d+)us, submit_wait_avg=(\d+)us, submit_hold_total=(\d+)us, submit_hold_avg=(\d+)us, flush_waits=(\d+)')
            if ($match.Success)
            {
                $result.QueueSubmits = [int]$match.Groups[1].Value
                $result.QueueSubmitWaitTotalUs = [int]$match.Groups[2].Value
                $result.QueueSubmitHoldAvgUs = [int]$match.Groups[5].Value
                $result.QueueFlushWaits = [int]$match.Groups[6].Value
            }
        }

        $crashLine = $content | Select-String -Pattern 'VM: Access violation' | Select-Object -Last 1
        if ($crashLine)
        {
            $result.Crash = $true
            $result.CrashLine = $crashLine.Line.Trim()
        }
    }

    return [pscustomobject]$result
}

Assert-Directory -Name 'BenchmarkRoot' -Path $BenchmarkRoot

$sessionSafe = ($SessionName -replace '[^A-Za-z0-9._-]+', '-').Trim('-')
$sessionRoot = Join-Path $BenchmarkRoot $sessionSafe
Assert-Directory -Name 'Benchmark session' -Path $sessionRoot

$runDirs = Get-ChildItem $sessionRoot -Directory -Recurse | Where-Object { Test-Path (Join-Path $_.FullName 'run-summary.txt') } | Sort-Object FullName

if (-not $runDirs)
{
    throw "No benchmark runs were found under $sessionRoot"
}

$runs = foreach ($runDir in $runDirs)
{
    Parse-Run -RunDir $runDir.FullName
}

$summaryPath = Join-Path $sessionRoot 'summary.md'
$lines = New-Object System.Collections.Generic.List[string]

$lines.Add("# MGS4 benchmark summary")
$lines.Add("")
$lines.Add("Session: ``$sessionSafe``")
$lines.Add("Title ID: ``$TitleId``")
$lines.Add("Generated: $(Get-Date -Format o)")
$lines.Add("")

foreach ($variantGroup in ($runs | Group-Object Variant))
{
    $variantRuns = @($variantGroup.Group | Sort-Object Started)
    $durationValues = @($variantRuns | Where-Object { $_.DurationSeconds -ne $null } | ForEach-Object { [double]$_.DurationSeconds })
    $shaderAvgValues = @($variantRuns | Where-Object { $_.ShaderAvgUs -ne $null } | ForEach-Object { [double]$_.ShaderAvgUs })
    $shaderMaxValues = @($variantRuns | Where-Object { $_.ShaderMaxUs -ne $null } | ForEach-Object { [double]$_.ShaderMaxUs })
    $queueHoldValues = @($variantRuns | Where-Object { $_.QueueSubmitHoldAvgUs -ne $null } | ForEach-Object { [double]$_.QueueSubmitHoldAvgUs })
    $crashCount = @($variantRuns | Where-Object { $_.Crash }).Count

    $lines.Add("## Variant: ``$($variantGroup.Name)``")
    $lines.Add("")
    $lines.Add("- Runs: $($variantRuns.Count)")
    if ($durationValues.Count -gt 0)
    {
        $lines.Add("- Avg run duration: $([math]::Round((($durationValues | Measure-Object -Average).Average), 2)) s")
    }
    if ($shaderAvgValues.Count -gt 0)
    {
        $lines.Add("- Avg shader compile time: $([math]::Round((($shaderAvgValues | Measure-Object -Average).Average), 2)) us")
    }
    if ($shaderMaxValues.Count -gt 0)
    {
        $lines.Add("- Avg max shader compile spike: $([math]::Round((($shaderMaxValues | Measure-Object -Average).Average), 2)) us")
    }
    if ($queueHoldValues.Count -gt 0)
    {
        $lines.Add("- Avg queue submit hold: $([math]::Round((($queueHoldValues | Measure-Object -Average).Average), 2)) us")
    }
    $lines.Add("- Guest crashes: $crashCount")
    $lines.Add("")
    $lines.Add("| Run | Duration (s) | Auto workers | Shader jobs | Shader avg us | Shader max us | Peak pending | Queue submits | Queue hold avg us | Crash |")
    $lines.Add("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |")

    foreach ($run in $variantRuns)
    {
        $runLabel = if ($run.RunName) { $run.RunName } else { Split-Path $run.RunDir -Leaf }
        $lines.Add("| ``$runLabel`` | $($run.DurationSeconds) | $($run.AutoWorkers) | $($run.ShaderJobs) | $($run.ShaderAvgUs) | $($run.ShaderMaxUs) | $($run.ShaderPeakPending) | $($run.QueueSubmits) | $($run.QueueSubmitHoldAvgUs) | $($run.Crash) |")
    }

    if ($crashCount -gt 0)
    {
        $lines.Add("")
        $lines.Add("Crash lines:")
        foreach ($run in ($variantRuns | Where-Object { $_.Crash }))
        {
            $lines.Add("- ``$($run.RunName)``: $($run.CrashLine)")
        }
    }

    $lines.Add("")
}

$lines.Add("## PR note template")
$lines.Add("")
$lines.Add('```md')
$lines.Add('Benchmark notes for `#18351` on Ryzen 9 9950X + RTX 4090:')
$lines.Add("")
$lines.Add("- Session: $sessionSafe")
$lines.Add("- Method: repeat runs using `Run-RPCS3-Logged.ps1` with `Enable Performance Report` enabled")
$lines.Add("- Title: $TitleId")
$lines.Add("- Compare the variant tables above for before/after timing and stability")
$lines.Add('```')

Set-Content -Path $summaryPath -Value ($lines -join "`r`n") -Encoding UTF8
Get-Content $summaryPath
