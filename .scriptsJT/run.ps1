# Remove all commit history from GitHub and replace with current working directory
# Run this from the root of your local repo

Write-Host "⚠️ This will ERASE ALL HISTORY from the remote repository!" -ForegroundColor Yellow
$confirm = Read-Host "Type YES to continue"
if ($confirm -ne "YES") {
    Write-Host "Aborted."
    exit
}

# Ensure we're in a Git repository
if (-not (Test-Path ".git")) {
    Write-Host "Error: This directory is not a Git repository." -ForegroundColor Red
    exit 1
}

# Optional: stash uncommitted changes
git add -A
git commit -m "Save current working directory as new root" --allow-empty

# Create a new orphan branch (no history)
Write-Host "Creating orphan branch 'fresh-start'..."
git checkout --orphan fresh-start

# Add all files
git add -A
git commit -m "Initial commit (history wiped)"

# Delete all other local branches
Write-Host "Deleting old branches..."
git branch | ForEach-Object {
    if ($_ -notmatch "fresh-start") {
        git branch -D $_.Trim()
    }
}

# Replace main with the new orphan branch
Write-Host "Renaming branch to main..."
git branch -M main

# Force push to GitHub, wiping remote history
Write-Host "Force pushing new clean history to origin..."
git push origin main --force

# Optional: clean up tags
Write-Host "Deleting all remote tags..."
git tag -l | ForEach-Object { git push origin ":refs/tags/$_" }

Write-Host "✅ Repository reset complete. All prior history has been deleted." -ForegroundColor Green
