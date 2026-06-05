import os
from dataclasses import dataclass


class ConfigurationError(RuntimeError):
    pass


@dataclass(frozen=True)
class BackendSettings:
    ai_provider: str = "mock"
    openai_api_key: str = ""
    ai_model: str = ""
    request_timeout: float = 30.0
    max_input_size: int = 200000
    max_output_tokens: int = 4096
    enable_ai_provider: bool = False
    log_level: str = "INFO"


def _read_bool(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _read_float(name: str, default: str) -> float:
    try:
        return float(os.getenv(name, default))
    except ValueError as exc:
        raise ConfigurationError(f"{name} must be numeric.") from exc


def _read_int(name: str, default: str) -> int:
    try:
        return int(os.getenv(name, default))
    except ValueError as exc:
        raise ConfigurationError(f"{name} must be an integer.") from exc


def load_settings() -> BackendSettings:
    return BackendSettings(
        ai_provider=os.getenv("AI_PROVIDER", "mock").strip().lower(),
        openai_api_key=os.getenv("OPENAI_API_KEY", ""),
        ai_model=os.getenv("AI_MODEL", ""),
        request_timeout=_read_float("REQUEST_TIMEOUT", "30"),
        max_input_size=_read_int("MAX_INPUT_SIZE", "200000"),
        max_output_tokens=_read_int("MAX_OUTPUT_TOKENS", "4096"),
        enable_ai_provider=_read_bool(os.getenv("ENABLE_AI_PROVIDER", "false")),
        log_level=os.getenv("LOG_LEVEL", "INFO"),
    )


def validate_settings(settings: BackendSettings) -> None:
    if settings.request_timeout <= 0:
        raise ConfigurationError("REQUEST_TIMEOUT must be greater than zero.")
    if settings.max_input_size <= 0:
        raise ConfigurationError("MAX_INPUT_SIZE must be greater than zero.")
    if settings.max_output_tokens <= 0:
        raise ConfigurationError("MAX_OUTPUT_TOKENS must be greater than zero.")

    if not settings.enable_ai_provider:
        return

    if settings.ai_provider != "openai":
        raise ConfigurationError("AI_PROVIDER must be 'openai' when ENABLE_AI_PROVIDER=true.")
    if not settings.openai_api_key:
        raise ConfigurationError("OPENAI_API_KEY is required when ENABLE_AI_PROVIDER=true.")
    if not settings.ai_model:
        raise ConfigurationError("AI_MODEL is required when ENABLE_AI_PROVIDER=true.")
    if not (settings.ai_model.startswith("gpt-") or settings.ai_model.startswith("o")):
        raise ConfigurationError("AI_MODEL is unsupported by the OpenAI backend configuration.")
