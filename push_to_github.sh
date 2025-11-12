#!/bin/bash
# Script to push pg_trace to GitHub
# Usage: ./push_to_github.sh YOUR_GITHUB_USERNAME

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 YOUR_GITHUB_USERNAME"
    echo ""
    echo "This script will:"
    echo "  1. Create a new GitHub repository called 'pg_trace'"
    echo "  2. Add it as a remote"
    echo "  3. Push the code"
    echo ""
    echo "You need:"
    echo "  - GitHub CLI (gh) installed and authenticated, OR"
    echo "  - Create the repo manually on GitHub first"
    exit 1
fi

GITHUB_USER="$1"
REPO_NAME="pg_trace"

echo "🚀 Preparing to push pg_trace to GitHub..."
echo ""

# Check if gh CLI is available
if command -v gh &> /dev/null; then
    echo "✅ Found GitHub CLI (gh)"
    
    # Check if authenticated
    if gh auth status &> /dev/null; then
        echo "✅ GitHub CLI is authenticated"
        echo ""
        echo "Creating repository: ${GITHUB_USER}/${REPO_NAME}"
        
        # Create repo (public by default, add --private for private repo)
        gh repo create "${REPO_NAME}" \
            --public \
            --description "Oracle 10046-style tracing for PostgreSQL with per-block I/O analysis" \
            --source=. \
            --remote=origin \
            --push
        
        echo ""
        echo "✅ Repository created and code pushed!"
        echo "   View at: https://github.com/${GITHUB_USER}/${REPO_NAME}"
    else
        echo "❌ GitHub CLI not authenticated"
        echo "   Run: gh auth login"
        exit 1
    fi
else
    echo "⚠️  GitHub CLI (gh) not found"
    echo ""
    echo "Manual steps:"
    echo "  1. Go to https://github.com/new"
    echo "  2. Create a new repository named 'pg_trace'"
    echo "  3. DO NOT initialize with README, .gitignore, or license"
    echo "  4. Then run these commands:"
    echo ""
    echo "     git remote add origin https://github.com/${GITHUB_USER}/${REPO_NAME}.git"
    echo "     git branch -M main"
    echo "     git push -u origin main"
    echo ""
    echo "Or, if you prefer SSH:"
    echo "     git remote add origin git@github.com:${GITHUB_USER}/${REPO_NAME}.git"
    echo "     git branch -M main"
    echo "     git push -u origin main"
    echo ""
    
    read -p "Have you created the repo on GitHub? (y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo ""
        echo "Adding remote and pushing..."
        git remote add origin "https://github.com/${GITHUB_USER}/${REPO_NAME}.git" 2>/dev/null || \
            git remote set-url origin "https://github.com/${GITHUB_USER}/${REPO_NAME}.git"
        git branch -M main 2>/dev/null || true
        git push -u origin main
        
        echo ""
        echo "✅ Code pushed!"
        echo "   View at: https://github.com/${GITHUB_USER}/${REPO_NAME}"
    else
        echo "Please create the repository first, then run this script again."
        exit 1
    fi
fi

