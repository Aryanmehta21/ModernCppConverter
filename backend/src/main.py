from fastapi import FastAPI
import logging

from .config import load_settings, validate_settings
from .routes.convert import router as convert_router
from .routes.health import router as health_router
from .services.ai_modernization_service import create_ai_service


def create_app() -> FastAPI:
    settings = load_settings()
    logging.basicConfig(
        level=getattr(logging, settings.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    validate_settings(settings)

    app = FastAPI(title="ModernCppConverter Backend", version="0.1.0")
    app.state.settings = settings
    app.state.ai_service = create_ai_service(settings)
    app.include_router(health_router)
    app.include_router(convert_router)
    return app


app = create_app()
