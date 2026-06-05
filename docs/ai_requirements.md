# AI Integration Requirements

This document describes what must be in place before enabling a real AI provider for ModernCppConverter.

OpenAI provider support is available behind `OpenAiModernizationService`, but it is disabled unless the backend is configured with `ENABLE_AI_PROVIDER=true`. The mock service remains available.

## Developer Requirements

Prerequisites for running AI-assisted mode:

- C++ compiler: C++20-capable compiler such as AppleClang, Clang, GCC, or MSVC.
- Qt: Qt 6 with Widgets and Network modules.
- CMake: 3.21 or newer.
- Python: 3.11 or newer recommended for the backend.
- FastAPI: use the version pinned or constrained in `backend/requirements.txt`.
- Uvicorn: required to run the local backend.
- Pydantic: required for request and response models.
- OS support: macOS, Windows, and Linux are supported by the desktop architecture if Qt 6 and a C++20 compiler are available.
- Network: desktop app must be able to reach the configured backend URL over HTTP locally or HTTPS in production.
- Backend configuration: `config/app_config.json` must point to the backend service.

Required backend pip packages:

```text
fastapi
uvicorn[standard]
pydantic
```

Future real AI provider packages should be added only to the backend, never to the desktop app.
The OpenAI Python package is backend-only and is listed in `backend/requirements.txt`.

## Backend Environment Variables

Production AI mode should be configured through environment variables.

| Variable | Required | Example | Purpose |
| --- | --- | --- | --- |
| `AI_PROVIDER` | Yes | `openai` | Selects the server-side AI provider implementation. |
| `OPENAI_API_KEY` | Required if `AI_PROVIDER=openai` | `sk-...` | Server-side provider credential. Never send this to the desktop app. |
| `AI_MODEL` | Yes | `gpt-4.1-mini` | Model used for modernization requests. Validate this at startup. |
| `REQUEST_TIMEOUT` | Yes | `30` | Backend-to-provider timeout in seconds. |
| `MAX_INPUT_SIZE` | Yes | `200000` | Maximum accepted request code size in bytes or characters. |
| `MAX_OUTPUT_TOKENS` | Yes | `4096` | Upper bound for generated provider output. |
| `LOG_LEVEL` | Optional | `INFO` | Backend logging verbosity. |
| `ALLOWED_ORIGINS` | Optional | `https://converter.example.com` | CORS allowlist if browser clients are ever added. Desktop clients generally do not need browser CORS. |
| `BACKEND_AUTH_TOKEN` | Recommended | generated secret | Optional desktop-to-backend authentication token for private deployments. |
| `RATE_LIMIT_PER_MINUTE` | Recommended | `60` | Request limit per client or credential. |
| `ENABLE_AI_PROVIDER` | Yes | `false` or `true` | Safety gate. Real AI calls should stay disabled until all required settings pass validation. |

Secrets belong only on the backend deployment host or secret manager. They must not be stored in the Qt application, app config JSON, repository, or installer.

## Local Development Setup

### macOS/Linux

```sh
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

Verify:

```sh
curl http://127.0.0.1:8000/health
```

Expected:

```json
{"status":"ok"}
```

### Windows PowerShell

```powershell
cd backend
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
uvicorn src.main:app --reload --host 127.0.0.1 --port 8000
```

Verify:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health
```

Expected response:

```powershell
status
------
ok
```

### Desktop Configuration

Ensure `config/app_config.json` points to the backend:

```json
{
  "backendUrl": "http://localhost:8000",
  "requestTimeoutMs": 30000
}
```

Then launch the Qt app and click `Check Backend Connection`.

## Production Deployment Requirements

Production AI-assisted mode should not expose the Python service directly to the public internet without controls.

Required:

- HTTPS termination with a valid certificate.
- Reverse proxy such as Nginx, Caddy, Traefik, or a managed load balancer.
- Firewall rules allowing only expected inbound ports.
- Backend authentication for desktop clients if deployed beyond localhost.
- Request size limits at both reverse proxy and FastAPI layers.
- Rate limiting by IP, user, tenant, license, or auth token.
- Structured logging for request IDs, status, latency, input sizes, and provider errors.
- Monitoring for uptime, latency, failure rate, provider timeout rate, and cost.
- Secrets management through environment variables, Docker/Kubernetes secrets, or a cloud secret manager.
- No provider keys in source control, desktop config files, logs, crash reports, or installers.

Recommended reverse proxy controls:

- Limit request body size.
- Enforce HTTPS.
- Add security headers.
- Configure upstream timeouts.
- Deny unknown paths.
- Rate limit `/api/convert`.

