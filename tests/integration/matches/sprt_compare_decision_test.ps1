$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$scriptPath = Join-Path $repositoryRoot 'tools/stability/sprt_compare.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    $scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "Could not parse sprt_compare.ps1: $($parseErrors -join '; ')"
}

$decisionFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Get-CombinedSprtResult'
    }, $true)
if ($null -eq $decisionFunction) {
    throw 'sprt_compare.ps1 must expose its combined SPRT decision calculation as Get-CombinedSprtResult.'
}

. ([scriptblock]::Create($decisionFunction.Extent.Text))
$threshold = [Math]::Log(19.0)

function Assert-CombinedResult([double]$WhiteLlr, [double]$BlackLlr,
                               [string]$ExpectedDecision, [string]$Scenario) {
    $result = Get-CombinedSprtResult -WhiteLlr $WhiteLlr -BlackLlr $BlackLlr `
        -AcceptThreshold $threshold -RejectThreshold (-$threshold)
    $expectedLlr = $WhiteLlr + $BlackLlr
    if ($result.decision -ne $ExpectedDecision -or
        [Math]::Abs([double]$result.llr - $expectedLlr) -gt 0.000001) {
        throw "$Scenario expected combined LLR $expectedLlr and decision '$ExpectedDecision'; got LLR $($result.llr) and '$($result.decision)'."
    }
}

# The white run accepts and the black run rejects independently, but their
# combined evidence is inconclusive. Neither per-color result may override it.
Assert-CombinedResult 3.1 -2.9 'inconclusive' 'opposed color decisions with inconclusive combined evidence'

# A white acceptance must not hide stronger combined evidence for rejection.
Assert-CombinedResult 3.2 -6.3 'reject' 'opposed color decisions with combined rejection'

# Positive combined evidence still accepts when the aggregate crosses its bound.
Assert-CombinedResult 1.6 1.5 'accept' 'combined acceptance'

# Capture the real per-color command construction without starting a match.
$startFunction = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq 'Start-ColorRun'
    }, $true)
if ($null -eq $startFunction) { throw 'Missing Start-ColorRun function.' }
. ([scriptblock]::Create($startFunction.Extent.Text))
$root = Join-Path ([System.IO.Path]::GetTempPath()) 'koi-sprt-argument-test'
$matchScript = $scriptPath
$CandidatePath = $scriptPath
$BaselinePath = $scriptPath
$ReplayPath = $scriptPath
$OpeningFile = $scriptPath
$Threads = 1
$Hash = 64
$Games = 1
$Elo0 = 0
$Elo1 = 5
$MinGames = 1
$MaxGames = 1
$TimeControl = ''
$Nodes = 100
function Start-Process {
    param([string]$FilePath, [object[]]$ArgumentList, [switch]$PassThru,
          [string]$WindowStyle, [string]$RedirectStandardOutput,
          [string]$RedirectStandardError)
    return [pscustomobject]@{ arguments = @($ArgumentList) }
}
$invocation = Start-ColorRun 'white'
$arguments = @($invocation.arguments)
if (($arguments -join ' ') -notmatch '(?:^| )-KoiRandomSeed 0(?: |$)' -or
    ($arguments -join ' ') -notmatch '(?:^| )-OpponentOwnBook false(?: |$)') {
    throw 'Baseline comparison must use equal deterministic seed and disabled-book settings.'
}

Write-Output 'PASS combined SPRT decision uses aggregate evidence only'
