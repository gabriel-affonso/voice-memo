# VoiceMemo NUC + GPU

Arquitetura:

```text
Telegram -> NUC -> SQLite queue -> GPU /process -> Notion -> Telegram
                         |
                         +-> fallback cloud quando GPU cai
```

O NUC fica sempre ligado e mantém a fila. O servidor GPU vira um backend opcional de inferência local com `GET /health` e `POST /process`. Se a GPU estiver fora do ar, lenta ou com erro, o NUC usa cloud para transcrição e LLM, validando o mesmo schema `ProcessedNote`. Por padrão, o fallback cloud usa OpenAI para transcrever o áudio e DeepSeek para estruturar a nota.

## Estrutura

```text
voice_memo/
  models/schemas.py       schema compartilhado
  prompts.py              prompt único para local e cloud
  config.py               variáveis de ambiente
  queue_db.py             fila SQLite
  telegram_bot.py         polling, download e respostas
  notion_client.py        criação de páginas no Notion
  local_processor.py      faster-whisper + Ollama/Qwen
  cloud_processor.py      fallback cloud
  gpu_server.py           FastAPI /health e /process
orchestrator.py           launcher do NUC
gpu_server.py             launcher da GPU
requirements-nuc.txt
requirements-gpu.txt
config/.env.example
scripts/
```

Tags controladas:

```text
Tese, Alimentação, Thíasos, OPERAS, Pessoal, Trabalho, Saúde, Gatos, Finanças, Ideias, Outros
```

## Instalação no servidor GPU

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-gpu.txt
cp config/.env.example .env
```

No `.env` da GPU, configure pelo menos:

```bash
GPU_HOST=0.0.0.0
GPU_PORT=8088
WHISPER_MODEL=large-v3
WHISPER_DEVICE=cuda
WHISPER_COMPUTE_TYPE=float16
OLLAMA_BASE_URL=http://127.0.0.1:11434
OLLAMA_MODEL=qwen2.5:7b-instruct
```

Garanta que o Ollama esteja rodando e que o modelo exista:

```bash
ollama pull qwen2.5:7b-instruct
ollama serve
```

Suba a API:

```bash
./scripts/run_gpu.sh
```

Teste:

```bash
curl http://GPU_SERVER_IP:8088/health
curl -F "file=@/caminho/para/audio.ogg" http://GPU_SERVER_IP:8088/process
```

## Instalação no NUC

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements-nuc.txt
cp config/.env.example .env
```

No `.env` do NUC, configure:

```bash
TELEGRAM_BOT_TOKEN=...
TELEGRAM_ALLOWED_CHAT_IDS=
GPU_SERVER_URL=http://GPU_SERVER_IP:8088
OPENAI_API_KEY=...
DEEPSEEK_API_KEY=...
NOTION_TOKEN=...
NOTION_DATA_SOURCE_ID=...
```

Ajuste também os nomes das propriedades do Notion se o seu database usa outros nomes:

```bash
NOTION_TITLE_PROPERTY=Name
NOTION_CLEAN_NOTE_PROPERTY=Clean Note
NOTION_SUMMARY_PROPERTY=Summary
NOTION_TRANSCRIPT_PROPERTY=Transcript
NOTION_TASKS_PROPERTY=Tasks
NOTION_TAGS_PROPERTY=Tags
NOTION_SOURCE_PROPERTY=Source
NOTION_PROCESSOR_PROPERTY=Processor
```

Rode:

```bash
./scripts/run_nuc.sh
```

## Teste incremental recomendado

1. Na GPU, teste `GET /health`.
2. Na GPU, teste `POST /process` com um áudio local.
3. No NUC, teste se `GPU_SERVER_URL` aponta para a GPU correta.
4. No NUC, rode o orquestrador e envie um áudio curto para o bot no Telegram.
5. Desligue o serviço da GPU e envie outro áudio. O NUC deve usar o fallback cloud e salvar no mesmo formato.

## Systemd opcional

Copie o projeto para `/opt/voice-memo`, configure `/opt/voice-memo/.env`, depois:

```bash
sudo cp scripts/voice-memo-gpu.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now voice-memo-gpu
```

No NUC:

```bash
sudo cp scripts/voice-memo-nuc.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now voice-memo-nuc
```

Logs:

```bash
journalctl -u voice-memo-nuc -f
journalctl -u voice-memo-gpu -f
```