## AI Cost Considerations

Exact costs depend on the provider and model pricing at deployment time. Treat these as planning categories rather than fixed prices.

| Input size | Example | Cost risk | Recommended controls |
| --- | --- | --- | --- |
| Small snippet | 20-100 lines | Low per request | Allow interactively, log token estimates. |
| Medium file | 300-1,000 lines | Moderate | Require size checks, summarize or chunk, cap output tokens. |
| Large file | 2,000+ lines | High | Prefer offline/token/AST preprocessing, chunk intentionally, require user confirmation or batch budget. |

Cost controls should be added in the backend:

- Reject inputs over `MAX_INPUT_SIZE`.
- Estimate tokens before provider calls.
- Enforce `MAX_OUTPUT_TOKENS`.
- Add per-user or per-client rate limits.
- Cache repeated requests where appropriate.
- Log model, estimated tokens, latency, and outcome.
- Prefer hybrid mode so the offline converter reduces the amount of work sent for review.

## Security Checklist

- Never expose AI provider API keys to the desktop app.
- Never ask users to enter provider keys in the Qt application.
- Never commit `.env`.
- Add `.env` and `.env.*` to `.gitignore` before real secrets exist.
- Use environment variables or a secrets manager.
- Validate request JSON server-side.
- Add request size limits.
- Add rate limiting.
- Add authentication for non-localhost deployments.
- Use HTTPS in production.
- Avoid logging full source code by default; log sizes, hashes, and request IDs instead.
- Scrub secrets from error messages.
- Keep provider dependencies isolated to the backend.

## Configuration Validation

When real AI mode is added, backend startup should fail if:

- `ENABLE_AI_PROVIDER=true` and `AI_PROVIDER` is missing.
- `AI_PROVIDER` is unsupported.
- Required provider API key is missing.
- `AI_MODEL` is missing.
- `AI_MODEL` is not supported by the selected provider implementation.
- `REQUEST_TIMEOUT` is missing, non-numeric, or outside an acceptable range.
- `MAX_INPUT_SIZE` is missing, non-numeric, or too large for operational limits.
- `MAX_OUTPUT_TOKENS` is missing, non-numeric, or larger than the selected model supports.
- Production mode is enabled without HTTPS/reverse-proxy guidance or authentication configuration.

Developer checklist before enabling real AI mode:

- Choose provider and model.
- Add backend-only provider SDK dependency.
- Configure `AI_PROVIDER`.
- Configure provider API key in backend secrets.
- Configure `AI_MODEL`.
- Configure timeout, input size, and output token limits.
- Add startup validation.
- Add request validation tests.
- Add provider failure and timeout tests.
- Confirm desktop fallback still works when backend/provider fails.
- Confirm logs do not expose secrets or full code unintentionally.

## Implementation Plan

### Current State: Offline Converter Only

Status:

- Rule-based local converter works offline.
- Desktop app can run without any backend.
- Backend mock architecture exists but does not call a provider.

Keep:

- Offline Rule-Based as the default mode.
- Existing converter tests.
- Automatic fallback to offline mode.

### Phase 1: Mock Backend

Status:

- FastAPI service exposes `/health` and `/api/convert`.
- `MockAiModernizationService` returns realistic mock responses.
- Desktop app can test Online and Hybrid request paths.

Recommended next work:

- Add backend unit tests with FastAPI `TestClient`.
- Add request size validation.
- Add structured error response format.
- Add development-only logging.

### Phase 2: Real AI Backend

Steps:

1. Provider-specific service class behind `IAiModernizationService`: implemented for OpenAI.
2. Backend-only provider SDK dependency: implemented via `openai` in backend requirements.
3. Environment-based configuration loader: implemented.
4. Startup validation for provider, key, model, timeout, input size, and output tokens: implemented.
5. Prompt/request construction server-side: implemented.
6. Strict JSON response validation: implemented.
7. Provider timeout handling: implemented.
8. Tests for missing key, invalid request, timeout, and malformed provider response: added.
9. Keep `MockAiModernizationService` available for offline backend testing: preserved.

### Phase 3: Production Deployment

Steps:

1. Deploy backend behind HTTPS reverse proxy.
2. Store provider keys in a secret manager.
3. Enable authentication between desktop app and backend.
4. Add rate limiting.
5. Add request body limits.
6. Add monitoring and alerts.
7. Add cost telemetry.
8. Add operational runbooks for provider outage and quota exhaustion.
9. Verify desktop fallback behavior under backend outage, provider outage, and timeout scenarios.
