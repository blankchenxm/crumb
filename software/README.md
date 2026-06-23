# Crumb software (receiving server)

The FastAPI server that receives audio from the Crumb device, saves it to
`uploads/`, and transcribes it.

## Deploy (Mac mini)

```bash
cd ~/research/crumb/software

# 1. First-time setup: creates .venv, installs deps, scaffolds .env
./setup.sh

# 2. Put your real API keys in .env (created by setup.sh)
#    SILICONFLOW_API_KEY=...   (used by server.py)
#    DEEPGRAM_API_KEY=...      (used by transcribe_deepgram.py)
nano .env

# 3. Start the server in the background on 127.0.0.1:8000
./run.sh
```

`run.sh` binds to `127.0.0.1:8000` — the port the Cloudflare tunnel forwards to,
and not directly reachable from the LAN.

| Command | What it does |
|---------|--------------|
| `./setup.sh` | Create venv, install `requirements.txt`, scaffold `.env`. Run once. |
| `./run.sh` | Start the server in the background (nohup). Writes `server.pid`. |
| `./stop.sh` | Stop the background server. |
| `tail -f logs/server.log` | Watch live logs. |

## Notes

- `.env` holds your keys and is **gitignored** — it never leaves the Mac.
  `.env.example` is the committed template.
- Requires Python 3.10+ (the code uses `X | None` type syntax).
- `rnnoise/` is not needed to run this server (not imported yet).
