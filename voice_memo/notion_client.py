from __future__ import annotations

from typing import Any

import requests

from voice_memo.config import Settings, get_settings
from voice_memo.models.schemas import ProcessedNote


class NotionClient:
    def __init__(self, settings: Settings | None = None) -> None:
        self.settings = settings or get_settings()
        if not self.settings.notion_token:
            raise RuntimeError("NOTION_TOKEN não configurado.")
        if not self.settings.notion_data_source_id and not self.settings.notion_database_id:
            raise RuntimeError("NOTION_DATA_SOURCE_ID não configurado.")

        self.headers = {
            "Authorization": f"Bearer {self.settings.notion_token}",
            "Notion-Version": self.settings.notion_api_version,
            "Content-Type": "application/json",
        }

    def create_voice_note(
        self,
        *,
        note: ProcessedNote,
        transcript: str,
        source: str,
        processor: str,
    ) -> str:
        properties: dict[str, Any] = {
            self.settings.notion_title_property: {
                "title": [{"text": {"content": note.title[:2000]}}],
            },
            self.settings.notion_clean_note_property: {
                "rich_text": self._rich_text(note.clean_note),
            },
            self.settings.notion_summary_property: {
                "rich_text": self._rich_text(note.summary),
            },
            self.settings.notion_transcript_property: {
                "rich_text": self._rich_text(transcript),
            },
            self.settings.notion_tasks_property: {
                "rich_text": self._rich_text(self._format_tasks(note)),
            },
            self.settings.notion_tags_property: {
                "multi_select": [{"name": tag} for tag in note.tags],
            },
            self.settings.notion_source_property: {
                "rich_text": self._rich_text(source),
            },
            self.settings.notion_processor_property: {
                "rich_text": self._rich_text(processor),
            },
        }

        payload = {
            "parent": self._parent(),
            "properties": properties,
        }
        response = requests.post(
            "https://api.notion.com/v1/pages",
            headers=self.headers,
            json=payload,
            timeout=30,
        )
        response.raise_for_status()
        return response.json()["id"]

    def _parent(self) -> dict[str, str]:
        if self.settings.notion_data_source_id:
            return {"data_source_id": self.settings.notion_data_source_id}
        return {"database_id": self.settings.notion_database_id}

    def _rich_text(self, value: str) -> list[dict[str, Any]]:
        text = value.strip()
        if not text:
            return []
        return [{"text": {"content": chunk}} for chunk in self._chunks(text, 2000)]

    def _chunks(self, value: str, size: int) -> list[str]:
        return [value[index : index + size] for index in range(0, len(value), size)]

    def _format_tasks(self, note: ProcessedNote) -> str:
        if not note.tasks:
            return ""
        lines = []
        for task in note.tasks:
            due = f" | {task.due}" if task.due else ""
            lines.append(f"- {task.text}{due}")
        return "\n".join(lines)
