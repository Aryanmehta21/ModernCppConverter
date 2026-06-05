import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from src.config import BackendSettings, ConfigurationError, validate_settings
from src.models.convert_models import ConvertRequest
from src.routes.convert import router as convert_router
from src.routes.health import router as health_router
from src.services.ai_modernization_service import (
    AiProviderMalformedResponseError,
    AiProviderTimeoutError,
    MockAiModernizationService,
    OpenAiModernizationService,
)


class TimeoutService:
    def convert(self, request: ConvertRequest):
        raise AiProviderTimeoutError("simulated timeout")


def make_test_app(settings: BackendSettings, service) -> FastAPI:
    app = FastAPI()
    app.state.settings = settings
    app.state.ai_service = service
    app.include_router(health_router)
    app.include_router(convert_router)
    return app


def test_mock_provider_works_when_disabled():
    settings = BackendSettings(enable_ai_provider=False, ai_provider="openai")
    app = make_test_app(settings, MockAiModernizationService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int* value = NULL;", "mode": "online"})

    assert response.status_code == 200
    body = response.json()
    assert body["ok"] is True
    assert "nullptr" in body["modernCode"]


def test_startup_validation_fails_when_key_missing_and_enabled():
    settings = BackendSettings(enable_ai_provider=True, ai_provider="openai", ai_model="gpt-4.1-mini", openai_api_key="")

    with pytest.raises(ConfigurationError):
        validate_settings(settings)


def test_startup_validation_fails_for_unsupported_model():
    settings = BackendSettings(
        enable_ai_provider=True,
        ai_provider="openai",
        ai_model="unsupported-model",
        openai_api_key="test-key",
    )

    with pytest.raises(ConfigurationError):
        validate_settings(settings)


def test_request_too_large_rejected():
    settings = BackendSettings(enable_ai_provider=False, max_input_size=5)
    app = make_test_app(settings, MockAiModernizationService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int value = 0;", "mode": "online"})

    assert response.status_code == 413
    assert response.json()["ok"] is False


def test_empty_code_rejected():
    app = make_test_app(BackendSettings(), MockAiModernizationService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "   ", "mode": "online"})

    assert response.status_code == 422


def test_invalid_target_standard_rejected():
    app = make_test_app(BackendSettings(), MockAiModernizationService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int value = 0;", "targetStandard": "C++11", "mode": "online"})

    assert response.status_code == 422


def test_health_does_not_expose_api_key():
    settings = BackendSettings(
        enable_ai_provider=True,
        ai_provider="openai",
        ai_model="gpt-4.1-mini",
        openai_api_key="secret-key",
    )
    app = make_test_app(settings, MockAiModernizationService())
    client = TestClient(app)

    response = client.get("/health")

    assert response.status_code == 200
    body = response.json()
    assert body["provider"] == "openai"
    assert body["aiEnabled"] is True
    assert body["model"] == "gpt-4.1-mini"
    assert "secret-key" not in response.text
    assert "OPENAI_API_KEY" not in response.text


def test_malformed_provider_response_handled():
    settings = BackendSettings(enable_ai_provider=True, ai_provider="openai", ai_model="gpt-4.1-mini", openai_api_key="test")
    service = OpenAiModernizationService(settings)

    with pytest.raises(AiProviderMalformedResponseError):
        service._parse_provider_output("not json")


def test_timeout_handled():
    app = make_test_app(BackendSettings(), TimeoutService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int value = 0;", "mode": "online"})

    assert response.status_code == 504
    assert response.json()["ok"] is False
    assert "provider timeout" in response.json()["error"]
