param(
    [Parameter(Mandatory)][int]$ProcessId,
    [int]$Seconds = 180,
    [string]$OutputDirectory = 'C:\bo3recomp\analysis\performance'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Bo3PerfNative {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenThread(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] public static extern int GetThreadDescription(IntPtr thread, out IntPtr description);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] public static extern IntPtr LocalFree(IntPtr pointer);
    public static string Name(uint id) {
        IntPtr h = OpenThread(0x0800, false, id);
        if (h == IntPtr.Zero) return "<unavailable>";
        try {
            IntPtr text;
            if (GetThreadDescription(h, out text) < 0) return "<unavailable>";
            try { return Marshal.PtrToStringUni(text); }
            finally { LocalFree(text); }
        } finally { CloseHandle(h); }
    }
}
'@
$threadPath = Join-Path $OutputDirectory 'thread-cpu.csv'
$processPath = Join-Path $OutputDirectory 'process-cpu.csv'
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$previous = @{}
$names = @{}
$previousTime = 0.0
$previousCpu = 0.0
$first = $true
$meta = [ordered]@{
    process_id = $ProcessId
    started_utc = [DateTime]::UtcNow.ToString('o')
    host_tick = [System.Diagnostics.Stopwatch]::GetTimestamp()
    host_tick_frequency = [System.Diagnostics.Stopwatch]::Frequency
    logical_processors = [int]$env:NUMBER_OF_PROCESSORS
    cpu_percent_definition = '100 percent equals one fully occupied logical processor'
}
$meta | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'sampling.json')
while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
    $game = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if (-not $game) { break }
    $now = $clock.Elapsed.TotalSeconds
    $utc = [DateTime]::UtcNow.ToString('o')
    $hostTick = [System.Diagnostics.Stopwatch]::GetTimestamp()
    $cpu = $game.TotalProcessorTime.TotalMilliseconds
    $dt = $now - $previousTime
    $rows = @()
    $next = @{}
    foreach ($thread in $game.Threads) {
        try {
            $id = $thread.Id
            $ms = $thread.TotalProcessorTime.TotalMilliseconds
            $next[$id] = $ms
            if (-not $names.ContainsKey($id)) { $names[$id] = [Bo3PerfNative]::Name($id) }
            if (-not $first -and $previous.ContainsKey($id)) {
                $rows += [pscustomobject]@{
                    utc = $utc; host_tick = $hostTick; elapsed_seconds = [math]::Round($now, 3)
                    thread_id = $id; name = $names[$id]
                    cpu_ms = [math]::Round($ms - $previous[$id], 3)
                    cpu_percent_one_core = [math]::Round(($ms - $previous[$id]) / ($dt * 10), 2)
                }
            }
        } catch { }
    }
    if (-not $first) {
        $rows | Export-Csv -LiteralPath $threadPath -Append -NoTypeInformation
        [pscustomobject]@{
            utc = $utc; host_tick = $hostTick; elapsed_seconds = [math]::Round($now, 3)
            cpu_percent_one_core = [math]::Round(($cpu - $previousCpu) / ($dt * 10), 2)
            working_set_mb = [math]::Round($game.WorkingSet64 / 1MB, 1)
            threads_sampled = $rows.Count
        } | Export-Csv -LiteralPath $processPath -Append -NoTypeInformation
    }
    $previous = $next
    $previousTime = $now
    $previousCpu = $cpu
    $first = $false
    Start-Sleep -Seconds 2
}
Write-Output "CPU sampling complete: $OutputDirectory"
