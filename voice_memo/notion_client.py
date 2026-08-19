from __future__ import annotations

from datetime import datetime
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
            }
        }
        self._add_rich_text_property(
            properties,
            self.settings.notion_clean_note_property,
            note.clean_note,
        )
        self._add_rich_text_property(
            properties,
            self.settings.notion_summary_property,
            note.summary,
        )
        self._add_rich_text_property(
            properties,
            self.settings.notion_transcript_property,
            transcript,
        )
        self._add_rich_text_property(
            properties,
            self.settings.notion_tasks_property,
            self._format_tasks(note),
        )
        self._add_multi_select_property(
            properties,
            self.settings.notion_tags_property,
            note.tags,
        )
        self._add_rich_text_property(properties, self.settings.notion_source_property, source)
        self._add_rich_text_property(
            properties,
            self.settings.notion_processor_property,
            processor,
        )
        self._add_status_property(
            properties,
            self.settings.notion_status_property,
            self.settings.notion_status_value,
        )
        self._add_select_property(
            properties,
            self.settings.notion_system_status_property,
            self.settings.notion_system_status_value,
        )
        self._add_date_property(
            properties,
            self.settings.notion_created_property,
            self.settings.notion_created_date or self._now_iso(),
        )

        payload = {
            "parent": self._parent(),
            "properties": properties,
            "children": self._children(note, transcript, source, processor),
        }
        response = requests.post(
            "https://api.notion.com/v1/pages",
            headers=self.headers,
            json=payload,
            timeout=30,
        )
        if not response.ok:
            raise RuntimeError(
                f"Notion create page falhou: HTTP {response.status_code} {response.text}"
            )
        return response.json()["id"]

    def _parent(self) -> dict[str, str]:
        if self.settings.notion_data_source_id:
            return {"data_source_id": self.settings.notion_data_source_id}
        return {"database_id": self.settings.notion_database_id}

    def retrieve_schema(self) -> dict[str, Any]:
        if self.settings.notion_data_source_id:
            url = (
                "https://api.notion.com/v1/data_sources/"
                f"{self.settings.notion_data_source_id}"
            )
        else:
            url = f"https://api.notion.com/v1/databases/{self.settings.notion_database_id}"

        response = requests.get(url, headers=self.headers, timeout=30)
        if not response.ok:
            raise RuntimeError(
                f"Notion schema fetch falhou: HTTP {response.status_code} {response.text}"
            )
        return response.json()

    def _add_rich_text_property(
        self,
        properties: dict[str, Any],
        name: str,
        value: str,
    ) -> None:
        if name:
            properties[name] = {"rich_text": self._rich_text(value)}

    def _add_multi_select_property(
        self,
        properties: dict[str, Any],
        name: str,
        values: list[str],
    ) -> None:
        if name:
            properties[name] = {"multi_select": [{"name": value} for value in values]}

    def _add_select_property(
        self,
        properties: dict[str, Any],
        name: str,
        value: str,
    ) -> None:
        if name and value:
            properties[name] = {"select": {"name": value}}

    def _add_status_property(
        self,
        properties: dict[str, Any],
        name: str,
        value: str,
    ) -> None:
        if name and value:
            properties[name] = {"status": {"name": value}}

    def _add_date_property(
        self,
        properties: dict[str, Any],
        name: str,
        value: str,
    ) -> None:
        if name and value:
            properties[name] = {"date": {"start": value}}

    def _children(
        self,
        note: ProcessedNote,
        transcript: str,
        source: str,
        processor: str,
    ) -> list[dict[str, Any]]:
        blocks: list[dict[str, Any]] = []
        self._append_section(blocks, "Nota processada", note.clean_note)
        self._append_section(blocks, "Resumo", note.summary)
        self._append_tasks(blocks, note)
        blocks.append({"object": "block", "type": "divider", "divider": {}})
        self._append_section(blocks, "Transcrição original", transcript)
        return blocks

    def _append_section(self, blocks: list[dict[str, Any]], heading: str, text: str) -> None:
        value = text.strip()
        if not value:
            return

        blocks.append(
            {
                "object": "block",
                "type": "heading_2",
                "heading_2": {"rich_text": self._rich_text(heading)},
            }
        )
        for chunk in self._chunks(value, 1900):
            blocks.append(
                {
                    "object": "block",
                    "type": "paragraph",
                    "paragraph": {"rich_text": self._rich_text(chunk)},
                }
            )

    def _append_tasks(self, blocks: list[dict[str, Any]], note: ProcessedNote) -> None:
        if not note.tasks:
            return

        blocks.append(
            {
                "object": "block",
                "type": "heading_2",
                "heading_2": {"rich_text": self._rich_text("Tarefas")},
            }
        )
        for task in note.tasks:
            due = f" | {task.due}" if task.due else ""
            blocks.append(
                {
                    "object": "block",
                    "type": "to_do",
                    "to_do": {
                        "rich_text": self._rich_text(f"{task.text}{due}"),
                        "checked": False,
                    },
                }
            )

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

    def _now_iso(self) -> str:
        return datetime.now().astimezone().isoformat(timespec="seconds")
