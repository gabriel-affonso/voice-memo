from __future__ import annotations

import logging
import re
import time
from pathlib import Path

import requests

from voice_memo.cloud_processor import CloudProcessor
from voice_memo.config import Settings, get_settings
from voice_memo.models.schemas import ProcessResponse
from voice_memo.notion_client import NotionClient
from voice_memo.queue_db import Job, QueueDB
from voice_memo.telegram_bot import IncomingAudio, TelegramBot


LOGGER = logging.getLogger("voice_memo.orchestrator")


class GPUClient:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.base_url = settings.gpu_server_url.rstrip("/")

    def healthy(self) -> bool:
        try:
            response = requests.get(
                f"{self.base_url}/health",
                timeout=self.settings.gpu_health_timeout_seconds,
            )
            return response.ok and bool(response.json().get("ok"))
        except requests.RequestException:
            return False

    def process(self, audio_path: Path) -> ProcessResponse:
        with audio_path.open("rb") as audio_file:
            response = requests.post(
                f"{self.base_url}/process",
                files={"file": (audio_path.name, audio_file, "application/octet-stream")},
                timeout=self.settings.gpu_process_timeout_seconds,
            )
        response.raise_for_status()
        return ProcessResponse.model_validate(response.json())


class VoiceMemoOrchestrator:
    def __init__(self, settings: Settings | None = None) -> None:
        self.settings = settings or get_settings()
        self.settings.ensure_runtime_dirs()
        self.db = QueueDB(self.settings.sqlite_path)
        self.telegram = TelegramBot(
            self.settings.telegram_bot_token,
            allowed_chat_ids=self.settings.allowed_chat_ids,
        )
        self.gpu = GPUClient(self.settings)
        self.notion = NotionClient(self.settings)
        self._cloud: CloudProcessor | None = None

    @property
    def cloud(self) -> CloudProcessor:
        if self._cloud is None:
            self._cloud = CloudProcessor(self.settings)
        return self._cloud

    def run_forever(self) -> None:
        LOGGER.info("VoiceMemo orchestrator iniciado.")
        self.db.recover_stale_processing(
            older_than_seconds=int(self.settings.gpu_process_timeout_seconds * 2),
        )

        while True:
            try:
                self.poll_telegram_once()
                self.process_ready_jobs_once()
            except Exception:
                LOGGER.exception("Erro no loop principal.")
                time.sleep(self.settings.poll_interval_seconds)

    def poll_telegram_once(self) -> None:
        offset_value = self.db.get_state("telegram_offset")
        offset = int(offset_value) if offset_value else None
        updates = self.telegram.get_updates(
            offset=offset,
            timeout=self.settings.telegram_poll_timeout_seconds,
        )
        for update in updates:
            self.db.set_state("telegram_offset", str(update["update_id"] + 1))
            audio = self.telegram.extract_audio(update)
            if not audio:
                continue
            self.enqueue_audio(audio)

    def enqueue_audio(self, audio: IncomingAudio) -> None:
        audio_path = self.telegram.download_file(audio, self.settings.audio_dir)
        job, inserted = self.db.create_job_if_new(
            telegram_update_id=audio.update_id,
            telegram_chat_id=audio.chat_id,
            telegram_message_id=audio.message_id,
            telegram_file_id=audio.file_id,
            telegram_file_unique_id=audio.file_unique_id,
            audio_path=audio_path,
        )
        if inserted:
            LOGGER.info("Job %s criado para %s.", job.id, audio_path)
            self.telegram.send_message(
                chat_id=audio.chat_id,
                reply_to_message_id=audio.message_id,
                text="Recebi o áudio. Vou processar e salvar no Notion.",
            )
        else:
            LOGGER.info("Áudio duplicado ignorado: job %s.", job.id)

    def process_ready_jobs_once(self) -> None:
        jobs = self.db.get_ready_jobs(
            limit=5,
            retry_delay_seconds=self.settings.retry_delay_seconds,
        )
        for job in jobs:
            if job.attempts >= self.settings.max_attempts:
                self.db.mark_dead(job.id, job.last_error or "Máximo de tentativas atingido.")
                self.telegram.send_message(
                    chat_id=job.telegram_chat_id,
                    reply_to_message_id=job.telegram_message_id,
                    text="Não consegui processar esse áudio depois de várias tentativas.",
                )
                continue
            self.process_job(job)

    def process_job(self, job: Job) -> None:
        LOGGER.info("Processando job %s.", job.id)
        self.db.mark_processing(job.id)
        try:
            result = self.process_with_gpu_or_cloud(job.audio_path)
            page_id = self.notion.create_voice_note(
                note=result.note,
                transcript=result.transcript,
                source=f"telegram:{job.telegram_chat_id}:{job.telegram_message_id}",
                processor=result.processor,
            )
            self.db.mark_done(job.id, result=result, notion_page_id=page_id)
        except Exception as exc:
            LOGGER.exception("Job %s falhou.", job.id)
            self.db.mark_failed(job.id, str(exc))
            self.telegram.send_message(
                chat_id=job.telegram_chat_id,
                reply_to_message_id=job.telegram_message_id,
                text="Ainda não consegui processar esse áudio. Vou tentar novamente em breve.",
            )
            return

        try:
            self.telegram.send_message(
                chat_id=job.telegram_chat_id,
                reply_to_message_id=job.telegram_message_id,
                text=self.format_success_message(result, page_id),
            )
        except Exception:
            LOGGER.exception("Job %s foi salvo, mas a confirmação no Telegram falhou.", job.id)

    def process_with_gpu_or_cloud(self, audio_path: Path) -> ProcessResponse:
        if self.gpu.healthy():
            try:
                LOGGER.info("GPU saudável; usando backend local.")
                return self.gpu.process(audio_path)
            except Exception:
                LOGGER.exception("Backend GPU falhou; acionando fallback cloud.")
        else:
            LOGGER.warning("Backend GPU offline; acionando fallback cloud.")

        return self.cloud.process(audio_path)

    def format_success_message(self, result: ProcessResponse, page_id: str | None = None) -> str:
        tasks = "\n".join(
            f"☐ {task.text}{f' | {task.due}' if task.due else ''}"
            for task in result.note.tasks
        )
        if not tasks:
            tasks = "Nenhuma tarefa detectada."

        tags = " ".join(f"#{self._hashtag(tag)}" for tag in result.note.tags)
        notion_link = f"\n\n🔗 Abrir no Notion:\n{self._notion_url(page_id)}" if page_id else ""
        prefix = (
            f"✅ Nota processada\n\n"
            f"📝 {result.note.title}\n\n"
            f"{result.note.clean_note}\n\n"
            f"📌 Resumo\n"
            f"{result.note.summary}\n\n"
            f"✅ Tarefas\n"
            f"{tasks}\n\n"
            f"🏷️ {tags}\n\n"
            f"____________________\n\n"
            f"🎙️ Transcrição original\n"
        )
        transcript = result.transcript
        max_transcript_len = 4000 - len(prefix) - len(notion_link)
        if max_transcript_len < 100:
            max_transcript_len = 100
        if len(transcript) > max_transcript_len:
            transcript = transcript[: max_transcript_len - 20].rstrip() + "\n...[cortado]"
        return prefix + transcript + notion_link

    def _hashtag(self, tag: str) -> str:
        return re.sub(r"\s+", "", tag)

    def _notion_url(self, page_id: str) -> str:
        return f"https://www.notion.so/{page_id.replace('-', '')}"


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    VoiceMemoOrchestrator().run_forever()


if __name__ == "__main__":
    main()
