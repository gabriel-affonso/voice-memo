from __future__ import annotations

from datetime import date

from voice_memo.models.schemas import CONTROLLED_TAGS


def build_note_prompt(transcript: str, today: date | None = None) -> list[dict[str, str]]:
    current_day = (today or date.today()).isoformat()
    tags = ", ".join(CONTROLLED_TAGS)

    system = f"""
Você transforma transcrições de voice memos em notas estruturadas.

Data atual: {current_day}

Responda apenas com JSON válido, sem Markdown, usando exatamente este formato:
{{
  "title": "título curto",
  "clean_note": "nota limpa em português",
  "summary": "resumo de uma frase",
  "tasks": [
    {{"text": "ação concreta", "due": "prazo explícito ou null"}}
  ],
  "tags": ["uma ou mais tags controladas"]
}}

Tags permitidas: {tags}

Regras:
- Use somente as tags da lista permitida.
- Escolha a categoria principal; não adicione tags por associação indireta.
- Use "Outros" se nenhuma categoria específica se aplicar.
- Não invente fatos, pessoas, lugares, datas, prazos ou escopos.
- Não generalize localizações geográficas. Se a transcrição diz Brasil, não escreva América Latina.
- Mantenha a nota na língua da transcrição quando possível.
- Só crie tarefas quando houver uma ação clara a fazer.
- Para prazos relativos como "amanhã", mantenha a expressão original em "due"; não converta se houver ambiguidade.
""".strip()

    user = f"Transcrição:\n{transcript.strip()}"
    return [
        {"role": "system", "content": system},
        {"role": "user", "content": user},
    ]

