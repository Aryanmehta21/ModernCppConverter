# GitHub Publish Checklist

Use this checklist before pushing ModernCppConverter to GitHub.

## Files That Must Not Be Committed

Never commit:

- `backend/.env`
- `.env`
- `.env.*`
- API keys
- OpenAI keys
- tokens
- local build folders
- Qt build folders
- CMake generated files
- Python virtual environment folders
- logs

Examples of unsafe paths:

```text
build/
out/
cmake-build-*/
CMakeFiles/
CMakeCache.txt
compile_commands.json
.venv/
backend/.venv/
*.log
```

## Verify .gitignore

Confirm `.gitignore` includes:

```text
build/
out/
.venv/
backend/.venv/
.env
.env.*
*.log
CMakeFiles/
CMakeCache.txt
compile_commands.json
```

The project should keep `backend/.env.example` trackable because it contains placeholders only.

## Safe Files That Can Be Committed

These are safe to commit when they contain no secrets:

- `backend/.env.example`
- `config/app_config.json`, only if it contains no secrets
- `README.md`
- `docs/`
- source code
- tests

`config/app_config.json` should contain only a backend URL and timeout, never API keys.

## Check For Secrets Before Commit

Run these before committing:

```sh
git status
git diff --cached
git grep -n "OPENAI_API_KEY"
git grep -n "sk-"
git grep -n "api_key"
git grep -n "secret"
git grep -n "token"
```

Also check ignored files:

```sh
git status --ignored --short
git check-ignore backend/.env
git check-ignore .env
```

Expected:

- real `.env` files are ignored
- `backend/.env.example` is not ignored
- no real key appears in staged diffs

## Optional Secret Scanning Tools

Recommended tools:

- `gitleaks`
- `trufflehog`
- GitHub secret scanning

Example:

```sh
gitleaks detect --source .
trufflehog filesystem .
```

## GitHub Push Commands

From the project root:

```sh
git init
git add .
git status
git commit -m "Initial commit"
git branch -M main
git remote add origin <your-github-repo-url>
git push -u origin main
```

Review `git status` carefully before `git commit`.

## If A Real Key Was Committed

If any real API key was ever committed:

1. Rotate or revoke the key immediately.
2. Remove it from the current files.
3. Remove it from Git history before publishing.
4. Force-push only if you fully understand the consequences and the repository is not shared yet.
5. Treat the key as compromised even if the repository was private.

Do not rely only on deleting the file in a later commit. Git history can still contain the secret.

## Safely Configure AI Locally

Create `backend/.env` manually on your own machine:

```env
ENABLE_AI_PROVIDER=true
AI_PROVIDER=openai
OPENAI_API_KEY=your_key_here
AI_MODEL=gpt-4.1-mini
REQUEST_TIMEOUT=30
MAX_INPUT_SIZE=200000
MAX_OUTPUT_TOKENS=4096
LOG_LEVEL=INFO
```

Rules:

- Never commit `backend/.env`.
- Never put a real key in `backend/.env.example`.
- Put only placeholder values in `backend/.env.example`.
- Keep API keys server-side only.
- The Qt desktop app must never ask users for API keys.

## After Cloning

Users can run the app in Offline Rule-Based mode without any API key.

Online AI mode requires backend setup:

1. Create `backend/.env` locally.
2. Install backend dependencies.
3. Start the FastAPI backend.
4. Use `Check Backend Connection` in the Qt app.

End users should not enter API keys in the Qt app. If they want Online AI mode without your hosted backend, they should run their own backend with their own local `backend/.env`.
