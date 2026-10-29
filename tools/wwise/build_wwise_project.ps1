<#
.SYNOPSIS
  Builds the Wwise project of Rocket Arena (wwise/RocketArena) from assets/data/audio_bank.json and the sound banks of the game.

.DESCRIPTION
  The events of the game are described in assets/data/audio_bank.json (the module audio of the engine plays them by name). This script turns the same file
  into a Wwise project: WwiseConsole creates an empty project, WwiseConsole waapi-server opens it, and the content goes in through WAAPI over HTTP
  (127.0.0.1:<HttpPort>), so that nothing is drawn by hand. Then the SoundBanks are generated and the two .bnk files the game needs are copied to
  assets/audio/wwise/.  docs/engine/wwise-project.md says what field of the json becomes what object of Wwise.

  Windows PowerShell 5.1, no packages; the Python of external/ is not used. Wwise itself is looked for in -WwiseRoot, then in $env:WWISEROOT, then in
  HKCU\Environment\WWISEROOT (a session started before the installation of Wwise does not see the variable, the registry has it).

.PARAMETER Rebuild
  Deletes wwise/RocketArena, creates the project again, fills it, generates the banks and verifies them.

.PARAMETER BanksOnly
  Only generates the banks of the existing project (after a change made by hand in Wwise Authoring) and verifies them.

.PARAMETER Verify
  Checks the project and the banks against audio_bank.json and changes nothing. The exit code is not 0 when something is missing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\wwise\build_wwise_project.ps1 -Rebuild
#>
[CmdletBinding()]
param(
    [switch]$Rebuild,
    [switch]$BanksOnly,
    [switch]$Verify,
    [string]$WwiseRoot = "",
    [int]$HttpPort = 8090
)

$ErrorActionPreference = "Stop"
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$inv = [Globalization.CultureInfo]::InvariantCulture

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$bankJsonPath = Join-Path $repo "assets\data\audio_bank.json"
$projectDir = Join-Path $repo "wwise\RocketArena"
$projectFile = Join-Path $projectDir "RocketArena.wproj"
$outputDir = Join-Path $repo "assets\audio\wwise"
$bankName = "RocketArena"
$platform = "Windows"

# the names of the Game Parameters of the game and what they are for (docs/engine/wwise-project.md)
$gameParameterNames = @("Pitch", "Volume", "Bus_Master", "Bus_Sfx", "Bus_Music", "Bus_Ui")
$busNames = @{ sfx = "Sfx"; music = "Music"; ui = "Ui" }

function Write-Step([string]$text) { Write-Host "[wwise] $text" }
function Fail([string]$text) { Write-Host "[wwise] ERROR: $text" -ForegroundColor Red; exit 1 }

if (-not ($Rebuild -or $BanksOnly -or $Verify)) {
    Write-Host "usage: build_wwise_project.ps1 -Rebuild | -BanksOnly | -Verify   [-WwiseRoot <dir>] [-HttpPort <port>]"
    Write-Host "  -Rebuild    create wwise/RocketArena from assets/data/audio_bank.json and generate the banks"
    Write-Host "  -BanksOnly  generate the banks of the existing project (after changes made in Wwise Authoring)"
    Write-Host "  -Verify     check the project and the banks against audio_bank.json"
    exit 2
}
if (($Rebuild -and $BanksOnly) -or ($Verify -and ($Rebuild -or $BanksOnly))) { Fail "choose one of -Rebuild, -BanksOnly, -Verify" }

# ---------------------------------------------------------------------------------------------------------------------------------- the file of the game

