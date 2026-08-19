from __future__ import annotations

import json
from pathlib import Path

from openai import OpenAI

from voice_memo.config import Settings, get_settings
from voice_memo.models.schemas import ProcessResponse, ProcessedNote
from voice_memo.prompts import build_note_prompt


class CloudProcessor:
    def __init__(self, settings: Settings | None = None) -> None:
        self.settings = settings or get_settings()
        if not self.settings.openai_api_key:
            raise RuntimeError("OPENAI_API_KEY não configurada para fallback cloud.")
        self.client = OpenAI(
            api_key=self.settings.openai_api_key,
            timeout=self.settings.cloud_timeout_seconds,
        )
        self.deepseek_client = None
        if self.settings.deepseek_api_key:
            self.deepseek_client = OpenAI(
                api_key=self.settings.deepseek_api_key,
                base_url=self.settings.deepseek_base_url,
                timeout=self.settings.cloud_timeout_seconds,
            )

    def transcribe(self, audio_path: Path) -> str:
        with audio_path.open("rb") as audio_file:
            result = self.client.audio.transcriptions.create(
                model=self.settings.cloud_stt_model,
                file=audio_file,
                language=self.settings.transcription_language,
            )
        transcript = getattr(result, "text", "").strip()
        if not transcript:
            raise RuntimeError("Transcrição cloud vazia.")
        return transcript

    def structure_note(self, transcript: str) -> ProcessedNote:
        client = self.deepseek_client or self.client
        model = self.settings.deepseek_model if self.deepseek_client else self.settings.cloud_llm_model
        kwargs = {}
        if self.deepseek_client:
            kwargs["extra_body"] = {"thinking": {"type": self.settings.deepseek_thinking}}

        response = client.chat.completions.create(
            model=model,
            messages=build_note_prompt(transcript),
            response_format={"type": "json_object"},
            temperature=0.1,
            **kwargs,
        )
        content = response.choices[0].message.content
        if not content:
            raise RuntimeError("LLM cloud não retornou conteúdo.")

        try:
            note_payload = json.loads(content)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"LLM cloud retornou JSON inválido: {content[:500]}") from exc

        return ProcessedNote.model_validate(note_payload)

    def process(self, audio_path: Path) -> ProcessResponse:
        transcript = self.transcribe(audio_path)
        note = self.structure_note(transcript)
        return ProcessResponse(
            transcript=transcript,
            note=note,
            processor=f"cloud:{self._processor_name()}",
        )

    def _processor_name(self) -> str:
        llm = (
            f"deepseek:{self.settings.deepseek_model}"
            if self.deepseek_client
            else f"openai:{self.settings.cloud_llm_model}"
        )
        return f"openai:{self.settings.cloud_stt_model}+{llm}"


def process_audio_cloud(audio_path: str | Path, settings: Settings | None = None) -> ProcessResponse:
    return CloudProcessor(settings=settings).process(Path(audio_path))
