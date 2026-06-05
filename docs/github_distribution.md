# GitHub Distribution Guide

This project can be shared publicly on GitHub, but the backend API key must never be committed.

## How Other Users Can Run It

Users have two choices.

### Option 1: Use Your Hosted Backend

You can run a backend server yourself and publish only its backend URL.

Users configure the desktop app to point to your backend in `config/app_config.json`:

```json
{
  "backendUrl": "https://your-backend.example.com",
  "requestTimeoutMs": 30000
}
```

In this model:

- Your backend must be online for Online AI-Assisted and Hybrid modes.
- Your backend pays for OpenAI usage.
- Your backend must protect its API key server-side.
- You should add authentication, rate limiting, monitoring, and request size limits before allowing public use.
- If your backend is offline, the desktop app falls back to Offline Rule-Based mode.

### Option 2: User Runs Their Own Backend

If your backend is not running, another developer can run their own local backend and use their own OpenAI key.

They should create `backend/.env` locally:

```env
ENABLE_AI_PROVIDER=true
AI_PROVIDER=openai
OPENAI_API_KEY=their_key_here
AI_MODEL=gpt-4.1-mini
REQUEST_TIMEOUT=30
MAX_INPUT_SIZE=200000
MAX_OUTPUT_TOKENS=4096
LOG_LEVEL=INFO
```

Then start the backend:

```sh
cd backend
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
set -a
. ./.env
set +a
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

Windows PowerShell:

```powershell
cd backend
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
Get-Content .env | ForEach-Object {
  if ($_ -match "^\s*([^#][^=]+)=(.*)$") {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
  }
}
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

They can verify the backend:

```sh
curl http://127.0.0.1:8000/health
```

The response must not expose the API key.

## What Happens If No Backend Is Running

The Qt app still works.

- Offline Rule-Based mode continues to run locally.
- Online AI-Assisted mode falls back to Offline Rule-Based mode.
- Hybrid mode runs the local converter and falls back if the backend is unavailable.
- Users see: `AI backend unavailable. Falling back to Offline Mode.`

## GitHub Safety Checklist

Before pushing to GitHub:

- Confirm `backend/.env` is not staged.
- Confirm `.env` files are ignored.
- Do not run `git add -f backend/.env`.
- Do not paste API keys into docs, screenshots, issues, commits, or release artifacts.
- Keep only `backend/.env.example` in the repository.
- Rotate the OpenAI key immediately if it is ever committed.

Useful checks:

```sh
git status --short
git check-ignore backend/.env
git ls-files | grep -E '(^|/)\\.env($|\\.)'
```

Expected:

- `backend/.env` should be ignored.
- `backend/.env.example` may be tracked.
- No real `.env` file should appear in `git ls-files`.

## Public Hosted Backend Warning

Do not expose a public backend without controls.

At minimum, add:

- HTTPS
- Authentication
- Rate limiting
- Request size limits
- Logging and monitoring
- Provider cost monitoring
- Abuse protection

Without these controls, anyone with the backend URL could consume your OpenAI quota.
