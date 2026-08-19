from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv


load_dotenv()


def _bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "y", "on"}


def _int(name: str, default: int) -> int:
    value = os.getenv(name)
    return int(value) if value else default


def _float(name: str, default: float) -> float:
    value = os.getenv(name)
    return float(value) if value else default


@dataclass(frozen=True)
class Settings:
    # Shared paths
    data_dir: Path = Path(os.getenv("DATA_DIR", "data"))
    audio_dir: Path = Path(os.getenv("AUDIO_DIR", "audio/inbox"))
    sqlite_path: Path = Path(os.getenv("SQLITE_PATH", "data/voice_memo.sqlite3"))

    # Telegram
    telegram_bot_token: str = os.getenv("TELEGRAM_BOT_TOKEN", "")
    telegram_poll_timeout_seconds: int = _int("TELEGRAM_POLL_TIMEOUT_SECONDS", 25)
    telegram_allowed_chat_ids: str = os.getenv("TELEGRAM_ALLOWED_CHAT_IDS", "")

    # GPU backend
    gpu_server_url: str = os.getenv("GPU_SERVER_URL", "http://127.0.0.1:8088")
    gpu_health_timeout_seconds: float = _float("GPU_HEALTH_TIMEOUT_SECONDS", 2.0)
    gpu_process_timeout_seconds: float = _float("GPU_PROCESS_TIMEOUT_SECONDS", 180.0)

    # Queue behavior
    poll_interval_seconds: float = _float("POLL_INTERVAL_SECONDS", 2.0)
    max_attempts: int = _int("MAX_ATTEMPTS", 3)
    retry_delay_seconds: int = _int("RETRY_DELAY_SECONDS", 60)

    # Local inference on GPU server
    whisper_model: str = os.getenv("WHISPER_MODEL", "large-v3")
    whisper_device: str = os.getenv("WHISPER_DEVICE", "cuda")
    whisper_compute_type: str = os.getenv("WHISPER_COMPUTE_TYPE", "float16")
    transcription_language: str = os.getenv("TRANSCRIPTION_LANGUAGE", "pt")
    ollama_base_url: str = os.getenv("OLLAMA_BASE_URL", "http://127.0.0.1:11434")
    ollama_model: str = os.getenv("OLLAMA_MODEL", "qwen2.5:7b-instruct")
    ollama_timeout_seconds: float = _float("OLLAMA_TIMEOUT_SECONDS", 120.0)

    # Cloud fallback
    openai_api_key: str = os.getenv("OPENAI_API_KEY", "")
    cloud_stt_model: str = os.getenv("CLOUD_STT_MODEL", "gpt-4o-mini-transcribe")
    cloud_llm_model: str = os.getenv("CLOUD_LLM_MODEL", "gpt-4o-mini")
    cloud_timeout_seconds: float = _float("CLOUD_TIMEOUT_SECONDS", 180.0)
    deepseek_api_key: str = os.getenv("DEEPSEEK_API_KEY", "")
    deepseek_base_url: str = os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com")
    deepseek_model: str = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-flash")
    deepseek_thinking: str = os.getenv("DEEPSEEK_THINKING", "disabled")

    # Notion
    notion_token: str = os.getenv("NOTION_TOKEN", "")
    notion_data_source_id: str = os.getenv(
        "NOTION_DATA_SOURCE_ID",
        os.getenv("NOTION_DATASOURCE_ID", ""),
    )
    notion_database_id: str = os.getenv("NOTION_DATABASE_ID", "")
    notion_api_version: str = os.getenv("NOTION_API_VERSION", "2026-03-11")
    notion_title_property: str = os.getenv("NOTION_TITLE_PROPERTY", "Name")
    notion_clean_note_property: str = os.getenv("NOTION_CLEAN_NOTE_PROPERTY", "Clean Note")
    notion_summary_property: str = os.getenv("NOTION_SUMMARY_PROPERTY", "Summary")
    notion_transcript_property: str = os.getenv("NOTION_TRANSCRIPT_PROPERTY", "Transcript")
    notion_tasks_property: str = os.getenv("NOTION_TASKS_PROPERTY", "Tasks")
    notion_tags_property: str = os.getenv("NOTION_TAGS_PROPERTY", "Tags")
    notion_source_property: str = os.getenv("NOTION_SOURCE_PROPERTY", "Source")
    notion_processor_property: str = os.getenv("NOTION_PROCESSOR_PROPERTY", "Processor")

    # Server
    gpu_host: str = os.getenv("GPU_HOST", "0.0.0.0")
    gpu_port: int = _int("GPU_PORT", 8088)

    def ensure_runtime_dirs(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.audio_dir.mkdir(parents=True, exist_ok=True)

    @property
    def allowed_chat_ids(self) -> set[int]:
        if not self.telegram_allowed_chat_ids.strip():
            return set()
        return {
            int(item.strip())
            for item in self.telegram_allowed_chat_ids.split(",")
            if item.strip()
        }

    @property
    def notion_enabled(self) -> bool:
        return bool(self.notion_token and (self.notion_data_source_id or self.notion_database_id))

    @property
    def cloud_enabled(self) -> bool:
        return bool(self.openai_api_key)


def get_settings() -> Settings:
    return Settings()
