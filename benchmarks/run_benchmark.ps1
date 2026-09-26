#requires -Version 5.1
[CmdletBinding(DefaultParameterSetName = 'Single')]
param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,

    [Parameter(ParameterSetName = 'Single')]
    [ValidateRange(1, 3600000)]
    [int]$IntervalMs = 1000,

    [Parameter(ParameterSetName = 'Single')]
    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 300,

    [Parameter(ParameterSetName = 'Single')]
    [ValidateRange(1, 100000)]
    [int]$Run = 1,

    [string]$RunId = 'current',

    [string]$ResultsRoot,

    [Parameter(Mandatory = $true, ParameterSetName = 'Suite')]
    [ValidateSet('Smoke', 'Full')]
    [string]$Suite,

    [switch]$Legacy,

    [string]$BuildConfig = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($BuildConfig -ne 'Release') {
    throw "Refusing to benchmark a $BuildConfig binary. Performance tests require Release."
}

function ConvertTo-CommandArgument {
    param([string]$Value)
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Get-GitMetadata {
    param([string]$ExePath)
    $result = @{ sha = $null; dirty = $null }
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) { return $result }
    $dir = Split-Path -Parent $ExePath
    & git -C $dir rev-parse --is-inside-work-tree 1>$null 2>$null
    if ($LASTEXITCODE -ne 0) { return $result }
    $sha = (& git -C $dir rev-parse HEAD).Trim()
    $porcelain = & git -C $dir status --porcelain
    $result.sha = $sha
    $result.dirty = -not [string]::IsNullOrWhiteSpace((($porcelain | Out-String).Trim()))
    return $result
}

function Get-MachineMetadata {
    $info = @{
        windows_version = $null
        cpu_model = $null
        total_memory_bytes = $null
        logical_cpu_count = [Environment]::ProcessorCount
    }
    try {
        $os = Get-CimInstance -ClassName Win32_OperatingSystem
        $info.windows_version = ('{0} {1}' -f $os.Caption, $os.Version).Trim()
    } catch {}
    try {
        $cpu = @(Get-CimInstance -ClassName Win32_Processor) | Select-Object -First 1
        if ($cpu -and $cpu.Name) { $info.cpu_model = ([string]$cpu.Name).Trim() }
    } catch {}
    try {
        $computer = Get-CimInstance -ClassName Win32_ComputerSystem
        $info.total_memory_bytes = [int64]$computer.TotalPhysicalMemory
    } catch {}
    return $info
}

