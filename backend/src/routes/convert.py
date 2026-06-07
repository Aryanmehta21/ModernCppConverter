from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse
import logging

from ..models.convert_models import ConvertRequest, ConvertResponse
from ..services.ai_modernization_service import AiProviderError, AiProviderTimeoutError

router = APIRouter(prefix="/api")
logger = logging.getLogger(__name__)


def _with_backend_metadata(response: ConvertResponse, settings, backend_status: str) -> ConvertResponse:
    response.backendStatus = backend_status
    response.aiProvider = settings.ai_provider
    response.aiModel = settings.ai_model if settings.enable_ai_provider else ""
    return response


def _error_response(settings, status_code: int, error: str, log_message: str) -> JSONResponse:
    logger.warning(log_message)
    response = _with_backend_metadata(ConvertResponse(ok=False, error=error), settings, "Error")
    return JSONResponse(status_code=status_code, content=response.model_dump())


@router.post("/convert", response_model=ConvertResponse)
def convert(request: ConvertRequest, app_request: Request) -> ConvertResponse | JSONResponse:
    settings = app_request.app.state.settings
    logger.info(
        "/api/convert called mode=%s aggressiveness=%s provider=%s model=%s inputSize=%d",
        request.mode,
        request.aggressivenessLevel or "default",
        settings.ai_provider,
        settings.ai_model if settings.enable_ai_provider else "",
        len(request.code),
    )
    if len(request.code) > settings.max_input_size:
        return _error_response(settings, 413, "invalid request: input exceeds MAX_INPUT_SIZE", "validation failure: input exceeds MAX_INPUT_SIZE")

    try:
        response = app_request.app.state.ai_service.convert(request)
        logger.info("/api/convert succeeded provider=%s model=%s changes=%d warnings=%d", settings.ai_provider, settings.ai_model if settings.enable_ai_provider else "", len(response.changes), len(response.warnings))
        return _with_backend_metadata(response, settings, "Connected")
    except AiProviderTimeoutError as exc:
        logger.warning("provider timeout: %s", exc)
        return _error_response(settings, 504, f"provider timeout: {exc}", "provider timeout")
    except AiProviderError as exc:
        logger.warning("provider error: %s", exc)
        return _error_response(settings, 502, f"provider error: {exc}", "provider error")
    except Exception as exc:
        logger.exception("unexpected conversion failure")
        return _error_response(settings, 500, f"backend conversion error: {exc}", "unexpected conversion failure")
