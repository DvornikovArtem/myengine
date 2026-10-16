<#
.SYNOPSIS
  One entry point for the C++ checks of myengine: build + tests, AddressSanitizer, MSVC /analyze, clang-tidy.

.DESCRIPTION
  tools\check.ps1 [-Quick] [-Asan] [-Analyze] [-Tidy] [-All] [-Config Debug] [-AsanFrames 300] [-UpdateBaseline]

    -Quick    build/ (the default build tree), Debug build, ctest                 (the default when nothing is given)
    -Asan     build-asan/ with MYENGINE_ASAN=ON: build, ctest, two GUI smokes and a short editor run
    -Analyze  build-analyze/ with MYENGINE_ANALYZE=ON: MSVC /analyze on our code, compared with the baseline
    -Tidy     build-tidy/ (Ninja, compile_commands.json): clang-tidy on our code, compared with the baseline
    -All      all of the above

  The console shows one line per step. Everything else is in <build dir>\logs\ (full logs, the list of
  findings). Exit code: 0 - every step is OK, 1 - something failed or a check found new problems.
  -UpdateBaseline rewrites tools\baselines\*.txt with what -Analyze / -Tidy found (use it on purpose, in a PR).

  The script is ASCII on purpose: it has to work in Windows PowerShell 5.1 with a Cyrillic path to the repository.
#>
[CmdletBinding()]
param(
    [switch]$Quick,
    [switch]$Asan,
    [switch]$Analyze,
    [switch]$Tidy,
    [switch]$All,
    [string]$Config = 'Debug',
    [int]$AsanFrames = 300,
    [switch]$UpdateBaseline,
    [switch]$ReuseLog   # -Analyze: parse the log of the previous analyze build instead of building again
)

$ErrorActionPreference = 'Continue'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BaselineDir = Join-Path $PSScriptRoot 'baselines'

if ($All) { $Quick = $true; $Asan = $true; $Analyze = $true; $Tidy = $true }
if (-not ($Quick -or $Asan -or $Analyze -or $Tidy)) { $Quick = $true }

$script:Results = New-Object System.Collections.ArrayList
$script:Failed = $false

# ---- helpers ---------------------------------------------------------------------------------------------------

function Add-Result([string]$Step, [bool]$Ok, [string]$Detail, [string]$Log) {
    [void]$script:Results.Add([pscustomobject]@{ Step = $Step; Ok = $Ok; Detail = $Detail; Log = $Log })
    if (-not $Ok) { $script:Failed = $true }
    $mark = if ($Ok) { 'OK  ' } else { 'FAIL' }
    $line = '[{0}] {1,-24} {2}' -f $mark, $Step, $Detail
    if ($Log) { $line += '  (' + $Log + ')' }
    Write-Host $line
}

