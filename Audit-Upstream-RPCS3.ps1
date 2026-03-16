[CmdletBinding()]
param(
	[string]$RepoRoot,
	[string]$UpstreamRef = 'upstream/master',
	[int]$MaxCommits = 20,
	[switch]$Fetch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $RepoRoot)
{
	$RepoRoot = $PSScriptRoot
}

$repoRoot = (Resolve-Path $RepoRoot).Path
$watchedPaths = @(
	'Utilities/JITLLVM.cpp',
	'Utilities/Thread.cpp',
	'rpcs3/Emu/Cell/PPUThread.cpp',
	'rpcs3/Emu/RSX',
	'3rdparty/GPUOpen/VulkanMemoryAllocator',
	'.gitmodules'
)

function Write-Section([string]$Title)
{
	Write-Host "`n== $Title ==" -ForegroundColor Cyan
}

function Invoke-Git([string[]]$Arguments)
{
	$output = & git -C $repoRoot @Arguments
	if ($LASTEXITCODE -ne 0)
	{
		throw "git command failed: git -C $repoRoot $($Arguments -join ' ')"
	}

	return $output
}

if ($Fetch)
{
	Write-Section 'Fetch'
	Invoke-Git @('fetch', 'upstream', 'master') | Out-Null
}

$head = (Invoke-Git @('rev-parse', 'HEAD')).Trim()
$upstream = (Invoke-Git @('rev-parse', $UpstreamRef)).Trim()
$aheadBehind = (Invoke-Git @('rev-list', '--left-right', '--count', "$head...$upstream")).Trim() -split '\s+'
$ahead = [int]$aheadBehind[0]
$behind = [int]$aheadBehind[1]

$localSubmodule = ((Invoke-Git @('ls-tree', 'HEAD', '3rdparty/GPUOpen/VulkanMemoryAllocator')).Trim() -split '\s+')[2]
$upstreamSubmodule = ((Invoke-Git @('ls-tree', $UpstreamRef, '3rdparty/GPUOpen/VulkanMemoryAllocator')).Trim() -split '\s+')[2]

$localDirty = Invoke-Git (@('status', '--short', '--') + $watchedPaths) | Where-Object { $_ }
$upstreamFiles = Invoke-Git (@('diff', '--name-only', "$head..$UpstreamRef", '--') + $watchedPaths) | Where-Object { $_ }
$upstreamCommits = Invoke-Git (@('log', '--oneline', "--max-count=$MaxCommits", "$head..$UpstreamRef", '--') + $watchedPaths) | Where-Object { $_ }

Write-Section 'Refs'
Write-Host "Repo root            : $repoRoot"
Write-Host "Local HEAD           : $head"
Write-Host "Upstream ref         : $UpstreamRef"
Write-Host "Upstream HEAD        : $upstream"
Write-Host "Ahead / behind       : $ahead / $behind"
Write-Host "Local VMA submodule  : $localSubmodule"
Write-Host "Upstream VMA pointer : $upstreamSubmodule"

Write-Section 'Local Changes'
if ($localDirty)
{
	$localDirty | ForEach-Object { Write-Host $_ }
}
else
{
	Write-Host 'No local modifications in watched paths.'
}

Write-Section 'Upstream Changed Files'
if ($upstreamFiles)
{
	$upstreamFiles | ForEach-Object { Write-Host $_ }
}
else
{
	Write-Host 'No upstream file deltas in watched paths.'
}

Write-Section 'Recent Upstream Commits'
if ($upstreamCommits)
{
	$upstreamCommits | ForEach-Object { Write-Host $_ }
}
else
{
	Write-Host 'No upstream commits touch watched paths beyond local HEAD.'
}
