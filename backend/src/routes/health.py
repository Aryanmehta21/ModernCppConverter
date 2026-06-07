from fastapi import APIRouter, Request
import logging

router = APIRouter()
logger = logging.getLogger(__name__)


@router.get("/health")
def health(request: Request) -> dict[str, object]:
    settings = request.app.state.settings
    logger.info("/health called provider=%s aiEnabled=%s model=%s", settings.ai_provider, settings.enable_ai_provider, settings.ai_model if settings.enable_ai_provider else "")
    return {
        "status": "ok",
        "provider": settings.ai_provider,
        "aiEnabled": settings.enable_ai_provider,
        "model": settings.ai_model if settings.enable_ai_provider else "",
    }
