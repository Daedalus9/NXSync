[CmdletBinding()]
param(
    [string]$Image = "devkitpro/devkita64:20260219@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$mount = "type=bind,source=$repo,target=/project"

function Invoke-DockerStep {
    param([Parameter(Mandatory)][string[]]$Arguments)

    & docker @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Docker step failed with exit code $LASTEXITCODE"
    }
}

if ($Clean) {
    Invoke-DockerStep @(
        "run", "--rm", "--mount", $mount, "-w", "/project",
        $Image, "make", "clean-all"
    )
}

Invoke-DockerStep @(
    "run", "--rm", "--mount", $mount, "-w", "/project",
    $Image, "make", "-j2", "components"
)

Invoke-DockerStep @(
    "run", "--rm", "--mount", $mount, "-w", "/project",
    $Image, "bash", "scripts/test-host.sh"
)

Write-Host "NXSync components and host tests completed successfully."

