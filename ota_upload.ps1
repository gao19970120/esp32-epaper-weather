Param(
    [string]$EspIp = "",
    [string]$OtaPassword = ""
)

Set-Location $PSScriptRoot

$bin = Get-ChildItem -Recurse -Filter "te.ino.bin" |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1

if (-not $bin) {
    Write-Error "te.ino.bin not found, please export compiled binary in Arduino IDE."
    exit 1
}

$arduinoCoreDir = Join-Path $env:LOCALAPPDATA "Arduino15\packages\esp32\hardware\esp32"
$espota = Get-ChildItem -Path $arduinoCoreDir -Recurse -Filter "espota.py" -ErrorAction SilentlyContinue |
          Sort-Object FullName -Descending |
          Select-Object -First 1

if (-not $espota) {
    Write-Error "espota.py not found under $arduinoCoreDir, please locate it manually."
    exit 1
}

if (-not $OtaPassword) {
    $confPath = Join-Path $PSScriptRoot "data\conf.json"
    if (Test-Path $confPath) {
        $json = Get-Content $confPath -Raw | ConvertFrom-Json
        $OtaPassword = $json.ota_password
    }
}

if (-not $EspIp) {
    $EspIp = Read-Host "ESP32 IP address (eg. 192.168.2.105)"
}
if (-not $OtaPassword) {
    $OtaPassword = Read-Host "OTA password (ota_password in conf.json)"
}

Write-Host "Firmware: $($bin.FullName)"
Write-Host "Target: $EspIp"
Write-Host "espota.py: $($espota.FullName)"
Write-Host "Timeout: 500 seconds"

python $espota.FullName -i $EspIp -f $bin.FullName --auth=$OtaPassword -t 500