function Read-AudioBank {
    if (-not (Test-Path $bankJsonPath)) { Fail "no $bankJsonPath" }
    $json = Get-Content -Raw -Encoding UTF8 -LiteralPath $bankJsonPath | ConvertFrom-Json
    $directory = if ($json.directory) { [string]$json.directory } else { "assets/audio" }
    $audioDir = Join-Path $repo ($directory -replace "/", "\")
    $events = @()
    foreach ($p in $json.events.PSObject.Properties) {
        $e = $p.Value
        $files = @()
        if ($e.files) { $files = @($e.files | ForEach-Object { [string]$_ }) } elseif ($e.file) { $files = @([string]$e.file) }
        if ($files.Count -eq 0) { Fail "event '$($p.Name)' has no file" }
        foreach ($f in $files) { if (-not (Test-Path (Join-Path $audioDir $f))) { Fail "event '$($p.Name)': no file $audioDir\$f" } }
        $bus = if ($e.bus) { ([string]$e.bus).ToLowerInvariant() } else { "sfx" }
        if (-not $busNames.ContainsKey($bus)) { Fail "event '$($p.Name)': unknown bus '$bus' (sfx, music or ui)" }
        $volume = if ($null -ne $e.volume) { [double]$e.volume } else { 1.0 }
        $pitch = if ($null -ne $e.pitch) { [double]$e.pitch } else { 1.0 }
        $events += [pscustomobject]@{
            Name         = [string]$p.Name
            Files        = $files
            Bus          = $busNames[$bus]
            Volume       = $volume
            VolumeMin    = if ($null -ne $e.volumeMin) { [double]$e.volumeMin } else { $volume }
            VolumeMax    = if ($null -ne $e.volumeMax) { [double]$e.volumeMax } else { $volume }
            PitchMin     = if ($null -ne $e.pitchMin) { [double]$e.pitchMin } else { $pitch }
            PitchMax     = if ($null -ne $e.pitchMax) { [double]$e.pitchMax } else { $pitch }
            Loop         = [bool]($e.loop -eq $true)
            Spatial      = [bool]($e.spatial -eq $true)
            MinDistance  = if ($null -ne $e.minDistance) { [double]$e.minDistance } else { 2.0 }
            MaxDistance  = if ($null -ne $e.maxDistance) { [double]$e.maxDistance } else { 80.0 }
            Rolloff      = if ($null -ne $e.rolloff) { [double]$e.rolloff } else { 1.0 }
            Priority     = if ($null -ne $e.priority) { [int]$e.priority } else { 0 }
            MaxInstances = if ($null -ne $e.maxInstances) { [int]$e.maxInstances } else { 0 }
        }
    }
    return [pscustomobject]@{ AudioDir = $audioDir; Events = $events }
}

function ToDb([double]$gain) { return [Math]::Round(20.0 * [Math]::Log10($gain), 3) }
function ToCents([double]$ratio) { return [Math]::Round(1200.0 * [Math]::Log($ratio, 2.0), 2) }
function Num([double]$value) { return $value.ToString("0.###", $inv) }

# ---------------------------------------------------------------------------------------------------------------------------------- Wwise

function Find-WwiseRoot {
    $candidates = @()
    if ($WwiseRoot) { $candidates += $WwiseRoot }
    if ($env:WWISEROOT) { $candidates += $env:WWISEROOT }
    try { $reg = (Get-ItemProperty -Path "HKCU:\Environment" -Name WWISEROOT -ErrorAction Stop).WWISEROOT; if ($reg) { $candidates += $reg } } catch { }
    foreach ($c in $candidates) {
        $exe = Join-Path $c "Authoring\x64\Release\bin\WwiseConsole.exe"
        if (Test-Path $exe) { return $c }
    }
    Fail "Wwise is not found: pass -WwiseRoot <dir> (the folder that has Authoring\x64\Release\bin\WwiseConsole.exe), or set WWISEROOT"
}

$script:Console = $null
function Console-Path {
    if (-not $script:Console) { $script:Console = Join-Path (Find-WwiseRoot) "Authoring\x64\Release\bin\WwiseConsole.exe" }
    return $script:Console
}

# One call of WAAPI over HTTP: the server wants uri, options and args in every request
$script:WaapiUrl = "http://127.0.0.1:$HttpPort/waapi"
function Waapi([string]$uri, $waapiArgs = @{}, $waapiOptions = @{}) {
    $body = [ordered]@{ uri = $uri; options = $waapiOptions; args = $waapiArgs }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($body | ConvertTo-Json -Depth 40 -Compress))
    $request = [Net.WebRequest]::Create($script:WaapiUrl)
    $request.Method = "POST"
    $request.ContentType = "application/json"
    $request.ContentLength = $bytes.Length
    $request.Timeout = 120000
    $stream = $request.GetRequestStream()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Close()
    try {
        $response = $request.GetResponse()
    } catch [Net.WebException] {
        $r = $_.Exception.Response
        $text = if ($r) { (New-Object IO.StreamReader($r.GetResponseStream())).ReadToEnd() } else { $_.Exception.Message }
        throw "WAAPI $uri failed: $text"
    }
    $text = (New-Object IO.StreamReader($response.GetResponseStream(), [Text.Encoding]::UTF8)).ReadToEnd()
    $response.Close()
    if ($text) { return ($text | ConvertFrom-Json) }
    return $null
}

