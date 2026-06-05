from abc import ABC, abstractmethod
import json

from ..models.convert_models import ConversionChangeModel, ConvertRequest, ConvertResponse
from ..config import BackendSettings


class AiProviderError(RuntimeError):
    status_code = 502


class AiProviderTimeoutError(AiProviderError):
    status_code = 504


class AiProviderMalformedResponseError(AiProviderError):
    status_code = 502


class IAiModernizationService(ABC):
    @abstractmethod
    def convert(self, request: ConvertRequest) -> ConvertResponse:
        raise NotImplementedError


class MockAiModernizationService(IAiModernizationService):
    """Mock service for architecture testing.

    Real AI provider integration belongs here or behind another server-side
    implementation. Provider keys must stay on the backend and must never be
    requested by or sent to the desktop client.
    """

    def convert(self, request: ConvertRequest) -> ConvertResponse:
        if request.mode == "hybrid" and request.localResult is not None:
            return ConvertResponse(
                modernCode=request.localResult.modernCode,
                changes=[
                    ConversionChangeModel(
                        ruleName="Mock AI modernization review",
                        before="offline conversion result",
                        after="review complete",
                        reason="Mock backend reviewed the offline result and found no blocking issues. Future real AI integration can add semantic recommendations here.",
                        applied=False,
                    )
                ],
                explanation="Mock AI review: the offline conversion was reviewed. Consider running clang-format and compiling with warnings enabled.",
            )

        code = request.code.replace("NULL", "nullptr")
        return ConvertResponse(
            modernCode=code,
            changes=[
                ConversionChangeModel(
                    ruleName="Mock AI-assisted modernization",
                    before=request.code[:120],
                    after=code[:120],
                    reason="Mock backend returned a realistic modernization response without calling any AI provider.",
                    applied=True,
                )
            ],
            explanation="Mock AI explanation: code was reviewed by the backend service. This is a placeholder for future server-side AI integration.",
            confidence=0.65,
        )


class OpenAiModernizationService(IAiModernizationService):
    RESPONSE_SCHEMA = {
        "type": "object",
        "additionalProperties": False,
        "properties": {
            "modernCode": {"type": "string"},
            "changes": {
                "type": "array",
                "items": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "ruleName": {"type": "string"},
                        "before": {"type": "string"},
                        "after": {"type": "string"},
                        "reason": {"type": "string"},
                        "applied": {"type": "boolean"},
                    },
                    "required": ["ruleName", "before", "after", "reason", "applied"],
                },
            },
            "suggestions": {"type": "array", "items": {"type": "string"}},
            "explanation": {"type": "string"},
            "warnings": {"type": "array", "items": {"type": "string"}},
            "confidence": {"type": "number", "minimum": 0.0, "maximum": 1.0},
        },
        "required": ["modernCode", "changes", "suggestions", "explanation", "warnings", "confidence"],
    }

    SYSTEM_PROMPT = (
        "You are a C++ modernization service. Convert legacy C++ to safe modern C++17/C++20. "
        "Preserve behavior. Do not invent missing project context. Do not remove required includes. "
        "Prefer safe transformations. If a transformation is unsafe or ambiguous, leave code unchanged "
        "and provide a suggestion instead. Return only valid JSON. No markdown fences."
    )

    def __init__(self, settings: BackendSettings):
        self._settings = settings

    def convert(self, request: ConvertRequest) -> ConvertResponse:
        try:
            from openai import APITimeoutError, OpenAI, OpenAIError
        except ImportError as exc:
            raise AiProviderError("OpenAI SDK is not installed on the backend.") from exc

        client = OpenAI(api_key=self._settings.openai_api_key, timeout=self._settings.request_timeout)
        payload = {
            "targetStandard": request.targetStandard,
            "conversionMode": request.mode,
            "customInstruction": request.options.customInstruction,
            "code": request.code,
            "localResult": request.localResult.model_dump() if request.localResult is not None else None,
        }

        try:
            response = client.responses.create(
                model=self._settings.ai_model,
                input=[
                    {"role": "system", "content": self.SYSTEM_PROMPT},
                    {
                        "role": "developer",
                        "content": (
                            "Return JSON matching the provided schema. Use changes for applied edits, "
                            "suggestions for unsafe ideas, warnings for assumptions, and confidence from 0 to 1."
                        ),
                    },
                    {"role": "user", "content": json.dumps(payload)},
                ],
                text={
                    "format": {
                        "type": "json_schema",
                        "name": "modern_cpp_conversion_result",
                        "strict": True,
                        "schema": self.RESPONSE_SCHEMA,
                    }
                },
                max_output_tokens=self._settings.max_output_tokens,
            )
        except APITimeoutError as exc:
            raise AiProviderTimeoutError("OpenAI provider request timed out.") from exc
        except OpenAIError as exc:
            raise AiProviderError(f"OpenAI provider error: {exc}") from exc

        return self._parse_provider_output(response.output_text)

    def _parse_provider_output(self, output_text: str) -> ConvertResponse:
        try:
            parsed = json.loads(output_text)
            return ConvertResponse(
                modernCode=parsed["modernCode"],
                changes=[ConversionChangeModel(**change) for change in parsed["changes"]],
                suggestions=list(parsed.get("suggestions", [])),
                explanation=parsed["explanation"],
                warnings=list(parsed.get("warnings", [])),
                confidence=float(parsed.get("confidence", 0.0)),
            )
        except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
            raise AiProviderMalformedResponseError("OpenAI provider returned malformed response JSON.") from exc


def create_ai_service(settings: BackendSettings) -> IAiModernizationService:
    if settings.enable_ai_provider and settings.ai_provider == "openai":
        return OpenAiModernizationService(settings)
    return MockAiModernizationService()