function Invoke-BenchmarkRun {
    param(
        [string]$ExePath,
        [int]$Interval,
        [int]$Duration,
        [int]$RunNumber,
        [string]$Id,
        [string]$Root,
        [bool]$UseLegacy,
        [hashtable]$GitInfo,
        [hashtable]$Machine
    )

    $runName = '{0}ms-run{1}' -f $Interval, $RunNumber
    $runDir = Join-Path (Join-Path $Root $Id) $runName
    if (Test-Path -LiteralPath $runDir) {
        throw "Run directory already exists: $runDir"
    }
    New-Item -ItemType Directory -Path $runDir | Out-Null
    $dataDir = Join-Path $runDir 'data'
    New-Item -ItemType Directory -Path $dataDir | Out-Null

    $timingPath = Join-Path $runDir 'sample_timing.csv'
    $resourcePath = Join-Path $runDir 'resource_usage.csv'
    $metadataPath = Join-Path $runDir 'metadata.json'
    $stdoutPath = Join-Path $runDir 'sentinel_stdout.txt'
    $stderrPath = Join-Path $runDir 'sentinel_stderr.txt'

    $arguments = @('start')
    if (-not $UseLegacy) {
        $arguments += @(
            '--benchmark',
            '--interval-ms', "$Interval",
            '--benchmark-output', $timingPath,
            '--benchmark-duration-sec', "$Duration",
            '--data-dir', $dataDir
        )
    }

    Write-Host ("Starting {0} interval={1}ms duration={2}s run={3} legacy={4}" -f $Id, $Interval, $Duration, $RunNumber, $UseLegacy)

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $ExePath
    $startInfo.Arguments = (($arguments | ForEach-Object { ConvertTo-CommandArgument $_ }) -join ' ')
    $startInfo.WorkingDirectory = $runDir
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw "Failed to start $ExePath" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()

    $cpuCount = [Environment]::ProcessorCount
    if ($cpuCount -lt 1) { throw 'ProcessorCount is invalid' }
    $invariant = [System.Globalization.CultureInfo]::InvariantCulture
    $rows = New-Object System.Collections.Generic.List[string]
    $started = [System.Diagnostics.Stopwatch]::StartNew()
    $nextSample = [DateTime]::UtcNow.AddSeconds(1)
    $baselineCpu = $null
    $baselineStamp = $null
    $stopMethod = 'self-exit'
    $hardTimeoutMs = $(if ($UseLegacy) { $Duration } else { $Duration + 90 }) * 1000

    while (-not $process.HasExited) {
        if ($started.ElapsedMilliseconds -gt $hardTimeoutMs) {
            try { $process.Kill() } catch {}
            $stopMethod = 'terminated'
            break
        }
        $waitMs = [int][Math]::Ceiling(($nextSample - [DateTime]::UtcNow).TotalMilliseconds)
        if ($waitMs -gt 0) {
            Start-Sleep -Milliseconds ([Math]::Min($waitMs, 200))
            continue
        }
        $nextSample = [DateTime]::UtcNow.AddSeconds(1)
        $now = [DateTime]::UtcNow
        try {
            $process.Refresh()
            if ($process.HasExited) { break }
            $cpuSeconds = $process.TotalProcessorTime.TotalSeconds
            $privateBytes = $process.PrivateMemorySize64
            $workingSetBytes = $process.WorkingSet64
        } catch {
            break
        }
        if ($null -ne $baselineCpu) {
            $wall = ($now - $baselineStamp).TotalSeconds
            if ($wall -gt 0) {
                $cpuPercent = 100.0 * ($cpuSeconds - $baselineCpu) / ($wall * $cpuCount)
                $line = [string]::Format(
                    $invariant,
                    '{0:yyyy-MM-ddTHH:mm:ss.fff}Z,{1:F3},{2:F4},{3},{4}',
                    $now,
                    $started.Elapsed.TotalSeconds,
                    $cpuPercent,
                    $privateBytes,
                    $workingSetBytes)
                $rows.Add($line)
            }
        }
        $baselineCpu = $cpuSeconds
        $baselineStamp = $now
    }

    if (-not $process.HasExited) {
        $null = $process.WaitForExit(10000)
    }
    if (-not $process.HasExited) {
        try { $process.Kill() } catch {}
        $stopMethod = 'terminated'
        $null = $process.WaitForExit(5000)
    }

    if ($stdoutTask.Wait(10000)) {
        [System.IO.File]::WriteAllText($stdoutPath, $stdoutTask.Result)
    } else {
        [System.IO.File]::WriteAllText($stdoutPath, '')
    }
    if ($stderrTask.Wait(10000)) {
        [System.IO.File]::WriteAllText($stderrPath, $stderrTask.Result)
    } else {
        [System.IO.File]::WriteAllText($stderrPath, '')
    }

    $header = 'timestamp_utc,elapsed_seconds,cpu_percent,private_bytes,working_set_bytes'
    $resourceLines = New-Object System.Collections.Generic.List[string]
    $resourceLines.Add($header)
    $resourceLines.AddRange($rows)
    [System.IO.File]::WriteAllLines($resourcePath, $resourceLines)

    $exitCode = $null
    try { $exitCode = $process.ExitCode } catch {}

    $notes = 'Scheduling delay is the steady-clock difference between the actual sample start and the existing sleep_until deadline. analyze.py applies the late threshold to the raw delays.'
    if ($UseLegacy) {
        $notes = 'Historical binary: external CPU and memory only. The sampling interval was not changed and sample_timing.csv was not produced.'
    }

    $metadata = [ordered]@{
        git_commit = $GitInfo.sha
        git_dirty = $GitInfo.dirty
        sampling_interval_ms = $(if ($UseLegacy) { $null } else { $Interval })
        requested_interval_ms = $Interval
        interval_control = -not $UseLegacy
        target_rate_hz = $(if ($UseLegacy) { $null } else { [math]::Round(1000.0 / $Interval, 6) })
        benchmark_duration_seconds = $Duration
        run_number = $RunNumber
        run_id = $Id
        build_configuration = $BuildConfig
        logical_cpu_count = $Machine.logical_cpu_count
        timestamp = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
        executable_path = $ExePath
        windows_version = $Machine.windows_version
        cpu_model = $Machine.cpu_model
        total_memory_bytes = $Machine.total_memory_bytes
        timing_instrumentation = -not $UseLegacy
        late_threshold_ratio = 0.1
        data_directory = $(if ($UseLegacy) { $null } else { $dataDir })
        stop_method = $stopMethod
        process_exit_code = $exitCode
        notes = $notes
    }
    $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $metadataPath -Encoding UTF8

    if ($stopMethod -eq 'terminated') {
        throw "Sentinel did not exit on its own. See $stderrPath"
    }
    if ($null -eq $exitCode -or $exitCode -ne 0) {
        $stderrText = [System.IO.File]::ReadAllText($stderrPath)
        throw "Sentinel exited with code $exitCode. $stderrText"
    }
    if (-not $UseLegacy -and -not (Test-Path -LiteralPath $timingPath)) {
        throw "Benchmark timing file was not written: $timingPath"
    }
    Write-Host "Saved $runDir"
}