function Start-WaapiServer {
    $alive = $false
    try { Waapi "ak.wwise.core.getInfo" | Out-Null; $alive = $true } catch { }
    if ($alive) { Fail "something already answers WAAPI on port $HttpPort (Wwise Authoring is open?). Close it or pass -HttpPort <other port>" }
    $log = Join-Path $env:TEMP "rocketarena_waapi"
    $process = Start-Process -FilePath (Console-Path) -ArgumentList @("waapi-server", "`"$projectFile`"", "--http-port", "$HttpPort", "--quiet") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput "$log.out" -RedirectStandardError "$log.err"
    for ($i = 0; $i -lt 180; $i++) {
        if ($process.HasExited) { Fail "waapi-server stopped at once (exit code $($process.ExitCode)); see $log.out" }
        try { Waapi "ak.wwise.core.getInfo" | Out-Null; return $process } catch { Start-Sleep -Seconds 1 }
    }
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Fail "waapi-server did not answer in three minutes; see $log.out"
}

function Stop-WaapiServer($process) {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(30000) | Out-Null
    }
}

# ---------------------------------------------------------------------------------------------------------------------------------- the content

function Add-ProjectContent($bank) {
    $events = $bank.Events
    $actor = "\Containers\Default Work Unit"
    $mainBus = "\Busses\Default Work Unit\Main Audio Bus"

    # buses: Main Audio Bus > Sfx, Music, Ui
    Write-Step "buses"
    foreach ($name in "Sfx", "Music", "Ui") {
        Waapi "ak.wwise.core.object.create" @{ parent = $mainBus; type = "Bus"; name = $name; onNameConflict = "fail" } | Out-Null
    }

    # Game Parameters and the Real-Time Parameter Controls that they drive
    Write-Step "game parameters"
    $gpRanges = @{ Pitch = @(0.25, 4.0); Volume = @(0.0, 4.0); Bus_Master = @(0.0, 1.0); Bus_Sfx = @(0.0, 1.0); Bus_Music = @(0.0, 1.0); Bus_Ui = @(0.0, 1.0) }
    foreach ($name in $gameParameterNames) {
        $path = "\Game Parameters\Default Work Unit\$name"
        Waapi "ak.wwise.core.object.create" @{ parent = "\Game Parameters\Default Work Unit"; type = "GameParameter"; name = $name; onNameConflict = "fail" } | Out-Null
        Waapi "ak.wwise.core.object.setProperty" @{ object = $path; property = "Min"; value = $gpRanges[$name][0] } | Out-Null
        Waapi "ak.wwise.core.object.setProperty" @{ object = $path; property = "Max"; value = $gpRanges[$name][1] } | Out-Null
        Waapi "ak.wwise.core.object.setProperty" @{ object = $path; property = "InitialValue"; value = 1.0 } | Out-Null
    }
    $silent = -200.0   # the bottom of a volume curve: nothing is heard
    $gainSteps = @(0.01, 0.03, 0.06, 0.1, 0.18, 0.3, 0.45, 0.6, 0.8, 1.0)
    $volumeCurve = @(@{ x = 0.0; y = $silent; shape = "Linear" }) + @($gainSteps | ForEach-Object { @{ x = $_; y = (ToDb $_); shape = "Linear" } })
    $volumeCurveWide = @(@{ x = 0.0; y = $silent; shape = "Linear" }) + @(($gainSteps + @(1.5, 2.0, 3.0, 4.0)) | ForEach-Object { @{ x = $_; y = (ToDb $_); shape = "Linear" } })
    $pitchRatios = @(0.25, 0.3, 0.35, 0.42, 0.5, 0.6, 0.7, 0.84, 1.0, 1.2, 1.4, 1.7, 2.0, 2.4, 2.8, 3.4, 4.0)
    $pitchCurve = @($pitchRatios | ForEach-Object { @{ x = $_; y = (ToCents $_); shape = "Linear" } })
    $rtpcObjects = @()
    foreach ($bus in "Sfx", "Music", "Ui") {
        $path = "$mainBus\$bus"
        # Pitch and Volume work on the voices of every bus (the voice property, in cents / in dB), Bus_<name> on the volume of the bus itself
        $rtpcObjects += @{ object = $path; "@RTPC" = @(
                @{ type = "RTPC"; name = ""; "@PropertyName" = "Pitch"; "@ControlInput" = "\Game Parameters\Default Work Unit\Pitch"; "@Curve" = @{ type = "Curve"; points = $pitchCurve } },
                @{ type = "RTPC"; name = ""; "@PropertyName" = "Volume"; "@ControlInput" = "\Game Parameters\Default Work Unit\Volume"; "@Curve" = @{ type = "Curve"; points = $volumeCurveWide } },
                @{ type = "RTPC"; name = ""; "@PropertyName" = "BusVolume"; "@ControlInput" = "\Game Parameters\Default Work Unit\Bus_$bus"; "@Curve" = @{ type = "Curve"; points = $volumeCurve } }) }
    }
    $rtpcObjects += @{ object = $mainBus; "@RTPC" = @(
            @{ type = "RTPC"; name = ""; "@PropertyName" = "BusVolume"; "@ControlInput" = "\Game Parameters\Default Work Unit\Bus_Master"; "@Curve" = @{ type = "Curve"; points = $volumeCurve } }) }
    Waapi "ak.wwise.core.object.set" @{ objects = $rtpcObjects } | Out-Null

    # attenuations: one share set for every pair (minDistance, maxDistance): full volume up to min, then min / distance as in the engine (rolloff 1),
    # and the last fifth of the way to max fades out smoothly (AudioSpatial.h)
    Write-Step "attenuations"
    $pairs = @{}
    foreach ($e in $events) { if ($e.Spatial) { $pairs["Att_$(Num $e.MinDistance)_$(Num $e.MaxDistance)"] = $e } }
    foreach ($name in ($pairs.Keys | Sort-Object)) {
        $e = $pairs[$name]
        $min = $e.MinDistance; $max = $e.MaxDistance
        $path = "\Attenuations\Default Work Unit\$name"
        Waapi "ak.wwise.core.object.create" @{ parent = "\Attenuations\Default Work Unit"; type = "Attenuation"; name = $name; onNameConflict = "fail" } | Out-Null
        Waapi "ak.wwise.core.object.setProperty" @{ object = $path; property = "RadiusMax"; value = $max } | Out-Null
        $points = @(@{ x = 0.0; y = 0.0; shape = "Linear" }, @{ x = $min; y = 0.0; shape = "Linear" })
        $fadeStart = $min + 0.8 * ($max - $min)
        $distances = @()
        $d = $min
        while ($true) {
            $d = $d * 1.6
            if ($d -ge $fadeStart) { break }
            $distances += $d
        }
        $distances += $fadeStart
        foreach ($distance in $distances) {
            $points += @{ x = [Math]::Round($distance, 3); y = (ToDb ([Math]::Pow($min / $distance, $e.Rolloff))); shape = "Linear" }
        }
        foreach ($t in 0.25, 0.5, 0.75) {   # the fade: the gain of the law times 1 - smoothstep
            $distance = $fadeStart + $t * ($max - $fadeStart)
            $fade = 1.0 - ($t * $t * (3.0 - 2.0 * $t))
            $points += @{ x = [Math]::Round($distance, 3); y = (ToDb ([Math]::Pow($min / $distance, $e.Rolloff) * $fade)); shape = "Linear" }
        }
        $points += @{ x = $max; y = $silent; shape = "Linear" }
        Waapi "ak.wwise.core.object.setAttenuationCurve" @{ object = $path; curveType = "VolumeDryUsage"; use = "Custom"; points = $points } | Out-Null
    }

    # sounds: every wav of the events; several files of an event go into a Random Container
    Write-Step "import of $((($events | ForEach-Object { $_.Files.Count }) | Measure-Object -Sum).Sum) files"
    $imports = @()
    foreach ($e in $events) {
        if ($e.Files.Count -eq 1) {
            $imports += @{ audioFile = (Join-Path $bank.AudioDir $e.Files[0]); objectPath = "$actor\<Sound SFX>$($e.Name)" }
        } else {
            foreach ($f in $e.Files) {
                $imports += @{ audioFile = (Join-Path $bank.AudioDir $f); objectPath = "$actor\<Random Container>$($e.Name)\<Sound SFX>$([IO.Path]::GetFileNameWithoutExtension($f))" }
            }
        }
    }
    Waapi "ak.wwise.core.audio.import" @{ importOperation = "createNew"; default = @{ importLanguage = "SFX" }; imports = $imports } | Out-Null

    # the properties of the top object of every event (a Sound, or the Random Container over its sounds; the sounds under a container take its settings)
    Write-Step "properties of the events"
    $sets = @()
    foreach ($e in $events) {
        $top = "$actor\$($e.Name)"
        $o = @{ object = $top
                "@Volume" = (ToDb (($e.VolumeMin + $e.VolumeMax) / 2.0))
                "@OverrideOutput" = $true
                "@OutputBus" = "$mainBus\$($e.Bus)"
                "@OverridePriority" = $true
                "@Priority" = $e.Priority }
        $centreCents = ((ToCents $e.PitchMin) + (ToCents $e.PitchMax)) / 2.0
        if ($centreCents -ne 0.0) { $o["@Pitch"] = $centreCents }
        if ($e.Loop) {
            if ($e.Files.Count -eq 1) { $o["@IsLoopingEnabled"] = $true; $o["@IsLoopingInfinite"] = $true }
            else { $o["@PlayMechanismLoop"] = $true; $o["@PlayMechanismInfiniteOrNumberOfLoops"] = 1 }
        }
        if ($e.Files.Count -gt 1) {
            # a random variant, never the one that has just played (the Standard mode, avoid repeating the last one)
            $o["@RandomOrSequence"] = 1; $o["@NormalOrShuffle"] = 0; $o["@RandomAvoidRepeating"] = $true; $o["@RandomAvoidRepeatingCount"] = 1
        }
        if ($e.MaxInstances -gt 0) {
            # at most N at once in the whole game; what happens when it is full is left to the defaults of Wwise (MaxReachedBehavior, OverLimitBehavior); the engine drops the oldest
            $o["@UseMaxSoundPerInstance"] = $true; $o["@MaxSoundPerInstance"] = $e.MaxInstances; $o["@IsGlobalLimit"] = 1
        }
        if ($e.Spatial) {
            $o["@OverridePositioning"] = $true; $o["@3DSpatialization"] = 1; $o["@3DPosition"] = 0
            $o["@EnableAttenuation"] = $true; $o["@Attenuation"] = "\Attenuations\Default Work Unit\Att_$(Num $e.MinDistance)_$(Num $e.MaxDistance)"
        }
        $sets += $o
    }
    Waapi "ak.wwise.core.object.set" @{ objects = $sets } | Out-Null
    foreach ($e in $events) {
        $top = "$actor\$($e.Name)"
        if ($e.PitchMax -gt $e.PitchMin) {
            $centre = ((ToCents $e.PitchMin) + (ToCents $e.PitchMax)) / 2.0
            Waapi "ak.wwise.core.object.setRandomizer" @{ object = $top; property = "Pitch"; enabled = $true; min = [Math]::Round((ToCents $e.PitchMin) - $centre, 2); max = [Math]::Round((ToCents $e.PitchMax) - $centre, 2) } | Out-Null
        }
        if ($e.VolumeMax -gt $e.VolumeMin) {
            $centreDb = (ToDb (($e.VolumeMin + $e.VolumeMax) / 2.0))
            Waapi "ak.wwise.core.object.setRandomizer" @{ object = $top; property = "Volume"; enabled = $true; min = [Math]::Round((ToDb $e.VolumeMin) - $centreDb, 3); max = [Math]::Round((ToDb $e.VolumeMax) - $centreDb, 3) } | Out-Null
        }
    }

    # events: Play_<name> plays the top object of the event
    Write-Step "events"
    foreach ($e in $events) {
        Waapi "ak.wwise.core.object.create" @{ parent = "\Events\Default Work Unit"; type = "Event"; name = "Play_$($e.Name)"; onNameConflict = "fail"
            children = @(@{ type = "Action"; name = ""; "@ActionType" = 1; "@Target" = "$actor\$($e.Name)" }) } | Out-Null
    }

    # one SoundBank with all the events, their structures and their media inside it (no streamed files)
    Write-Step "SoundBank $bankName"
    Waapi "ak.wwise.core.object.create" @{ parent = "\SoundBanks\Default Work Unit"; type = "SoundBank"; name = $bankName; onNameConflict = "fail" } | Out-Null
    Waapi "ak.wwise.core.soundbank.setInclusions" @{ soundbank = "\SoundBanks\Default Work Unit\$bankName"; operation = "replace"
        inclusions = @(@{ object = "\Events\Default Work Unit"; filter = @("events", "structures", "media") }) } | Out-Null
    Waapi "ak.wwise.core.project.save" | Out-Null
}

# Wwise gives every object it creates a random GUID (and a random short id for sounds, containers and attenuations, and a random media id for every
# source file), so two runs of -Rebuild would differ in every Work Unit and in the banks. After the content is in, the ids are replaced by ids that
# come from the place of the object in the project (its path of names), so that the same audio_bank.json gives the same files. The factory Work Units
# keep their objects (the shipped GUIDs); only the id of the Work Unit itself changes with the project, and the references to it follow.
function Normalize-ProjectIds {
    Write-Step "making the ids of the project depend on the names only"
    $md5 = [Security.Cryptography.MD5]::Create()
    $guidPattern = '\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}'
    $files = @(Get-Item $projectFile) + @(Get-ChildItem $projectDir -Recurse -Filter *.wwu -File | Where-Object { $_.FullName -notmatch '\\\.cache\\|\\GeneratedSoundBanks\\' })
    $guidMap = @{}
    $numberMap = @{}
    $usedNumbers = New-Object 'System.Collections.Generic.HashSet[uint32]'

    function Derive([string]$key) {
        return $md5.ComputeHash([Text.Encoding]::UTF8.GetBytes("RocketArena|$key"))
    }
    function Walk($node, [string]$chain, [string]$file, [bool]$factory) {
        $seen = @{}
        foreach ($child in $node.ChildNodes) {
            if ($child.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
            $name = $child.GetAttribute("Name")
            $segment = "$($child.LocalName):$name"
            if ($seen.ContainsKey($segment)) { $seen[$segment]++ } else { $seen[$segment] = 0 }
            $path = "$chain/$segment#$($seen[$segment])"
            $id = $child.GetAttribute("ID")
            # the bus that every project has has the same ids in every project, and the engine knows them: it stays as it is
            $builtIn = ($child.LocalName -eq "Bus" -and ($name -eq "Main Audio Bus" -or $name -eq "Master Audio Bus"))
            # an ObjectRef and an ActiveSource only name an object that is defined somewhere else: they follow the definition
            $isReference = ($child.LocalName -eq "ObjectRef" -or $child.LocalName -eq "ActiveSource")
            if (-not $isReference -and $id -match "^$guidPattern`$") {
                # a factory file keeps the GUIDs of its objects; its Work Unit is the one thing in it that is new in every project
                if (((-not $factory) -or $child.LocalName -eq "WorkUnit") -and -not $builtIn) {
                    $b = Derive "$file$path"
                    $guidMap[$id.ToUpperInvariant()] = ("{{{0}-{1}-{2}-{3}-{4}}}" -f [BitConverter]::ToString($b, 0, 4).Replace("-", ""), [BitConverter]::ToString($b, 4, 2).Replace("-", ""),
                        [BitConverter]::ToString($b, 6, 2).Replace("-", ""), [BitConverter]::ToString($b, 8, 2).Replace("-", ""), [BitConverter]::ToString($b, 10, 6).Replace("-", ""))
                }
            }
            if ($child.LocalName -eq "MediaID" -and $id -match '^\d+$') {
                $key = "media:$id"
                $numberMap[$key] = (New-Number "$file$path")
            }
            $short = $child.GetAttribute("ShortID")
            if ($short -match '^\d+$' -and -not $factory) {
                # the objects named by a path of the user keep the id that Wwise makes of the name; the others get one from their place
                if (-not $builtIn -and [uint32]$short -ne (Get-WwiseId $name)) { $numberMap["short:$short"] = (New-Number "$file$path") }
            }
            Walk $child $path $file $factory
        }
    }
    function New-Number([string]$key) {
        $salt = 0
        while ($true) {
            $b = Derive "$key|$salt"
            $n = [BitConverter]::ToUInt32($b, 0)
            if ($n -ne 0 -and $usedNumbers.Add($n)) { return [string]$n }
            $salt++
        }
    }

    foreach ($file in $files) {
        $xml = New-Object Xml.XmlDocument
        $xml.PreserveWhitespace = $true
        $xml.Load($file.FullName)
        $relative = $file.FullName.Substring($projectDir.Length + 1).Replace("\", "/")
        $factory = $file.Name -like "Factory *"
        Walk $xml.DocumentElement "" $relative $factory
    }

    $utf8 = New-Object Text.UTF8Encoding($false)
    foreach ($file in $files) {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        if ($hasBom) { $text = $text.Substring(1) }
        $text = [regex]::Replace($text, $guidPattern, { param($m) $k = $m.Value.ToUpperInvariant(); if ($guidMap.ContainsKey($k)) { $guidMap[$k] } else { $m.Value } })
        $text = [regex]::Replace($text, 'ShortID="(\d+)"', { param($m) $k = "short:" + $m.Groups[1].Value; if ($numberMap.ContainsKey($k)) { 'ShortID="' + $numberMap[$k] + '"' } else { $m.Value } })
        $text = [regex]::Replace($text, '<MediaID ID="(\d+)"', { param($m) $k = "media:" + $m.Groups[1].Value; if ($numberMap.ContainsKey($k)) { '<MediaID ID="' + $numberMap[$k] + '"' } else { $m.Value } })
        [IO.File]::WriteAllText($file.FullName, $text, (New-Object Text.UTF8Encoding($hasBom)))
    }
    # the cache of Wwise (converted media, the index of the project) knows the old ids; it is made again by the next load
    $cache = Join-Path $projectDir ".cache"
    if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
    Write-Step ("{0} GUIDs, {1} short and media ids" -f $guidMap.Count, $numberMap.Count)
}

