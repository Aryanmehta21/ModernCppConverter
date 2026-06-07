from abc import ABC, abstractmethod
import json
import logging
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from ..models.convert_models import ConversionChangeModel, ConvertRequest, ConvertResponse
from ..config import BackendSettings

logger = logging.getLogger(__name__)


class AiProviderError(RuntimeError):
    status_code = 502


class AiProviderTimeoutError(AiProviderError):
    status_code = 504


class AiProviderMalformedResponseError(AiProviderError):
    status_code = 502


def _balanced(text: str, open_char: str, close_char: str) -> bool:
    depth = 0
    for char in text:
        if char == open_char:
            depth += 1
        elif char == close_char:
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def _looks_like_explanation(text: str) -> bool:
    stripped = text.lstrip()
    return stripped.startswith(("Explanation:", "Modernization summary", "Summary of changes", "```"))


def _compile_check(code: str) -> list[str]:
    compiler = shutil.which("clang++") or shutil.which("g++") or shutil.which("cl")
    if compiler is None:
        return ["Compile verification skipped because no clang++, g++, or cl compiler was found on the backend host."]

    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "modernized.cpp"
        source.write_text(code, encoding="utf-8")
        compiler_name = Path(compiler).name.lower()
        if compiler_name == "cl.exe" or compiler_name == "cl":
            command = [compiler, "/std:c++20", "/syntaxOnly", str(source)]
        else:
            command = [compiler, "-std=c++20", "-fsyntax-only", str(source)]
        completed = subprocess.run(command, capture_output=True, text=True, timeout=20, check=False)
        if completed.returncode != 0:
            output = (completed.stderr or completed.stdout).strip()
            return [f"Compile verification failed: {output[:2000]}"]
    return []


