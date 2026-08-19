from __future__ import annotations

import json
import sqlite3
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from voice_memo.models.schemas import ProcessResponse


def utc_now() -> str:
    return datetime.now(UTC).isoformat()


@dataclass(frozen=True)
class Job:
    id: int
    telegram_update_id: int
    telegram_chat_id: int
    telegram_message_id: int
    telegram_file_id: str
    telegram_file_unique_id: str
    audio_path: Path
    status: str
    attempts: int
    last_error: str | None
    notion_page_id: str | None
    transcript: str | None
    processed_json: dict[str, Any] | None
    created_at: str
    updated_at: str


class QueueDB:
    def __init__(self, sqlite_path: Path) -> None:
        self.sqlite_path = sqlite_path
        self.sqlite_path.parent.mkdir(parents=True, exist_ok=True)
        self.init()

    def connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.sqlite_path)
        conn.row_factory = sqlite3.Row
        return conn

    def init(self) -> None:
        with self.connect() as conn:
            conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS jobs (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    telegram_update_id INTEGER NOT NULL,
                    telegram_chat_id INTEGER NOT NULL,
                    telegram_message_id INTEGER NOT NULL,
                    telegram_file_id TEXT NOT NULL,
                    telegram_file_unique_id TEXT NOT NULL UNIQUE,
                    audio_path TEXT NOT NULL,
                    status TEXT NOT NULL DEFAULT 'queued',
                    attempts INTEGER NOT NULL DEFAULT 0,
                    last_error TEXT,
                    notion_page_id TEXT,
                    transcript TEXT,
                    processed_json TEXT,
                    created_at TEXT NOT NULL,
                    updated_at TEXT NOT NULL
                );

                CREATE INDEX IF NOT EXISTS idx_jobs_status_updated
                ON jobs(status, updated_at);

                CREATE TABLE IF NOT EXISTS kv_state (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
                """
            )

    def create_job_if_new(
        self,
        *,
        telegram_update_id: int,
        telegram_chat_id: int,
        telegram_message_id: int,
        telegram_file_id: str,
        telegram_file_unique_id: str,
        audio_path: Path,
    ) -> tuple[Job, bool]:
        now = utc_now()
        with self.connect() as conn:
            conn.execute(
                """
                INSERT OR IGNORE INTO jobs (
                    telegram_update_id,
                    telegram_chat_id,
                    telegram_message_id,
                    telegram_file_id,
                    telegram_file_unique_id,
                    audio_path,
                    status,
                    created_at,
                    updated_at
                )
                VALUES (?, ?, ?, ?, ?, ?, 'queued', ?, ?)
                """,
                (
                    telegram_update_id,
                    telegram_chat_id,
                    telegram_message_id,
                    telegram_file_id,
                    telegram_file_unique_id,
                    str(audio_path),
                    now,
                    now,
                ),
            )
            inserted = conn.total_changes > 0
            row = conn.execute(
                "SELECT * FROM jobs WHERE telegram_file_unique_id = ?",
                (telegram_file_unique_id,),
            ).fetchone()

        return self._job_from_row(row), inserted

    def get_ready_jobs(self, *, limit: int, retry_delay_seconds: int) -> list[Job]:
        cutoff = (datetime.now(UTC) - timedelta(seconds=retry_delay_seconds)).isoformat()
        with self.connect() as conn:
            rows = conn.execute(
                """
                SELECT * FROM jobs
                WHERE status = 'queued'
                   OR (status = 'failed' AND updated_at <= ?)
                ORDER BY created_at ASC
                LIMIT ?
                """,
                (cutoff, limit),
            ).fetchall()
        return [self._job_from_row(row) for row in rows]

    def recover_stale_processing(self, *, older_than_seconds: int) -> int:
        cutoff = (datetime.now(UTC) - timedelta(seconds=older_than_seconds)).isoformat()
        now = utc_now()
        with self.connect() as conn:
            cur = conn.execute(
                """
                UPDATE jobs
                SET status = 'queued', updated_at = ?, last_error = 'Recuperado após processamento interrompido.'
                WHERE status = 'processing' AND updated_at <= ?
                """,
                (now, cutoff),
            )
            return cur.rowcount

    def mark_processing(self, job_id: int) -> None:
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE jobs
                SET status = 'processing',
                    attempts = attempts + 1,
                    updated_at = ?
                WHERE id = ?
                """,
                (utc_now(), job_id),
            )

    def mark_done(
        self,
        job_id: int,
        *,
        result: ProcessResponse,
        notion_page_id: str | None,
    ) -> None:
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE jobs
                SET status = 'done',
                    last_error = NULL,
                    notion_page_id = ?,
                    transcript = ?,
                    processed_json = ?,
                    updated_at = ?
                WHERE id = ?
                """,
                (
                    notion_page_id,
                    result.transcript,
                    result.note.model_dump_json(),
                    utc_now(),
                    job_id,
                ),
            )

    def mark_failed(self, job_id: int, error: str) -> None:
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE jobs
                SET status = 'failed',
                    last_error = ?,
                    updated_at = ?
                WHERE id = ?
                """,
                (error[:2000], utc_now(), job_id),
            )

    def mark_dead(self, job_id: int, error: str) -> None:
        with self.connect() as conn:
            conn.execute(
                """
                UPDATE jobs
                SET status = 'dead',
                    last_error = ?,
                    updated_at = ?
                WHERE id = ?
                """,
                (error[:2000], utc_now(), job_id),
            )

    def get_state(self, key: str, default: str | None = None) -> str | None:
        with self.connect() as conn:
            row = conn.execute("SELECT value FROM kv_state WHERE key = ?", (key,)).fetchone()
        return row["value"] if row else default

    def set_state(self, key: str, value: str) -> None:
        with self.connect() as conn:
            conn.execute(
                """
                INSERT INTO kv_state(key, value)
                VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value
                """,
                (key, value),
            )

    def _job_from_row(self, row: sqlite3.Row) -> Job:
        processed_json = None
        if row["processed_json"]:
            processed_json = json.loads(row["processed_json"])

        return Job(
            id=row["id"],
            telegram_update_id=row["telegram_update_id"],
            telegram_chat_id=row["telegram_chat_id"],
            telegram_message_id=row["telegram_message_id"],
            telegram_file_id=row["telegram_file_id"],
            telegram_file_unique_id=row["telegram_file_unique_id"],
            audio_path=Path(row["audio_path"]),
            status=row["status"],
            attempts=row["attempts"],
            last_error=row["last_error"],
            notion_page_id=row["notion_page_id"],
            transcript=row["transcript"],
            processed_json=processed_json,
            created_at=row["created_at"],
            updated_at=row["updated_at"],
        )