# A command line from arguments (CreateProcess takes one string)
function ConvertTo-CommandLine([string[]]$Arguments) {
    ($Arguments | ForEach-Object {
        if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' '
}

# Starts a program (CreateProcessW: Cyrillic paths are fine) with stdout and stderr copied byte for byte into a log
# file. The returned object is for Wait-Logged. Use -CommandLine for a line that is already quoted.
function Start-Logged([string]$LogFile, [string]$Exe, [string[]]$Arguments = @(), [string]$CommandLine = $null,
                      [hashtable]$Environment = $null, [string]$WorkingDirectory = $null) {
    New-Item -ItemType Directory -Force (Split-Path $LogFile -Parent) | Out-Null
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Exe
    $info.Arguments = if ($CommandLine) { $CommandLine } else { ConvertTo-CommandLine $Arguments }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    if ($WorkingDirectory) { $info.WorkingDirectory = $WorkingDirectory }
    if ($Environment) {
        foreach ($key in $Environment.Keys) { $info.EnvironmentVariables[$key] = [string]$Environment[$key] }
    }
    Set-Content -Path ($LogFile + '.cmdline') -Value ($Exe + ' ' + $info.Arguments) -Encoding UTF8   # for a failed step
    $process = [System.Diagnostics.Process]::Start($info)
    $out = [System.IO.File]::Create($LogFile)
    $err = [System.IO.File]::Create($LogFile + '.err')
    return [pscustomobject]@{
        Process = $process; LogFile = $LogFile; Out = $out; Err = $err
        OutTask = $process.StandardOutput.BaseStream.CopyToAsync($out)
        ErrTask = $process.StandardError.BaseStream.CopyToAsync($err)
    }
}

# Waits for the program, closes the log (stderr goes after stdout) and returns the exit code (-2: timeout, killed)
function Wait-Logged($Started, [int]$TimeoutSeconds = 0) {
    $process = $Started.Process
    $timedOut = $false
    if ($TimeoutSeconds -gt 0) {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            & taskkill.exe /T /F /PID $process.Id 2>&1 | Out-Null
        }
    } else {
        $process.WaitForExit()
    }
    $process.WaitForExit()
    [void]$Started.OutTask.Wait(10000)
    [void]$Started.ErrTask.Wait(10000)
    $Started.Out.Dispose()
    $Started.Err.Dispose()
    $errFile = $Started.LogFile + '.err'
    if (Test-Path $errFile) {
        if ((Get-Item $errFile).Length -gt 0) {
            $bytes = [System.IO.File]::ReadAllBytes($errFile)
            $stream = [System.IO.File]::Open($Started.LogFile, [System.IO.FileMode]::Append)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Dispose()
        }
        Remove-Item $errFile -ErrorAction SilentlyContinue
    }
    if ($timedOut) {
        Add-Content -Path $Started.LogFile -Value "[check.ps1] timeout after $TimeoutSeconds s, the process was killed"
        return -2
    }
    return [int]$process.ExitCode
}

# Runs a native command and waits: the exit code of the command
function Invoke-Logged([string]$LogFile, [string]$Exe, [string[]]$Arguments = @(), [hashtable]$Environment = $null,
                       [int]$TimeoutSeconds = 0, [string]$WorkingDirectory = $null, [string]$CommandLine = $null) {
    $started = Start-Logged $LogFile $Exe $Arguments $CommandLine $Environment $WorkingDirectory
    return (Wait-Logged $started $TimeoutSeconds)
}

function Find-Tool([string]$Name, [string[]]$Hints) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($hint in $Hints) {
        if ($hint -and (Test-Path $hint)) { return $hint }
    }
    return $null
}

function Get-VsInstall {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -property installationPath 2>$null | Select-Object -First 1
        if ($path) { return $path }
    }
    return $null
}

