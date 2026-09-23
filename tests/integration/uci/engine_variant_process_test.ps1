param(
    [Parameter(Mandatory = $true)]
    [string]$EnginePath,
    [Parameter(Mandatory = $true)]
    [string]$Avx2EnginePath,
    [Parameter(Mandatory = $true)]
    [string]$Avx512EnginePath
)

$ErrorActionPreference = 'Stop'
$TimeoutMilliseconds = if ($env:KOI_UCI_TIMEOUT_MS) {
    [int]$env:KOI_UCI_TIMEOUT_MS
} else {
    15000
}

Import-Module ([System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '../../support/UciSession.psm1'))) -Force

# Every engine process the shared session module starts is tracked there so a
# failed assertion can never orphan a running koi-engine.exe. The trap runs on
# any terminating error, kills the survivors, and then lets the error propagate.
trap {
    Stop-AllUciSessions
    break
}

function Invoke-EngineTranscript {
    <#
    .SYNOPSIS
        Runs one complete UCI transcript through an engine executable with an
        optional KOI_CPU_VARIANT value and returns its exit code, stdout lines,
        and stderr text.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [string]$Executable,
        [string]$Variant = $null
    )

    $previousVariant = $env:KOI_CPU_VARIANT
    if ($null -ne $Variant) {
        $env:KOI_CPU_VARIANT = $Variant
    } else {
        Remove-Item Env:\KOI_CPU_VARIANT -ErrorAction SilentlyContinue
    }
    try {
        $session = Start-UciSession -Executable $Executable
        $stdoutTask = $session.Process.StandardOutput.ReadToEndAsync()
        $session.Process.StandardInput.Write(
            "uci`nisready`nposition startpos`ngo depth 2`nstop`nquit`n")
        $session.Process.StandardInput.Close()

        if (-not $session.Process.WaitForExit($TimeoutMilliseconds)) {
            Stop-TimedOutSession $session 'engine transcript did not exit within the timeout.'
        }

        return [pscustomobject]@{
            ExitCode = $session.Process.ExitCode
            Lines = @($stdoutTask.GetAwaiter().GetResult() -split "`r?`n" |
                Where-Object { $_.Length -ne 0 })
            Diagnostics = $session.StderrTask.GetAwaiter().GetResult()
        }
    } finally {
        if ($null -ne $previousVariant) {
            $env:KOI_CPU_VARIANT = $previousVariant
        } else {
            Remove-Item Env:\KOI_CPU_VARIANT -ErrorAction SilentlyContinue
        }
    }
}

function Assert-HealthyTranscript {
    param(
        [Parameter(Mandatory = $true)]
        $Result,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )

    if ($Result.ExitCode -ne 0) {
        throw "$Description exited with $($Result.ExitCode): $($Result.Diagnostics)"
    }
    if ($Result.Diagnostics.Length -ne 0) {
        throw "$Description wrote diagnostics for a valid transcript: $($Result.Diagnostics)"
    }
    if ($Result.Lines -notcontains 'uciok') {
        throw "$Description did not answer uciok."
    }
    if (-not ($Result.Lines | Where-Object { $_ -like 'bestmove *' })) {
        throw "$Description did not return a bestmove."
    }
}

function Assert-GatedOrHealthy {
    param(
        [Parameter(Mandatory = $true)]
        $Result,
        [Parameter(Mandatory = $true)]
        [string]$Description,
        [Parameter(Mandatory = $true)]
        [string]$Instruction
    )

    if ($Result.ExitCode -eq 0) {
        Assert-HealthyTranscript $Result $Description
        return
    }
    if ($Result.ExitCode -eq 3 -and $Result.Diagnostics -match $Instruction) {
        return
    }
    throw "$Description failed unexpectedly: exit $($Result.ExitCode): $($Result.Diagnostics)"
}

# The default startup path must stay a clean UCI engine: on a capable host the
# selector silently re-execs the best sibling, and on an old host it runs the
# baseline build in-process. Either way stdout/stderr stay protocol-clean.
$automatic = Invoke-EngineTranscript -Executable $EnginePath
Assert-HealthyTranscript $automatic 'koi-engine.exe (automatic)'

# The explicit baseline override must run the generic build on any x64 CPU.
$generic = Invoke-EngineTranscript -Executable $EnginePath -Variant 'generic'
Assert-HealthyTranscript $generic 'koi-engine.exe (KOI_CPU_VARIANT=generic)'

# An unknown value is treated like `auto` instead of failing the engine.
$unknown = Invoke-EngineTranscript -Executable $EnginePath -Variant 'bogus'
Assert-HealthyTranscript $unknown 'koi-engine.exe (unknown KOI_CPU_VARIANT)'

# The direct AVX2 binary is healthy on an AVX2 host and must gate itself with a
# clear message (exit 3) on anything older, never crash with an illegal opcode.
$avx2 = Invoke-EngineTranscript -Executable $Avx2EnginePath
$hostSupportsAvx2 = $avx2.ExitCode -eq 0
Assert-GatedOrHealthy $avx2 'koi-engine-avx2.exe' 'AVX2'

# A forced AVX2 selection goes through the selector, so the child's exit code
# and diagnostics must be forwarded unchanged.
$forcedAvx2 = Invoke-EngineTranscript -Executable $EnginePath -Variant 'avx2'
if ($hostSupportsAvx2) {
    Assert-HealthyTranscript $forcedAvx2 'koi-engine.exe (KOI_CPU_VARIANT=avx2)'
} else {
    Assert-GatedOrHealthy $forcedAvx2 'koi-engine.exe (KOI_CPU_VARIANT=avx2)' 'AVX2'
}

# The direct AVX-512 binary is healthy on an AVX-512 host and gates itself on
# every other CPU; this test host has AVX2 only, so both outcomes are accepted
# as long as the binary never crashes.
$avx512 = Invoke-EngineTranscript -Executable $Avx512EnginePath
Assert-GatedOrHealthy $avx512 'koi-engine-avx512.exe' 'AVX-512'

# Forcing the AVX-512 sibling through the selector must forward its behavior.
$forcedAvx512 = Invoke-EngineTranscript -Executable $EnginePath -Variant 'avx512'
Assert-GatedOrHealthy $forcedAvx512 'koi-engine.exe (KOI_CPU_VARIANT=avx512)' 'AVX-512'
