param(
    [string]$Injector = (Join-Path $PSScriptRoot '..\build\relwithdebinfo\bedrock_mcp_injector.exe'),
    [int]$PollSeconds = 3,
    [int]$StartupDelaySeconds = 8
)

$mutex = New-Object System.Threading.Mutex($false, 'Local\BedrockMCPAutoInject')
if (-not $mutex.WaitOne(0)) { exit 0 }

$logDir = Join-Path $env:LOCALAPPDATA 'BedrockMCP'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$log = Join-Path $logDir 'auto_inject.log'

function Write-Log([string]$message) {
    Add-Content -Path $log -Value ("{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $message) -Encoding utf8
}

$injector = (Resolve-Path $Injector).Path
Write-Log "watching with $injector"
$injected = 0

while ($true) {
    $game = Get-Process -Name 'Minecraft.Windows' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($game -and $game.Id -ne $injected) {
        Start-Sleep -Seconds $StartupDelaySeconds
        if (-not $game.HasExited) {
            $output = & $injector 2>&1 | Out-String
            Write-Log ("pid {0} exit {1}: {2}" -f $game.Id, $LASTEXITCODE, $output.Trim())
            if ($LASTEXITCODE -eq 0) { $injected = $game.Id }
        }
    }
    Start-Sleep -Seconds $PollSeconds
}