def validate_convert_response(response: ConvertResponse, *, run_compile_check: bool = True) -> ConvertResponse:
    if not response.modernCode.strip():
        raise AiProviderMalformedResponseError("AI response validation failed: modernCode is empty.")
    if "```" in response.modernCode:
        raise AiProviderMalformedResponseError("AI response validation failed: markdown fences are not allowed.")
    if not _balanced(response.modernCode, "{", "}"):
        raise AiProviderMalformedResponseError("AI response validation failed: braces are not balanced.")
    if not _balanced(response.modernCode, "(", ")"):
        raise AiProviderMalformedResponseError("AI response validation failed: parentheses are not balanced.")
    if _looks_like_explanation(response.modernCode):
        raise AiProviderMalformedResponseError("AI response validation failed: modernCode appears to contain explanation text.")

    if response.changes is None or response.suggestions is None or response.explanation is None or response.warnings is None:
        raise AiProviderMalformedResponseError("AI response validation failed: required response fields are missing.")

    if run_compile_check:
        response.warnings.extend(_compile_check(response.modernCode))

    return response


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
        logger.info("mock AI conversion selected mode=%s aggressiveness=%s inputSize=%d", request.mode, request.aggressivenessLevel or "default", len(request.code))
        if request.mode == "hybrid" and request.localResult is not None:
            modern_code = self._modernize_complex_patterns(request.localResult.modernCode)
            return validate_convert_response(ConvertResponse(
                modernCode=modern_code,
                changes=[
                    ConversionChangeModel(
                        ruleName="Mock AI modernization review",
                        before="offline conversion result",
                        after="AI-reviewed modernized result",
                        reason="Mock backend applied additional safe AI-style modernization where simple complex patterns were recognized.",
                        applied=True,
                    )
                ],
                suggestions=["Review public API changes manually before committing large migrations."],
                explanation="Mock AI review: the offline conversion was reviewed and additional safe modernization patterns were applied where possible.",
                confidence=0.72,
            ))

        code = self._modernize_complex_patterns(request.code.replace("NULL", "nullptr"))
        return validate_convert_response(ConvertResponse(
            modernCode=code,
            changes=[
                ConversionChangeModel(
                    ruleName="Mock AI-assisted modernization",
                    before=request.code[:120],
                    after=code[:120],
                    reason="Mock backend directly modernized recognized complex patterns without calling any AI provider.",
                    applied=True,
                )
            ],
            suggestions=["Unsafe ownership, lifetime, or public API changes should remain review-only."],
            explanation="Mock AI explanation: code was reviewed by the backend service. This is a placeholder for future server-side AI integration.",
            confidence=0.65,
        ))

    def _modernize_complex_patterns(self, code: str) -> str:
        code = self._convert_functor_to_lambda(code)
        code = self._convert_make_unique(code)
        code = self._convert_strncpy_buffer(code)
        code = self._convert_iterator_print_loop(code)
        return code

    def _ensure_include(self, code: str, include_line: str) -> str:
        if include_line in code:
            return code
        lines = code.splitlines()
        insert_at = 0
        for index, line in enumerate(lines):
            if line.startswith("#include"):
                insert_at = index + 1
        lines.insert(insert_at, include_line)
        return "\n".join(lines) + ("\n" if code.endswith("\n") else "")

    def _convert_make_unique(self, code: str) -> str:
        pattern = re.compile(
            r"(?m)^(?P<indent>\s*)(?P<type>[A-Za-z_]\w*)\*\s+(?P<name>[A-Za-z_]\w*)\s*=\s*new\s+(?P=type)(?:\((?P<args>[^;]*)\))?\s*;\s*\n\s*delete\s+(?P=name)\s*;"
        )

        def replace(match: re.Match[str]) -> str:
            args = match.group("args") or ""
            return f"{match.group('indent')}auto {match.group('name')} = std::make_unique<{match.group('type')}>({args});"

        updated = pattern.sub(replace, code)
        if updated != code:
            updated = self._ensure_include(updated, "#include <memory>")
        return updated

    def _convert_strncpy_buffer(self, code: str) -> str:
        pattern = re.compile(
            r"(?m)^(?P<indent>\s*)char\s+(?P<name>[A-Za-z_]\w*)\s*\[\s*\d+\s*\]\s*;\s*\n\s*std::strncpy\s*\(\s*(?P=name)\s*,\s*(?P<src>[A-Za-z_]\w*)\s*,\s*sizeof\s*\(\s*(?P=name)\s*\)\s*\)\s*;"
        )
        updated = pattern.sub(lambda match: f"{match.group('indent')}std::string {match.group('name')} = {match.group('src')};", code)
        if updated != code:
            updated = self._ensure_include(updated, "#include <string>")
        return updated

    def _convert_iterator_print_loop(self, code: str) -> str:
        pattern = re.compile(
            r"(?ms)^(?P<indent>\s*)for\s*\(\s*[\w:<>]+\s*::iterator\s+(?P<it>\w+)\s*=\s*(?P<values>\w+)\.begin\(\)\s*;\s*(?P=it)\s*!=\s*(?P=values)\.end\(\)\s*;\s*\+\+(?P=it)\s*\)\s*\n(?P=indent)\{\s*\n(?P<body>\s*)std::cout\s*<<\s*\*(?P=it)\s*<<\s*std::endl\s*;\s*\n(?P=indent)\}"
        )

        def replace(match: re.Match[str]) -> str:
            values = match.group("values")
            element = values[:-1] if values.endswith("s") and len(values) > 1 else "value"
            return (
                f"{match.group('indent')}for (const auto& {element} : {values})\n"
                f"{match.group('indent')}{{\n"
                f"{match.group('body')}std::cout << {element} << std::endl;\n"
                f"{match.group('indent')}}}"
            )

        return pattern.sub(replace, code)

    def _convert_functor_to_lambda(self, code: str) -> str:
        pattern = re.compile(
            r"struct\s+(?P<name>\w+)\s*\{\s*int\s+operator\(\)\s*\(\s*int\s+(?P<arg>\w+)\s*\)\s*const\s*\{\s*return\s+(?P<body>[^;]+);\s*\}\s*\};\s*"
        )
        match = pattern.search(code)
        if not match:
            return code
        lambda_line = f"auto {match.group('name')[0].lower() + match.group('name')[1:]} = [](int {match.group('arg')}) {{ return {match.group('body')}; }};\n"
        return pattern.sub(lambda_line, code)


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
        "Preserve behavior. Do not invent missing project context, missing classes, or missing APIs. "
        "Do not remove required includes or important logic. Do not output pseudo-code. "
        "Return complete compilable C++ wherever possible. Add necessary includes when using new standard library features. "
        "When conversionMode is online or hybrid, directly modernize complex patterns when reasonably safe. "
        "If a transformation is unsafe or ambiguous, leave that code unchanged and add a suggestion or warning. "
        "Avoid changing public APIs unless clearly safe. Return only strict valid JSON. No markdown fences."
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
            "aggressivenessLevel": request.aggressivenessLevel or ("aggressive_safe" if request.mode == "hybrid" else "balanced"),
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
                            "suggestions for unsafe ideas, warnings for assumptions, and confidence from 0 to 1. "
                            "Apply aggressivenessLevel as follows: conservative means only very low-risk edits; balanced means "
                            "apply safe direct improvements and warn on uncertainty; aggressive_safe means apply all reasonably "
                            "safe direct improvements while still preserving behavior. Directly apply these transformations when safe: "
                            "owning raw pointers to std::unique_ptr/std::make_unique with matching delete removal; std::shared_ptr only "
                            "when shared ownership is clear; helper functions/functors and callback-style functions to lambdas; "
                            "safe move semantics; char arrays plus std::strncpy to std::string; iterator and index loops to range-for; "
                            "typedef to using; NULL/custom NULL macros to native nullptr; auto when type is obvious; override on clear "
                            "overrides; constexpr for simple compile-time constants/functions; enum class only when all usages can be "
                            "updated safely; std::optional for clear null/sentinel state; std::string_view only with safe lifetimes; "
                            "structured bindings for clear pair/tuple/map usage; if constexpr only for clear compile-time branching. "
                            "If unsafe, do not change code; put it in suggestions/warnings. No markdown fences."
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
        except Exception as exc:
            raise AiProviderError(f"OpenAI provider unexpected error: {exc}") from exc

        return validate_convert_response(self._parse_provider_output(response.output_text))

    def _parse_provider_output(self, output_text: str) -> ConvertResponse:
        try:
            parsed = json.loads(output_text)
            required = {"modernCode", "changes", "suggestions", "explanation", "warnings", "confidence"}
            if not required.issubset(parsed):
                raise KeyError("missing required response field")
            return ConvertResponse(
                modernCode=parsed["modernCode"],
                changes=[ConversionChangeModel(**change) for change in parsed["changes"]],
                suggestions=list(parsed["suggestions"]),
                explanation=parsed["explanation"],
                warnings=list(parsed["warnings"]),
                confidence=float(parsed["confidence"]),
            )
        except (json.JSONDecodeError, KeyError, TypeError, ValueError) as exc:
            raise AiProviderMalformedResponseError("OpenAI provider returned malformed response JSON.") from exc


def create_ai_service(settings: BackendSettings) -> IAiModernizationService:
    if settings.enable_ai_provider and settings.ai_provider == "openai":
        return OpenAiModernizationService(settings)
    return MockAiModernizationService()
