param(
    [switch]$Minor,
    [switch]$Major
)

$ErrorActionPreference = "Stop"

# Read current version from the DFIRMWARE_VERSION line in platformio.ini
$line = Get-Content platformio.ini | Where-Object { $_ -like '*DFIRMWARE_VERSION*' }
if (-not $line -or $line -notmatch '(\d+\.\d+\.\d+)') {
    Write-Error "Could not find DFIRMWARE_VERSION in platformio.ini"
    exit 1
}
$current = $Matches[1]

# Parse and bump
$parts = $current -split '\.'
$maj = [int]$parts[0]
$min = [int]$parts[1]
$pat = [int]$parts[2]

if ($Major) {
    $maj++; $min = 0; $pat = 0
} elseif ($Minor) {
    $min++; $pat = 0
} else {
    $pat++
}

$new = "$maj.$min.$pat"
$tag = "v$new"

Write-Host "Current version: $current"
Write-Host "New version:     $new"
Write-Host ""
$confirm = Read-Host "About to create release $tag. Continue? [y/N]"
if ($confirm -notmatch '^[yY]') {
    Write-Host "Aborted."
    exit 1
}

# Replace version number on the DFIRMWARE_VERSION line
# Using '$1' (single-quoted) so $1 is a literal regex backreference, not a PS variable
$content = Get-Content platformio.ini -Raw
$content = $content -replace '(DFIRMWARE_VERSION=[^0-9]*)[\d.]+', ('$1' + $new)
Set-Content platformio.ini -Value $content -NoNewline

git add platformio.ini
git commit -m "Release $tag"
git push
git tag $tag
git push origin $tag

$remote = git remote get-url origin
$repo = $remote -replace '.*github\.com[:/](.+?)(\.git)?$', '$1'
Write-Host ""
Write-Host "Released $tag - pipeline running at:"
Write-Host "https://github.com/$repo/actions"
