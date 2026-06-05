from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from ..models.convert_models import ConvertRequest, ConvertResponse
from ..services.ai_modernization_service import AiProviderError, AiProviderTimeoutError

router = APIRouter(prefix="/api")


@router.post("/convert", response_model=ConvertResponse)
def convert(request: ConvertRequest, app_request: Request) -> ConvertResponse | JSONResponse:
    settings = app_request.app.state.settings
    if len(request.code) > settings.max_input_size:
        return JSONResponse(
            status_code=413,
            content=ConvertResponse(ok=False, error="invalid request: input exceeds MAX_INPUT_SIZE").model_dump(),
        )

    try:
        return app_request.app.state.ai_service.convert(request)
    except AiProviderTimeoutError as exc:
        return JSONResponse(status_code=504, content=ConvertResponse(ok=False, error=f"provider timeout: {exc}").model_dump())
    except AiProviderError as exc:
        return JSONResponse(status_code=502, content=ConvertResponse(ok=False, error=f"provider error: {exc}").model_dump())
