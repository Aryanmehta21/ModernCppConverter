import pytest
import sys
import logging
from pathlib import Path
from fastapi import FastAPI
from fastapi.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from src.config import BackendSettings, ConfigurationError, validate_settings
from src.models.convert_models import ConvertRequest
from src.routes.convert import router as convert_router
from src.routes.health import router as health_router
from src.services.ai_modernization_service import (
    AiProviderError,
    AiProviderMalformedResponseError,
    AiProviderTimeoutError,
    MockAiModernizationService,
    OpenAiModernizationService,
    validate_convert_response,
)


class TimeoutService:
    def convert(self, request: ConvertRequest):
        raise AiProviderTimeoutError("simulated timeout")


class ProviderFailureService:
    def convert(self, request: ConvertRequest):
        raise AiProviderError("simulated provider failure")


class ExplodingService:
    called = False

    def convert(self, request: ConvertRequest):
        self.called = True
        raise AssertionError("health should not call AI service")


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


def test_health_works_and_does_not_call_ai_provider():
    service = ExplodingService()
    app = make_test_app(BackendSettings(), service)
    client = TestClient(app)

    response = client.get("/health")

    assert response.status_code == 200
    assert response.json()["status"] == "ok"
    assert service.called is False


def test_convert_returns_provider_model_metadata_without_api_key():
    settings = BackendSettings(
        enable_ai_provider=True,
        ai_provider="openai",
        ai_model="gpt-4.1-mini",
        openai_api_key="secret-key",
    )
    app = make_test_app(settings, MockAiModernizationService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int* value = NULL;", "mode": "online"})

    assert response.status_code == 200
    body = response.json()
    assert body["backendStatus"] == "Connected"
    assert body["aiProvider"] == "openai"
    assert body["aiModel"] == "gpt-4.1-mini"
    assert "secret-key" not in response.text


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


def test_empty_modern_code_rejected():
    with pytest.raises(AiProviderMalformedResponseError):
        validate_convert_response(
            service_response("", changes=[], explanation="bad"),
            run_compile_check=False,
        )


def test_markdown_fenced_response_rejected():
    with pytest.raises(AiProviderMalformedResponseError):
        validate_convert_response(
            service_response("```cpp\nint value = 0;\n```", changes=[], explanation="bad"),
            run_compile_check=False,
        )


def test_timeout_handled():
    app = make_test_app(BackendSettings(), TimeoutService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int value = 0;", "mode": "online"})

    assert response.status_code == 504
    assert response.json()["ok"] is False
    assert "provider timeout" in response.json()["error"]


def test_provider_exception_does_not_crash_server():
    app = make_test_app(BackendSettings(enable_ai_provider=True, ai_provider="openai", ai_model="gpt-4.1-mini"), ProviderFailureService())
    client = TestClient(app)

    response = client.post("/api/convert", json={"code": "int value = 0;", "mode": "online"})

    assert response.status_code == 502
    assert response.json()["ok"] is False
    assert "provider error" in response.json()["error"]


def test_logs_do_not_contain_full_source_or_api_key(caplog):
    source = "int super_secret_source_marker = 42;"
    key = "secret-key"
    settings = BackendSettings(
        enable_ai_provider=True,
        ai_provider="openai",
        ai_model="gpt-4.1-mini",
        openai_api_key=key,
    )
    app = make_test_app(settings, MockAiModernizationService())
    client = TestClient(app)

    with caplog.at_level(logging.INFO):
        client.get("/health")
        client.post("/api/convert", json={"code": source, "mode": "online"})

    logs = caplog.text
    assert source not in logs
    assert key not in logs
    assert "inputSize=" in logs


def test_mock_ai_conversion_includes_direct_complex_modernization():
    service = MockAiModernizationService()
    request = ConvertRequest(
        code=(
            "#include <iostream>\n"
            "#include <cstring>\n"
            "#include <vector>\n"
            "struct Doubler { int operator()(int value) const { return value * 2; } };\n"
            "class DiagnosticTool {};\n"
            "void run(const char* input, std::vector<int>& values) {\n"
            "    DiagnosticTool* primaryTool = new DiagnosticTool();\n"
            "    delete primaryTool;\n"
            "    char name[50];\n"
            "    std::strncpy(name, input, sizeof(name));\n"
            "}\n"
        ),
        mode="online",
    )

    response = service.convert(request)

    assert response.ok is True
    assert "std::make_unique" in response.modernCode
    assert "std::string name = input;" in response.modernCode
    assert "[]" in response.modernCode
    assert response.changes
    assert any(change.applied for change in response.changes)


def test_generated_mock_sample_validated_before_returning():
    service = MockAiModernizationService()
    response = service.convert(ConvertRequest(code="int* value = NULL;", mode="online"))

    assert response.ok is True
    assert response.modernCode.strip()
    assert "```" not in response.modernCode


def service_response(modern_code: str, changes: list, explanation: str):
    from src.models.convert_models import ConvertResponse

    return ConvertResponse(
        modernCode=modern_code,
        changes=changes,
        suggestions=[],
        explanation=explanation,
        warnings=[],
        confidence=0.0,
    )
