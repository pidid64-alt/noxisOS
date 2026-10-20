<#
.SYNOPSIS
    Joins the NoxisOS ISO parts published in a release and verifies the result.

.DESCRIPTION
    Windows counterpart of scripts/assemble-iso.sh. It joins every
    noxis-*.iso.part* file in one folder in the right order, checks the size
    against the parts, verifies the parts against SHA256SUMS and the joined
    image against noxis-*.iso.sha256. A truncated (half-joined) image is the
    usual reason a stick then says "no bootable device", so this script
    refuses to finish silently when the result does not match.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\assemble-iso.ps1
    # joins the parts found in the current folder into the original file name

.EXAMPLE
    .\assemble-iso.ps1 -Parts "$env:USERPROFILE\Downloads" -Out noxis.iso
#>
[CmdletBinding()]
param(
    [string]$Parts = ".",
    [string]$Out = ""
)

$ErrorActionPreference = 'Stop'

function Fail([string]$Message) {
    Write-Host "assemble-iso: $Message" -ForegroundColor Red
    exit 1
}

$parts = @(Get-ChildItem -Path $Parts -Filter '*.iso.part*' -File | Sort-Object Name)
if ($parts.Count -eq 0) {
    Fail "no *.iso.part* files in '$Parts'. Download every part of the release into one folder."
}
if ($parts.Count -eq 1) {
    Fail "only one part found ($($parts[0].Name)) - the image is bigger than that. Download the remaining parts; joining a single part gives a truncated image ('no bootable device')."
}

# numbering must have no gaps: part01, part02, ...
$expected = 1
foreach ($p in $parts) {
    $n = 0
    if (-not [int]::TryParse(($p.Name -replace '^.*\.part', ''), [ref]$n)) {
        Fail "unexpected file name: $($p.Name)"
    }
    if ($n -ne $expected) {
        Fail "part numbering jumps: expected part$('{0:d2}' -f $expected), got $($p.Name). One part is missing."
    }
    $expected++
}

$firstName = $parts[0].Name -replace '\.part[0-9]+$', ''
if ([string]::IsNullOrWhiteSpace($Out)) { $Out = $firstName }
$outPath = Join-Path (Resolve-Path $Parts) $Out

$total = ($parts | Measure-Object -Property Length -Sum).Sum
Write-Host "assemble-iso: $($parts.Count) part(s) -> $outPath"
$parts | ForEach-Object { Write-Host ("  {0}  {1} bytes" -f $_.Name, $_.Length) }

if ((Get-Item $parts[-1].FullName).Length -eq $parts[0].Length) {
    Write-Host "assemble-iso: warning: the last part is exactly as big as the previous ones - one part may still be missing." -ForegroundColor Yellow
}

# join with a file stream: no text mode, no line-ending games, no truncation at 0x1A
$stream = [System.IO.File]::Create($outPath)
try {
    foreach ($p in $parts) {
        $input = [System.IO.File]::OpenRead($p.FullName)
        try { $input.CopyTo($stream) } finally { $input.Dispose() }
    }
} finally { $stream.Dispose() }

$joined = (Get-Item $outPath).Length
if ($joined -ne $total) {
    Fail "the joined file is $joined bytes, expected $total - not enough free disk space?"
}

# per-part checksums
$sums = Join-Path $Parts 'SHA256SUMS'
if (Test-Path $sums) {
    Write-Host "assemble-iso: checking the parts against SHA256SUMS"
    $table = @{}
    foreach ($line in Get-Content $sums) {
        if ($line -match '^(?<hash>[0-9a-fA-F]{64})\s+\*?(?<name>.+?)\s*$') {
            $table[[System.IO.Path]::GetFileName($Matches.name)] = $Matches.hash.ToLower()
        }
    }
    foreach ($p in $parts) {
        if (-not $table.ContainsKey($p.Name)) {
            Fail "$($p.Name) is not listed in SHA256SUMS - wrong or mixed-up files."
        }
        $actual = (Get-FileHash $p.FullName -Algorithm SHA256).Hash.ToLower()
        if ($actual -ne $table[$p.Name]) {
            Fail "$($p.Name) does not match SHA256SUMS - it is incomplete or damaged. Download it again."
        }
    }
    Write-Host "  parts OK"
} else {
    Write-Host "assemble-iso: note: SHA256SUMS is not here, skipping the per-part check"
}

# whole-image checksum
$shaFile = @(Get-ChildItem -Path $Parts -Filter '*.iso.sha256' -File)
if ($shaFile.Count -gt 0) {
    Write-Host "assemble-iso: checking the image against $($shaFile[0].Name)"
    $expectedHash = (Select-String -Path $shaFile[0].FullName -Pattern '[0-9a-fA-F]{64}' |
                     Select-Object -First 1).Matches[0].Value.ToLower()
    $actualHash = (Get-FileHash $outPath -Algorithm SHA256).Hash.ToLower()
    if ($expectedHash -ne $actualHash) {
        Fail "checksum mismatch!`n       expected $expectedHash`n       actual   $actualHash`n       The parts are missing, mixed up or damaged. Delete $Out and start over with fresh downloads."
    }
    Write-Host "  sha256 $actualHash"
} else {
    Write-Host "assemble-iso: note: no *.iso.sha256 here, skipping the whole-image check"
}

Write-Host ""
Write-Host "assemble-iso: done - $outPath, $joined bytes" -ForegroundColor Green
Write-Host ""
Write-Host "Write it raw to the whole disk (not to a partition, and never with a plain"
Write-Host "file copy). On Windows the simplest reliable way is Rufus in 'DD image mode'" -NoNewline
Write-Host " ..."
Write-Host "  https://rufus.ie  ->  select the ISO  ->  device  ->  START  ->  'Write in DD image mode'"
Write-Host "On Linux/macOS:"
Write-Host "  sudo dd if=$Out of=/dev/sdX bs=4M status=progress oflag=sync"
