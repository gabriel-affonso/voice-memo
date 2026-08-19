from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator


CONTROLLED_TAGS = [
    "Tese",
    "Alimentação",
    "Thíasos",
    "OPERAS",
    "Pessoal",
    "Trabalho",
    "Saúde",
    "Gatos",
    "Finanças",
    "Ideias",
    "Outros",
]

TagName = Literal[
    "Tese",
    "Alimentação",
    "Thíasos",
    "OPERAS",
    "Pessoal",
    "Trabalho",
    "Saúde",
    "Gatos",
    "Finanças",
    "Ideias",
    "Outros",
]


class TaskItem(BaseModel):
    model_config = ConfigDict(extra="forbid")

    text: str = Field(..., min_length=1)
    due: str | None = Field(
        default=None,
        description="Prazo em linguagem natural ou ISO-8601 quando estiver explícito.",
    )


class ProcessedNote(BaseModel):
    model_config = ConfigDict(extra="forbid")

    title: str = Field(..., min_length=1)
    clean_note: str = Field(..., min_length=1)
    summary: str = Field(..., min_length=1)
    tasks: list[TaskItem] = Field(default_factory=list)
    tags: list[TagName] = Field(default_factory=lambda: ["Outros"])

    @field_validator("tags")
    @classmethod
    def validate_tags(cls, tags: list[str]) -> list[str]:
        if not tags:
            return ["Outros"]

        allowed = set(CONTROLLED_TAGS)
        invalid = [tag for tag in tags if tag not in allowed]
        if invalid:
            raise ValueError(f"Tags inválidas: {invalid}")

        seen: set[str] = set()
        deduped: list[str] = []
        for tag in tags:
            if tag not in seen:
                deduped.append(tag)
                seen.add(tag)
        return deduped


class ProcessRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    language: str = "pt"
    audio_filename: str | None = None


class ProcessResponse(BaseModel):
    model_config = ConfigDict(extra="forbid")

    transcript: str
    note: ProcessedNote
    processor: str