if (-not (Test-Path -LiteralPath $Executable)) {
    throw "Executable not found: $Executable"
}
$resolvedExe = (Resolve-Path -LiteralPath $Executable).Path
$exeParent = Split-Path -Parent $resolvedExe
if ($exeParent -match '\\build$' -and $exeParent -notmatch '\\build-release$') {
    Write-Warning "Executable is in a 'build' directory. The default CMake preset is Debug. Use the Release binary from build-release."
}

if ($PSCmdlet.ParameterSetName -eq 'Suite') {
    if ($Legacy) {
        throw '-Legacy cannot be combined with -Suite. Historical binaries keep their built-in interval. Measure them with -Legacy -IntervalMs 1000.'
    }
} elseif ($Legacy -and $IntervalMs -ne 1000) {
    throw '-Legacy does not change the sampling interval. Historical checkpoints in this repo sample at 1000 ms. Pass -IntervalMs 1000.'
}

if ([string]::IsNullOrWhiteSpace($ResultsRoot)) {
    $ResultsRoot = Join-Path $PSScriptRoot 'results'
}
New-Item -ItemType Directory -Path $ResultsRoot -Force | Out-Null

$gitInfo = Get-GitMetadata -ExePath $resolvedExe
$machine = Get-MachineMetadata

if ($PSCmdlet.ParameterSetName -eq 'Suite') {
    $plan = New-Object System.Collections.Generic.List[object]
    if ($Suite -eq 'Smoke') {
        foreach ($interval in @(1000, 100, 10)) {
            $plan.Add(@{ Interval = $interval; Duration = 30; Run = 1 })
        }
    } else {
        foreach ($interval in @(1000, 500, 100, 50, 10)) {
            foreach ($repetition in 1..3) {
                $plan.Add(@{ Interval = $interval; Duration = 300; Run = $repetition })
            }
        }
    }
    foreach ($item in $plan) {
        Invoke-BenchmarkRun -ExePath $resolvedExe -Interval $item.Interval -Duration $item.Duration -RunNumber $item.Run -Id $RunId -Root $ResultsRoot -UseLegacy:$false -GitInfo $gitInfo -Machine $machine
    }
} else {
    Invoke-BenchmarkRun -ExePath $resolvedExe -Interval $IntervalMs -Duration $DurationSeconds -RunNumber $Run -Id $RunId -Root $ResultsRoot -UseLegacy ([bool]$Legacy) -GitInfo $gitInfo -Machine $machine
}
