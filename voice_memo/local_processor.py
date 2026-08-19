from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import requests
from faster_whisper import WhisperModel

from voice_memo.config import Settings, get_settings
from voice_memo.models.schemas import ProcessResponse, ProcessedNote
from voice_memo.prompts import build_note_prompt


class LocalProcessor:
    def __init__(self, settings: Settings | None = None) -> None:
        self.settings = settings or get_settings()
        self._whisper: WhisperModel | None = None

    @property
    def whisper(self) -> WhisperModel:
        if self._whisper is None:
            self._whisper = WhisperModel(
                self.settings.whisper_model,
                device=self.settings.whisper_device,
                compute_type=self.settings.whisper_compute_type,
            )
        return self._whisper

    def transcribe(self, audio_path: Path) -> str:
        segments, _info = self.whisper.transcribe(
            str(audio_path),
            language=self.settings.transcription_language,
            vad_filter=True,
        )
        transcript = " ".join(segment.text.strip() for segment in segments).strip()
        if not transcript:
            raise RuntimeError("Transcrição local vazia.")
        return transcript

    def structure_note(self, transcript: str) -> ProcessedNote:
        payload = {
            "model": self.settings.ollama_model,
            "messages": build_note_prompt(transcript),
            "stream": False,
            "format": "json",
            "options": {"temperature": 0.1},
        }
        response = requests.post(
            f"{self.settings.ollama_base_url.rstrip('/')}/api/chat",
            json=payload,
            timeout=self.settings.ollama_timeout_seconds,
        )
        response.raise_for_status()
        data: dict[str, Any] = response.json()
        content = data.get("message", {}).get("content") or data.get("response")
        if not content:
            raise RuntimeError("Ollama não retornou conteúdo.")

        try:
            note_payload = json.loads(content)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Ollama retornou JSON inválido: {content[:500]}") from exc

        return ProcessedNote.model_validate(note_payload)

    def process(self, audio_path: Path) -> ProcessResponse:
        transcript = self.transcribe(audio_path)
        note = self.structure_note(transcript)
        return ProcessResponse(
            transcript=transcript,
            note=note,
            processor=f"gpu:faster-whisper:{self.settings.whisper_model}+ollama:{self.settings.ollama_model}",
        )


def process_audio(audio_path: str | Path, settings: Settings | None = None) -> ProcessResponse:
    return LocalProcessor(settings=settings).process(Path(audio_path))

