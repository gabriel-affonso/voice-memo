from __future__ import annotations

import tempfile
from pathlib import Path

import requests
from fastapi import FastAPI, File, UploadFile

from voice_memo.config import get_settings
from voice_memo.local_processor import LocalProcessor
from voice_memo.models.schemas import ProcessResponse


settings = get_settings()
processor = LocalProcessor(settings=settings)
app = FastAPI(title="VoiceMemo GPU Backend", version="0.1.0")


@app.get("/health")
def health() -> dict[str, object]:
    ollama_ok = False
    try:
        response = requests.get(
            f"{settings.ollama_base_url.rstrip('/')}/api/tags",
            timeout=2,
        )
        ollama_ok = response.ok
    except requests.RequestException:
        ollama_ok = False

    return {
        "ok": True,
        "ollama_ok": ollama_ok,
        "whisper_model": settings.whisper_model,
        "ollama_model": settings.ollama_model,
    }


@app.post("/process", response_model=ProcessResponse)
async def process(file: UploadFile = File(...)) -> ProcessResponse:
    suffix = Path(file.filename or "audio.ogg").suffix or ".ogg"
    with tempfile.NamedTemporaryFile(suffix=suffix, delete=False) as tmp:
        tmp_path = Path(tmp.name)
        tmp.write(await file.read())

    try:
        return processor.process(tmp_path)
    finally:
        tmp_path.unlink(missing_ok=True)


def main() -> None:
    import uvicorn

    uvicorn.run(
        "voice_memo.gpu_server:app",
        host=settings.gpu_host,
        port=settings.gpu_port,
        reload=False,
    )


if __name__ == "__main__":
    main()