$script:VsInstall = Get-VsInstall
$script:Cmake = Find-Tool 'cmake' @((Join-Path $script:VsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'))
$script:Ctest = Find-Tool 'ctest' @((Join-Path $script:VsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'))
$script:Generator = 'Visual Studio 18 2026'
$mainCache = Join-Path $Root 'build\CMakeCache.txt'
if (Test-Path $mainCache) {
    $line = Select-String -Path $mainCache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
    if ($line) { $script:Generator = $line.Matches[0].Groups[1].Value }
}

# Our code is engine/, app/ and tests/ of this repository (it may itself live under .team/worktrees/)
function Get-RelativePath([string]$Path) {
    $backslash = [string][char]92
    $normalized = $Path.Replace('/', $backslash)
    if ($normalized.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase)) {
        return $normalized.Substring($Root.Length).TrimStart([char]92).Replace($backslash, '/')
    }
    # The log of a tool may have the Cyrillic part of the path in another code page, so the prefix does not match:
    # cut at the repository folder instead (it may be a worktree: myengine\.team\worktrees\<name>\)
    $m = [regex]::Match($normalized, '^.*?\\myengine\\(?:\.team\\worktrees\\[^\\]+\\)?(?<rel>.*)$')
    if ($m.Success) {
        return $m.Groups['rel'].Value.Replace($backslash, '/')
    }
    return $normalized.Replace($backslash, '/')
}

function Test-OurPath([string]$Path) {
    return (Get-RelativePath $Path) -match '^(engine|app|tests)/'
}

function Initialize-Tree([string]$BuildDir, [string[]]$ExtraArguments) {
    $log = Join-Path $BuildDir 'logs\configure.log'
    # Always: a configure of an existing tree is quick, and without it files added since the last run are not in
    # the project (the regeneration at build time is off, CMAKE_SUPPRESS_REGENERATION)
    $arguments = @('-S', $Root, '-B', $BuildDir, '-G', $script:Generator, '-A', 'x64') + $ExtraArguments
    $code = Invoke-Logged $log $script:Cmake $arguments
    return $code -eq 0
}

# Warnings and errors of MSVC / MSBuild in a log: de-duplicated, with the path of the file
function Read-CompilerDiagnostics([string]$LogFile) {
    $items = @{}
    if (-not (Test-Path $LogFile)) { return @() }
    $pattern = '^\s*(?<file>[A-Za-z]:[^()]+?)\((?<line>\d+)(?:,\d+)?\)\s*:\s*(?<kind>warning|error)\s+(?<code>[A-Z]+\d+)\s*:\s*(?<text>.*)$'
    # cl.exe writes UTF-8 (/utf-8), and so does the redirected log
    foreach ($line in Get-Content $LogFile -Encoding UTF8 -ErrorAction SilentlyContinue) {
        $m = [regex]::Match($line, $pattern)
        if (-not $m.Success) { continue }
        $file = $m.Groups['file'].Value
        $key = '{0}|{1}|{2}|{3}' -f $file, $m.Groups['line'].Value, $m.Groups['kind'].Value, $m.Groups['code'].Value
        if (-not $items.ContainsKey($key)) {
            $items[$key] = [pscustomobject]@{
                File = $file; Line = [int]$m.Groups['line'].Value; Kind = $m.Groups['kind'].Value
                Code = $m.Groups['code'].Value; Text = $m.Groups['text'].Value.Trim()
            }
        }
    }
    return @($items.Values)
}

function Read-Baseline([string]$Name) {
    $path = Join-Path $BaselineDir $Name
    $map = @{}
    if (Test-Path $path) {
        foreach ($line in Get-Content $path) {
            if ($line -match '^\s*#' -or $line.Trim() -eq '') { continue }
            $parts = $line.Split('|')
            if ($parts.Count -ge 3) { $map[($parts[0] + '|' + $parts[1])] = [int]$parts[2] }
        }
    }
    return $map
}

function Write-Baseline([string]$Name, [hashtable]$Counts, [string]$Header) {
    New-Item -ItemType Directory -Force $BaselineDir | Out-Null
    $lines = @('# ' + $Header, '# key|file|count; regenerate with tools\check.ps1 -UpdateBaseline')
    foreach ($key in ($Counts.Keys | Sort-Object)) { $lines += ('{0}|{1}' -f $key, $Counts[$key]) }
    Set-Content -Path (Join-Path $BaselineDir $Name) -Value $lines -Encoding ASCII
}

# Findings (key = code|relative file) against the baseline; returns the new ones
function Compare-ToBaseline([hashtable]$Counts, [hashtable]$Baseline) {
    $new = @()
    foreach ($key in $Counts.Keys) {
        $allowed = 0
        if ($Baseline.ContainsKey($key)) { $allowed = $Baseline[$key] }
        if ($Counts[$key] -gt $allowed) { $new += ('{0}|{1}' -f $key, ($Counts[$key] - $allowed)) }
    }
    return $new
}

function Read-CtestSummary([string]$LogFile) {
    $text = Get-Content $LogFile -Raw -ErrorAction SilentlyContinue
    if ($text -match '(\d+)% tests passed, (\d+) tests failed out of (\d+)') {
        return [pscustomobject]@{ Failed = [int]$Matches[2]; Total = [int]$Matches[3] }
    }
    # newer ctest does not print the failed count when everything passed
    if ($text -match '100% tests passed out of (\d+)') {
        return [pscustomobject]@{ Failed = 0; Total = [int]$Matches[1] }
    }
    return $null
}

# ---- steps -----------------------------------------------------------------------------------------------------

function Build-AndTest([string]$StepName, [string]$BuildDir, [string[]]$ConfigureArguments, [hashtable]$TestEnvironment) {
    $logs = Join-Path $BuildDir 'logs'
    if (-not (Initialize-Tree $BuildDir $ConfigureArguments)) {
        Add-Result "$StepName configure" $false 'cmake configure failed' (Join-Path $logs 'configure.log')
        return $false
    }

    $buildLog = Join-Path $logs "$StepName-build.log"
    $code = Invoke-Logged $buildLog $script:Cmake @('--build', $BuildDir, '--config', $Config)
    $diagnostics = Read-CompilerDiagnostics $buildLog
    $ours = @($diagnostics | Where-Object { Test-OurPath $_.File })
    $errors = @($diagnostics | Where-Object { $_.Kind -eq 'error' }).Count
    $warnings = @($ours | Where-Object { $_.Kind -eq 'warning' })
    $ok = ($code -eq 0)
    $detail = if ($ok) { 'built' } else { "build failed (exit $code, $errors errors)" }
    $detail += ", warnings in our code: $($warnings.Count)"
    Add-Result "$StepName build" ($ok -and $warnings.Count -eq 0) $detail $buildLog
    if (-not $ok) { return $false }

    $testLog = Join-Path $logs "$StepName-ctest.log"
    $code = Invoke-Logged $testLog $script:Ctest @('--test-dir', $BuildDir, '-C', $Config, '--output-on-failure') $TestEnvironment 1200
    $summary = Read-CtestSummary $testLog
    if ($summary) {
        Add-Result "$StepName ctest" ($summary.Failed -eq 0 -and $code -eq 0) ("{0}/{1} passed" -f ($summary.Total - $summary.Failed), $summary.Total) $testLog
    } else {
        Add-Result "$StepName ctest" $false "ctest exit $code, no summary" $testLog
    }
    return ($code -eq 0)
}

function Invoke-Quick {
    [void](Build-AndTest 'quick' (Join-Path $Root 'build') @() @{})
}

# A GUI program of ours under ASan: finished by itself with exit code 0 and no ASan report
function Invoke-AsanProgram([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$LogsDir, [hashtable]$Environment) {
    if (-not (Test-Path $Exe)) {
        Add-Result "asan $Name" $false 'executable not found' $Exe
        return
    }
    $log = Join-Path $LogsDir "asan-$Name.log"
    $reportBase = Join-Path $LogsDir "asan-$Name-report"
    Get-ChildItem $LogsDir -Filter "asan-$Name-report*" -ErrorAction SilentlyContinue | Remove-Item -Force
    $environmentWithLog = @{}
    foreach ($key in $Environment.Keys) { $environmentWithLog[$key] = $Environment[$key] }
    # (the path is quoted: a drive letter has a colon and ':' separates the options)
    $environmentWithLog['ASAN_OPTIONS'] = $Environment['ASAN_OPTIONS'] + ":log_path='" + $reportBase + "'"
    $code = Invoke-Logged $log $Exe $Arguments $environmentWithLog 240 (Split-Path $Exe -Parent)
    $reports = @(Get-ChildItem $LogsDir -Filter "asan-$Name-report*" -ErrorAction SilentlyContinue)
    if ($reports.Count -gt 0) {
        Add-Result "asan $Name" $false ("AddressSanitizer report, exit $code") $reports[0].FullName
    } elseif ($code -ne 0) {
        Add-Result "asan $Name" $false ("exit code $code") $log
    } else {
        Add-Result "asan $Name" $true 'exit 0, no reports' $log
    }
}

function Invoke-Asan {
    $buildDir = Join-Path $Root 'build-asan'
    $logs = Join-Path $buildDir 'logs'
    $environment = @{ 'ASAN_OPTIONS' = 'abort_on_error=0:halt_on_error=1:detect_stack_use_after_return=0' }
    $built = Build-AndTest 'asan' $buildDir @('-DMYENGINE_ASAN=ON') $environment
    if (-not $built -and -not (Test-Path (Join-Path $buildDir "app\$Config\myengine.exe"))) { return }

    $smokeDir = Join-Path $buildDir "tests\$Config"
    Invoke-AsanProgram 'gameplay-smoke' (Join-Path $smokeDir 'myengine_gameplay_smoke.exe') @() $logs $environment
    Invoke-AsanProgram 'scripting-smoke' (Join-Path $smokeDir 'myengine_scripting_smoke.exe') @() $logs $environment

    # The editor writes the map back on exit: it gets a copy, not the file of the repository
    $scene = Join-Path $logs 'coin_guard_demo.json'
    Copy-Item (Join-Path $Root 'assets\scenes\coin_guard_demo.json') $scene -Force
    $editorArguments = @('--scene', $scene, '--play', '--exit-after-frames', [string]$AsanFrames)
    Invoke-AsanProgram 'editor-play' (Join-Path $buildDir "app\$Config\myengine.exe") $editorArguments $logs $environment
}

function Invoke-Analyze {
    $buildDir = Join-Path $Root 'build-analyze'
    $logs = Join-Path $buildDir 'logs'
    if (-not (Initialize-Tree $buildDir @('-DMYENGINE_ANALYZE=ON'))) {
        Add-Result 'analyze configure' $false 'cmake configure failed' (Join-Path $logs 'configure.log')
        return
    }
    $buildLog = Join-Path $logs 'analyze-build.log'
    if ($ReuseLog -and (Test-Path $buildLog)) {
        $code = 0   # an up-to-date build prints no warnings: the log of the last real build is parsed
    } else {
        # Warnings exist only for files that are compiled: the whole tree is rebuilt, or an incremental build would
        # show a handful of findings and look green
        $code = Invoke-Logged $buildLog $script:Cmake @('--build', $buildDir, '--config', $Config, '--clean-first')
    }
    $diagnostics = Read-CompilerDiagnostics $buildLog
    $errors = @($diagnostics | Where-Object { $_.Kind -eq 'error' -and (Test-OurPath $_.File) })
    $findings = @($diagnostics | Where-Object { $_.Kind -eq 'warning' -and (Test-OurPath $_.File) -and $_.Code -match '^C(6|28)\d{3}$' })
    $others = @($diagnostics | Where-Object { $_.Kind -eq 'warning' -and (Test-OurPath $_.File) -and $_.Code -notmatch '^C(6|28)\d{3}$' })

    $listFile = Join-Path $logs 'analyze-findings.txt'
    $findings | Sort-Object File, Line | ForEach-Object {
        '{0}({1}): {2} {3}' -f (Get-RelativePath $_.File), $_.Line, $_.Code, $_.Text
    } | Set-Content -Path $listFile -Encoding UTF8

    if ($code -ne 0 -or $errors.Count -gt 0) {
        Add-Result 'analyze build' $false "build failed (exit $code, $($errors.Count) errors in our code)" $buildLog
        return
    }

    $counts = @{}
    foreach ($item in $findings) {
        $key = '{0}|{1}' -f $item.Code, (Get-RelativePath $item.File)
        if ($counts.ContainsKey($key)) { $counts[$key]++ } else { $counts[$key] = 1 }
    }
    if ($UpdateBaseline) {
        Write-Baseline 'analyze.txt' $counts 'MSVC /analyze findings in engine/, app/, tests/ (code|file|count)'
        Add-Result 'analyze' $true ("baseline written: {0} findings in {1} groups" -f $findings.Count, $counts.Count) $listFile
        return
    }
    $new = Compare-ToBaseline $counts (Read-Baseline 'analyze.txt')
    $detail = "{0} findings ({1} other warnings), new vs baseline: {2}" -f $findings.Count, $others.Count, $new.Count
    Add-Result 'analyze' ($new.Count -eq 0) $detail $listFile
    $new | Select-Object -First 5 | ForEach-Object { Write-Host ('       new: ' + $_) }
}

function Invoke-Tidy {
    $clangTidy = Find-Tool 'clang-tidy' @(
        (Join-Path $script:VsInstall 'VC\Tools\Llvm\x64\bin\clang-tidy.exe'),
        'C:\Program Files\LLVM\bin\clang-tidy.exe')
    if (-not $clangTidy) {
        Add-Result 'tidy' $false 'clang-tidy.exe not found (VS: C++ Clang tools for Windows)' ''
        return
    }

    $buildDir = Join-Path $Root 'build-tidy'
    $logs = Join-Path $buildDir 'logs'
    $database = Join-Path $buildDir 'compile_commands.json'
    # Configured every time (a few seconds): files added since the last run have to be in the database
    if ($true) {
        # Ninja gives a compile_commands.json; it needs the MSVC environment of a developer prompt
        $devCmd = Join-Path $script:VsInstall 'Common7\Tools\VsDevCmd.bat'
        if (-not (Test-Path $devCmd)) {
            Add-Result 'tidy configure' $false 'VsDevCmd.bat not found' ''
            return
        }
        New-Item -ItemType Directory -Force $logs | Out-Null
        $log = Join-Path $logs 'configure.log'
        # cmd /c with an outer pair of quotes: VsDevCmd sets up the MSVC environment, then cmake runs in it
        $line = '/d /c ""{0}" -arch=x64 -host_arch=x64 >nul && cmake -S "{1}" -B "{2}" -G Ninja -DCMAKE_BUILD_TYPE={3} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"' -f $devCmd, $Root, $buildDir, $Config
        $code = Invoke-Logged $log 'cmd.exe' @() $null 0 $null $line
        if ($code -ne 0 -or -not (Test-Path $database)) {
            Add-Result 'tidy configure' $false "cmake -G Ninja failed (exit $code)" $log
            return
        }
    }

    # Our translation units, from the database
    # UTF-8 explicitly: Windows PowerShell reads a file without BOM as ANSI and the Cyrillic path becomes mojibake
    $entries = [System.IO.File]::ReadAllText($database, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $files = @($entries | ForEach-Object { ($_.file -replace '/', '\') } | Where-Object { $_ -match '\.cpp$' -and (Test-OurPath $_) } | Sort-Object -Unique)
    if ($files.Count -eq 0) {
        Add-Result 'tidy' $false 'no translation units of ours in the database' $database
        return
    }

    $workers = [Math]::Max(1, [Math]::Min(8, [Environment]::ProcessorCount))
    $buckets = @(for ($i = 0; $i -lt $workers; $i++) { , (New-Object System.Collections.ArrayList) })
    for ($i = 0; $i -lt $files.Count; $i++) { [void]$buckets[$i % $workers].Add($files[$i]) }

    $jobs = @()
    for ($i = 0; $i -lt $workers; $i++) {
        if ($buckets[$i].Count -eq 0) { continue }
        $out = Join-Path $logs ("tidy-part{0}.log" -f $i)
        $arguments = @('-p', $buildDir, '--quiet') + @($buckets[$i])
        $jobs += Start-Logged $out $clangTidy $arguments
    }
    $brokenParts = @()
    foreach ($job in $jobs) {
        [void](Wait-Logged $job 3600)
        # An invalid invocation or an unreadable file prints usage or "Error ... does not exist": not a result
        $head = (Get-Content $job.LogFile -Encoding UTF8 -TotalCount 40 -ErrorAction SilentlyContinue) -join "`n"
        if ($head -match 'USAGE: clang-tidy|Error while processing|Skipping .*Compile command not found|does not exist') {
            $brokenParts += $job.LogFile
        }
    }
    if ($brokenParts.Count -gt 0) {
        Add-Result 'tidy' $false ("clang-tidy did not process the files ({0} of {1} logs)" -f $brokenParts.Count, $jobs.Count) $brokenParts[0]
        return
    }

    $findings = @{}
    $pattern = '^(?<file>[A-Za-z]:[^:]+?):(?<line>\d+):(?<col>\d+):\s*(?<kind>warning|error):\s*(?<text>.*?)\s*\[(?<check>[^\]]+)\]\s*$'
    $seen = @{}
    foreach ($job in $jobs) {
        foreach ($line in Get-Content $job.LogFile -Encoding UTF8 -ErrorAction SilentlyContinue) {
            $m = [regex]::Match($line, $pattern)
            if (-not $m.Success) { continue }
            $file = $m.Groups['file'].Value
            if (-not (Test-OurPath $file)) { continue }
            $unique = '{0}:{1}:{2}:{3}' -f $file, $m.Groups['line'].Value, $m.Groups['col'].Value, $m.Groups['check'].Value
            if ($seen.ContainsKey($unique)) { continue }
            $seen[$unique] = '{0}({1}): {2} {3}' -f (Get-RelativePath $file), $m.Groups['line'].Value, $m.Groups['check'].Value, $m.Groups['text'].Value
            $key = '{0}|{1}' -f $m.Groups['check'].Value, (Get-RelativePath $file)
            if ($findings.ContainsKey($key)) { $findings[$key]++ } else { $findings[$key] = 1 }
        }
    }

    $listFile = Join-Path $logs 'tidy-findings.txt'
    $seen.Values | Sort-Object | Set-Content -Path $listFile -Encoding UTF8
    if ($UpdateBaseline) {
        Write-Baseline 'tidy.txt' $findings 'clang-tidy findings in engine/, app/, tests/ (check|file|count)'
        Add-Result 'tidy' $true ("baseline written: {0} findings in {1} groups" -f $seen.Count, $findings.Count) $listFile
        return
    }
    $new = Compare-ToBaseline $findings (Read-Baseline 'tidy.txt')
    Add-Result 'tidy' ($new.Count -eq 0) ("{0} files, {1} findings, new vs baseline: {2}" -f $files.Count, $seen.Count, $new.Count) $listFile
    $new | Select-Object -First 5 | ForEach-Object { Write-Host ('       new: ' + $_) }
}

# ---- main ------------------------------------------------------------------------------------------------------

if (-not $script:Cmake -or -not $script:Ctest) {
    Write-Host '[FAIL] cmake / ctest not found (Visual Studio CMake tools or PATH)'
    exit 1
}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
if ($Quick) { Invoke-Quick }
if ($Asan) { Invoke-Asan }
if ($Analyze) { Invoke-Analyze }
if ($Tidy) { Invoke-Tidy }
$stopwatch.Stop()

$failedSteps = @($script:Results | Where-Object { -not $_.Ok })
Write-Host ''
if ($failedSteps.Count -eq 0) {
    Write-Host ('RESULT: OK, {0} steps, {1:n0} s' -f $script:Results.Count, $stopwatch.Elapsed.TotalSeconds)
    exit 0
}
Write-Host ('RESULT: FAIL, {0} of {1} steps failed, {2:n0} s' -f $failedSteps.Count, $script:Results.Count, $stopwatch.Elapsed.TotalSeconds)
exit 1
