[CmdletBinding()]
param(
	[string]$SessionPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $SessionPath)
{
	$latest = Get-ChildItem -Path (Join-Path $PSScriptRoot "bin\tool_runs") -Directory -ErrorAction SilentlyContinue |
		Sort-Object LastWriteTime -Descending |
		Select-Object -First 1

	if (-not $latest)
	{
		throw "No tooling sessions were found."
	}

	$SessionPath = $latest.FullName
}

$sessionJsonPath = Join-Path $SessionPath "session.json"
if (-not (Test-Path $sessionJsonPath))
{
	throw "Could not find session metadata at $sessionJsonPath"
}

$session = Get-Content $sessionJsonPath | ConvertFrom-Json
$stopped = @()

foreach ($procId in @($session.PresentMonPid, $session.ProcDumpPid))
{
	if (-not $procId)
	{
		continue
	}

	$process = Get-Process -Id $procId -ErrorAction SilentlyContinue
	if ($process)
	{
		Stop-Process -Id $procId
		$stopped += $procId
	}
}

if ($stopped.Count -eq 0)
{
	Write-Host "No active tooling processes were running for $SessionPath"
}
else
{
	Write-Host "Stopped tooling process IDs: $($stopped -join ', ')"
}
