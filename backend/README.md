# ModernCppConverter Backend

This is the FastAPI backend for Online AI-Assisted and Hybrid modes.

The desktop application never talks directly to an AI provider and never asks users for API keys. Provider keys belong only in backend deployment configuration.

By default, `ENABLE_AI_PROVIDER=false`, so the backend uses `MockAiModernizationService`.

When `ENABLE_AI_PROVIDER=true` and `AI_PROVIDER=openai`, the backend uses `OpenAiModernizationService`.

## Run

```sh
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

Then use **Check Backend Connection** in the desktop app.

## Security Notes

- AI provider keys are server-side only.
- The desktop client never stores or receives secrets.
- Add authentication before exposing this service outside localhost.
- Add rate limiting before public deployment.
- Validate request sizes and accepted content types.
- Deploy behind HTTPS in production.

Real provider support is implemented behind `OpenAiModernizationService`, but it is disabled unless `ENABLE_AI_PROVIDER=true`.

## Enable OpenAI Provider Locally

Create `backend/.env` manually. Never commit it.

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

Load the variables in your shell, then start the backend:

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

macOS/Linux:

```sh
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
set -a
. ./.env
set +a
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

Verify health:

```sh
curl http://127.0.0.1:8000/health
```

The response shows provider, enabled state, and model, but never the API key.

## Tests

```sh
cd backend
source .venv/bin/activate
python -m pytest tests -q
```
