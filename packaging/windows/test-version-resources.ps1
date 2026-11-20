param(
    [Parameter(Mandatory = $true)][string]$BinaryDirectory,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Keep this file ASCII-only: Windows PowerShell 5 uses the system ANSI code page
# for scripts without a BOM. Build the expected Unicode value independently.
$expectedName = -join @(
    [char]0x5c0f, [char]0x667a, [char]0x8fbe, [char]0x68a6,
    [char]0x7ba1, [char]0x7406, [char]0x5de5, [char]0x5177
)
foreach ($name in @('GzydDmTools.exe', 'GzydDmToolsCli.exe', 'GzydDmToolsDebugHost.exe')) {
    $path = Join-Path $BinaryDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing application binary: $path"
    }
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    foreach ($field in @('FileDescription', 'ProductName')) {
        if ($info.$field -cne $expectedName) {
            throw "Incorrect Unicode $field in $name"
        }
    }
    if ($info.FileVersion -cne $ExpectedVersion -or $info.ProductVersion -cne $ExpectedVersion) {
        throw "Version resource mismatch in $name (expected $ExpectedVersion)"
    }
    Write-Output "VERSION_RESOURCE=PASS FILE=$name"
}
Write-Output 'WINDOWS_VERSION_RESOURCES=PASS'