function New-WwiseProject($bank) {
    if (Test-Path $projectDir) {
        $full = (Resolve-Path $projectDir).Path
        if ($full -ne (Join-Path $repo "wwise\RocketArena")) { Fail "refusing to delete $full" }
        Write-Step "deleting $projectDir"
        Remove-Item -Recurse -Force $projectDir
    }
    Write-Step "creating the project"
    $text = & (Console-Path) create-new-project $projectFile --platform $platform --quiet 2>&1
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $projectFile)) { Fail "create-new-project failed: $text" }
    # what Authoring writes next to the project and git must not keep (the cache, the user settings, the generated banks of Authoring)
    Add-Content -Path (Join-Path $projectDir ".gitignore") -Value @("GeneratedSoundBanks/", ".p4ignore") -Encoding ASCII
    Write-Step "starting the Authoring API server (HTTP, port $HttpPort)"
    $server = Start-WaapiServer
    try {
        Add-ProjectContent $bank
    } finally {
        Stop-WaapiServer $server
    }
    Normalize-ProjectIds
}

function New-Banks {
    if (-not (Test-Path $projectFile)) { Fail "no project ${projectFile}: run with -Rebuild" }
    $temp = Join-Path $env:TEMP ("rocketarena_banks_" + [Guid]::NewGuid().ToString("N"))
    Write-Step "generating the SoundBanks"
    try {
        $text = & (Console-Path) generate-soundbank $projectFile --platform $platform --soundbank-path $platform $temp --bank $bankName 2>&1
        if ($LASTEXITCODE -ne 0) { Fail "generate-soundbank failed (exit code $LASTEXITCODE): $text" }
        foreach ($name in "Init.bnk", "$bankName.bnk") {
            if (-not (Test-Path (Join-Path $temp $name))) { Fail "generate-soundbank did not write ${name}: $text" }
        }
        # only the banks go to the game: no metadata (it has paths of the machine), no .wem files
        if (Test-Path $outputDir) { Remove-Item -Recurse -Force $outputDir }
        New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
        foreach ($name in "Init.bnk", "$bankName.bnk") { Copy-Item (Join-Path $temp $name) (Join-Path $outputDir $name) }
        Write-Step ("banks in assets/audio/wwise: " + ((Get-ChildItem $outputDir | ForEach-Object { "$($_.Name) $($_.Length) bytes" }) -join ", "))
    } finally {
        if (Test-Path $temp) { Remove-Item -Recurse -Force $temp }
    }
}

