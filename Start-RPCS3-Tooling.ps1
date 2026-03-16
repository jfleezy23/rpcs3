[CmdletBinding()]
param(
	[string]$ProcessName = "rpcs3.exe",
	[string]$SessionName = "RPCS3",
	[ValidateSet("Mini", "MiniPlus", "FullCompressed")]
	[string]$DumpType = "MiniPlus",
	[int]$TimedSeconds = 0,
	[int]$MaxDumps = 1,
	[switch]$LaunchRpcs3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-InstalledToolPath
{
	param(
		[Parameter(Mandatory = $true)]
		[string[]]$Candidates,
		[Parameter(Mandatory = $true)]
		[string]$ToolName
	)

	foreach ($candidate in $Candidates)
	{
		if (Test-Path $candidate)
		{
			return $candidate
		}
	}

	throw "Could not find $ToolName in the expected install locations."
}

$presentMonPath = Resolve-InstalledToolPath -ToolName "PresentMon" -Candidates @(
	"$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Intel.PresentMon.Console_Microsoft.Winget.Source_8wekyb3d8bbwe\presentmon.exe"
)

$procDumpPath = Resolve-InstalledToolPath -ToolName "ProcDump" -Candidates @(
	"$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\procdump64.exe",
	"$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Microsoft.Sysinternals.Suite_Microsoft.Winget.Source_8wekyb3d8bbwe\procdump.exe"
)

$binDir = Join-Path $PSScriptRoot "bin"
$rpcs3Exe = Join-Path $binDir "rpcs3.exe"

if ($LaunchRpcs3 -and -not (Test-Path $rpcs3Exe))
{
	throw "Could not find RPCS3 executable at $rpcs3Exe"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$runRoot = Join-Path $binDir "tool_runs\$stamp"
$presentMonDir = Join-Path $runRoot "presentmon"
$dumpDir = Join-Path $runRoot "dumps"

New-Item -ItemType Directory -Force -Path $presentMonDir, $dumpDir | Out-Null

$presentMonCsv = Join-Path $presentMonDir "presentmon.csv"
$presentMonSession = "$SessionName-$stamp"
$presentMonArgs = @(
	"--process_name", $ProcessName,
	"--output_file", $presentMonCsv,
	"--v2_metrics",
	"--date_time",
	"--no_console_stats",
	"--session_name", $presentMonSession,
	"--terminate_on_proc_exit"
)

if ($TimedSeconds -gt 0)
{
	$presentMonArgs += @("--timed", $TimedSeconds, "--terminate_after_timed")
}

$dumpModeArg = switch ($DumpType)
{
	"Mini" { "-mm" }
	"MiniPlus" { "-mp" }
	"FullCompressed" { "-mac" }
	default { throw "Unsupported dump type: $DumpType" }
}

$procDumpArgs = @(
	"-accepteula",
	$dumpModeArg,
	"-e",
	"-h",
	"-n", $MaxDumps,
	"-w", $ProcessName,
	$dumpDir
)

$presentMonProcess = Start-Process -FilePath $presentMonPath -ArgumentList $presentMonArgs -WorkingDirectory $presentMonDir -PassThru
$procDumpProcess = Start-Process -FilePath $procDumpPath -ArgumentList $procDumpArgs -WorkingDirectory $dumpDir -PassThru

if ($LaunchRpcs3)
{
	Start-Process -FilePath $rpcs3Exe -WorkingDirectory $binDir | Out-Null
}

$summary = @"
RPCS3 tooling session started.

Run root     : $runRoot
PresentMon   : $presentMonPath
ProcDump     : $procDumpPath
CSV output   : $presentMonCsv
Dump folder  : $dumpDir
Process name : $ProcessName
Dump type    : $DumpType
Timed run    : $TimedSeconds
PresentMon PID: $($presentMonProcess.Id)
ProcDump PID : $($procDumpProcess.Id)
"@

$summaryPath = Join-Path $runRoot "session.txt"
Set-Content -Path $summaryPath -Value $summary -Encoding ASCII

$sessionInfo = [pscustomobject]@{
	RunRoot        = $runRoot
	PresentMonPath = $presentMonPath
	ProcDumpPath   = $procDumpPath
	PresentMonCsv  = $presentMonCsv
	DumpDir        = $dumpDir
	ProcessName    = $ProcessName
	DumpType       = $DumpType
	TimedSeconds   = $TimedSeconds
	PresentMonPid  = $presentMonProcess.Id
	ProcDumpPid    = $procDumpProcess.Id
}

$sessionJsonPath = Join-Path $runRoot "session.json"
$sessionInfo | ConvertTo-Json | Set-Content -Path $sessionJsonPath -Encoding ASCII

Write-Host $summary
