# Installs axil-scribe as a user logon task or Windows service
param(
    [string]$Prefix = "$HOME\.local",
    [switch]$AsService
)

$AxilBin = Join-Path $Prefix "bin\axil.exe"
if (-not (Test-Path $AxilBin)) {
    $AxilBin = (Get-Command axil.exe -ErrorAction SilentlyContinue)?.Source
}
$ScribeDll = Join-Path $Prefix "bin\libaxil-scribe.dll"

if ($AsService) {
    Write-Host "Registering axil-scribe Windows Service..."
    New-Service -Name "axil-scribe" -BinaryPathName "`"$AxilBin`" -A -d -p 8095 -m `"$ScribeDll`"" -DisplayName "Axil Scribe Archival OCR Server" -StartupType Automatic
    Start-Service -Name "axil-scribe"
} else {
    Write-Host "Registering axil-scribe Scheduled Task at logon..."
    $XmlTemplate = Get-Content (Join-Path $PSScriptRoot "axil-scribe.xml") -Raw
    $XmlContent = $XmlTemplate.Replace("@PREFIX@", $Prefix)
    $XmlPath = [System.IO.Path]::GetTempFileName() + ".xml"
    [System.IO.File]::WriteAllText($XmlPath, $XmlContent)
    schtasks.exe /Create /TN "Axil\axil-scribe" /XML $XmlPath /F
    Remove-Item $XmlPath
    schtasks.exe /Run /TN "Axil\axil-scribe"
}
Write-Host "axil-scribe service registered successfully."
