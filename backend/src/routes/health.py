from fastapi import APIRouter, Request

router = APIRouter()


@router.get("/health")
def health(request: Request) -> dict[str, object]:
    settings = request.app.state.settings
    return {
        "status": "ok",
        "provider": settings.ai_provider,
        "aiEnabled": settings.enable_ai_provider,
        "model": settings.ai_model if settings.enable_ai_provider else "",
    }
