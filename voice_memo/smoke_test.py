from __future__ import annotations

import argparse
from datetime import datetime

from voice_memo.config import get_settings
from voice_memo.models.schemas import ProcessedNote, TaskItem
from voice_memo.notion_client import NotionClient


def test_notion() -> None:
    settings = get_settings()
    note = ProcessedNote(
        title=f"VoiceMemo smoke test {datetime.now().strftime('%Y-%m-%d %H:%M')}",
        clean_note="Esta é uma nota de teste criada pelo VoiceMemo para validar a conexão com o Notion.",
        summary="Teste de conexão entre VoiceMemo e Notion.",
        tasks=[TaskItem(text="Apagar esta nota de teste se quiser", due=None)],
        tags=["Outros"],
    )
    page_id = NotionClient(settings).create_voice_note(
        note=note,
        transcript="Transcrição fictícia para teste de integração.",
        source="smoke-test",
        processor="smoke-test",
    )
    print(f"OK: página criada no Notion: {page_id}")


def show_notion_schema() -> None:
    settings = get_settings()
    schema = NotionClient(settings).retrieve_schema()
    properties = schema.get("properties", {})
    if not properties:
        print("Nenhuma propriedade encontrada na resposta do Notion.")
        return

    print("Propriedades do Notion:")
    for name, definition in sorted(properties.items(), key=lambda item: item[0].lower()):
        prop_type = definition.get("type", "unknown")
        print(f"- {name} | {prop_type}")


def main() -> None:
    parser = argparse.ArgumentParser(description="VoiceMemo smoke tests")
    parser.add_argument(
        "target",
        choices=["notion", "notion-schema"],
        help="Integração a testar",
    )
    args = parser.parse_args()

    if args.target == "notion":
        test_notion()
    elif args.target == "notion-schema":
        show_notion_schema()


if __name__ == "__main__":
    main()