# ---------------------------------------------------------------------------------------------------------------------------------- the check

# the id of a name in Wwise: FNV-1, 32 bits, of the lower case name (AK::SoundEngine::GetIDFromString)
function Get-WwiseId([string]$name) {
    [uint64]$h = 2166136261
    foreach ($b in [Text.Encoding]::UTF8.GetBytes($name.ToLowerInvariant())) {
        $h = (($h * 16777619) -band 4294967295) -bxor $b
    }
    return [uint32]$h
}

# the chunks of a .bnk: ids of the events (HIRC object type 4), the number of media in DIDX, the ids of all objects
function Read-Bank([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $result = [pscustomobject]@{ Events = New-Object 'System.Collections.Generic.HashSet[uint32]'; Objects = New-Object 'System.Collections.Generic.HashSet[uint32]'; Media = 0; Chunks = @(); Meta = $null }
    $meta = New-Object IO.MemoryStream   # every chunk but the audio itself: what a hash of a name can be looked for in
    $pos = 0
    while ($pos + 8 -le $bytes.Length) {
        $tag = [Text.Encoding]::ASCII.GetString($bytes, $pos, 4)
        $size = [BitConverter]::ToUInt32($bytes, $pos + 4)
        $body = $pos + 8
        $result.Chunks += $tag
        if ($tag -ne "DATA" -and $tag -ne "DIDX") { $meta.Write($bytes, $body, [int][Math]::Min($size, $bytes.Length - $body)) }
        if ($tag -eq "DIDX") { $result.Media = [int]($size / 12) }
        if ($tag -eq "HIRC") {
            $count = [BitConverter]::ToUInt32($bytes, $body)
            $at = $body + 4
            for ($i = 0; $i -lt $count; $i++) {
                $type = $bytes[$at]
                $objSize = [BitConverter]::ToUInt32($bytes, $at + 1)
                $id = [BitConverter]::ToUInt32($bytes, $at + 5)
                $result.Objects.Add($id) | Out-Null
                if ($type -eq 4) { $result.Events.Add($id) | Out-Null }
                $at += 5 + $objSize
            }
        }
        $pos = $body + $size
    }
    $result.Meta = $meta.ToArray()
    return $result
}

function Invoke-Verify($bank) {
    $problems = New-Object System.Collections.Generic.List[string]
    $events = $bank.Events

    if (-not (Test-Path $projectFile)) { $problems.Add("no project $projectFile") }
    foreach ($name in "Init.bnk", "$bankName.bnk") {
        if (-not (Test-Path (Join-Path $outputDir $name))) { $problems.Add("no bank assets/audio/wwise/$name") }
    }
    $extra = @()
    if (Test-Path $outputDir) { $extra = @(Get-ChildItem $outputDir -Recurse -File | Where-Object { $_.Extension -ne ".bnk" }) }
    foreach ($f in $extra) { $problems.Add("a file that is not a bank in assets/audio/wwise: $($f.Name)") }

    $bankFile = Join-Path $outputDir "$bankName.bnk"
    $initFile = Join-Path $outputDir "Init.bnk"
    $playCount = 0
    if (Test-Path $bankFile) {
        $parsed = Read-Bank $bankFile
        foreach ($e in $events) {
            $id = Get-WwiseId "Play_$($e.Name)"
            if ($parsed.Events.Contains($id)) { $playCount++ } else { $problems.Add("the bank has no event Play_$($e.Name) (id $id)") }
        }
        $files = @($events | ForEach-Object { $_.Files } | Select-Object -Unique)
        if ($parsed.Media -ne $files.Count) { $problems.Add("the bank has $($parsed.Media) media inside, audio_bank.json uses $($files.Count) files") }
        if (-not ($parsed.Chunks -contains "DATA")) { $problems.Add("the bank carries no media (no DATA chunk)") }
        Write-Step ("{0}.bnk: {1} events of {2}, {3} media inside, chunks {4}" -f $bankName, $playCount, $events.Count, $parsed.Media, ($parsed.Chunks -join " "))
    }

    # the Game Parameters: in the project (the work unit) and in the banks (the id of a parameter is in the bank that uses it: Init.bnk and the bank)
    $gpCount = 0
    $gpFile = Join-Path $projectDir "Game Parameters\Default Work Unit.wwu"
    $gpNames = @()
    if (Test-Path $gpFile) {
        [xml]$xml = Get-Content -Raw -Encoding UTF8 -LiteralPath $gpFile
        $gpNames = @($xml.SelectNodes("//GameParameter") | ForEach-Object { $_.GetAttribute("Name") })
    }
    foreach ($name in $gameParameterNames) {
        if ($gpNames -contains $name) {
            $gpCount++
            $id = Get-WwiseId $name
            $idBytes = [BitConverter]::GetBytes([uint32]$id)
            $found = $false
            foreach ($f in $bankFile, $initFile) {
                if (Test-Path $f) {
                    $all = (Read-Bank $f).Meta
                    for ($i = 0; $i + 4 -le $all.Length -and -not $found; $i++) {
                        if ($all[$i] -eq $idBytes[0] -and $all[$i + 1] -eq $idBytes[1] -and $all[$i + 2] -eq $idBytes[2] -and $all[$i + 3] -eq $idBytes[3]) { $found = $true }
                    }
                }
            }
            if (-not $found) { $problems.Add("the id of the Game Parameter $name ($id) is in none of the banks") }
        } else {
            $problems.Add("no Game Parameter $name in the project")
        }
    }
    Write-Step "$gpCount Game Parameters of $($gameParameterNames.Count)"

    if ($problems.Count -gt 0) {
        foreach ($p in $problems) { Write-Host "[wwise] MISSING: $p" -ForegroundColor Red }
        Write-Host "[wwise] verify FAILED: $($problems.Count) problem(s)" -ForegroundColor Red
        exit 1
    }
    Write-Step "verify OK: $playCount events, $gpCount Game Parameters, banks in place"
}

# ---------------------------------------------------------------------------------------------------------------------------------- main

$bank = Read-AudioBank
Write-Step "audio_bank.json: $($bank.Events.Count) events"
if ($Verify) {
    Invoke-Verify $bank
    exit 0
}
if ($Rebuild) {
    New-WwiseProject $bank
}
New-Banks
Invoke-Verify $bank
exit 0
