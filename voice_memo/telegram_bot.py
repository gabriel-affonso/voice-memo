from __future__ import annotations

import mimetypes
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import requests


@dataclass(frozen=True)
class IncomingAudio:
    update_id: int
    chat_id: int
    message_id: int
    file_id: str
    file_unique_id: str
    original_name: str


class TelegramBot:
    def __init__(self, token: str, allowed_chat_ids: set[int] | None = None) -> None:
        if not token:
            raise RuntimeError("TELEGRAM_BOT_TOKEN não configurado.")
        self.token = token
        self.allowed_chat_ids = allowed_chat_ids or set()
        self.base_url = f"https://api.telegram.org/bot{token}"
        self.file_base_url = f"https://api.telegram.org/file/bot{token}"

    def get_updates(self, *, offset: int | None, timeout: int) -> list[dict[str, Any]]:
        params: dict[str, Any] = {
            "timeout": timeout,
            "allowed_updates": json.dumps(["message"]),
        }
        if offset is not None:
            params["offset"] = offset
        response = requests.get(f"{self.base_url}/getUpdates", params=params, timeout=timeout + 10)
        response.raise_for_status()
        payload = response.json()
        if not payload.get("ok"):
            raise RuntimeError(f"Telegram getUpdates falhou: {payload}")
        return payload["result"]

    def extract_audio(self, update: dict[str, Any]) -> IncomingAudio | None:
        message = update.get("message") or {}
        chat = message.get("chat") or {}
        chat_id = chat.get("id")
        if chat_id is None:
            return None
        if self.allowed_chat_ids and int(chat_id) not in self.allowed_chat_ids:
            return None

        media = message.get("voice") or message.get("audio") or message.get("document")
        if not media:
            return None

        mime_type = media.get("mime_type", "")
        if "document" in message and not mime_type.startswith("audio/"):
            return None

        file_id = media["file_id"]
        file_unique_id = media["file_unique_id"]
        original_name = media.get("file_name") or f"{file_unique_id}{self._extension_from_mime(mime_type)}"

        return IncomingAudio(
            update_id=update["update_id"],
            chat_id=int(chat_id),
            message_id=message["message_id"],
            file_id=file_id,
            file_unique_id=file_unique_id,
            original_name=original_name,
        )

    def download_file(self, audio: IncomingAudio, destination_dir: Path) -> Path:
        destination_dir.mkdir(parents=True, exist_ok=True)
        metadata_response = requests.get(
            f"{self.base_url}/getFile",
            params={"file_id": audio.file_id},
            timeout=30,
        )
        metadata_response.raise_for_status()
        metadata = metadata_response.json()
        if not metadata.get("ok"):
            raise RuntimeError(f"Telegram getFile falhou: {metadata}")

        file_path = metadata["result"]["file_path"]
        suffix = Path(file_path).suffix or Path(audio.original_name).suffix or ".ogg"
        local_path = destination_dir / f"{audio.file_unique_id}{suffix}"
        if local_path.exists() and local_path.stat().st_size > 0:
            return local_path

        download_response = requests.get(f"{self.file_base_url}/{file_path}", timeout=120)
        download_response.raise_for_status()
        local_path.write_bytes(download_response.content)
        return local_path

    def send_message(
        self,
        *,
        chat_id: int,
        text: str,
        reply_to_message_id: int | None = None,
    ) -> None:
        payload: dict[str, Any] = {
            "chat_id": chat_id,
            "text": text[:4096],
            "disable_web_page_preview": True,
        }
        if reply_to_message_id is not None:
            payload["reply_parameters"] = {"message_id": reply_to_message_id}
        response = requests.post(f"{self.base_url}/sendMessage", json=payload, timeout=30)
        response.raise_for_status()

    def _extension_from_mime(self, mime_type: str) -> str:
        if not mime_type:
            return ".ogg"
        return mimetypes.guess_extension(mime_type) or ".ogg"
