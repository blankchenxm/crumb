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

## Always-on + auto-update (Mac mini)

For a Mac mini that should keep the server running and stay in sync with GitHub
without manual `git pull`, install the LaunchAgents:

```bash
cd ~/research/crumb/software
./install-services.sh
```

This sets up two user LaunchAgents:

| Agent | Behavior |
|-------|----------|
| `com.crumb.serve` | Runs the server via `serve.sh`, restarts on crash, starts at login. Logs: `logs/serve.log`. |
| `com.crumb.sync` | Every 60s runs `sync-github.sh`: `git fetch`, and on new commits `git pull` + restart the server (reinstalls deps if `requirements.txt` changed). Logs: `logs/sync.log`. |

Notes:
- Workflow stays **edit on PC → push → Mac auto-pulls**. Don't commit on the Mac
  (the sync uses `git pull --ff-only` and will skip if the Mac has diverged).
- LaunchAgents don't read `~/.zshrc`, so `install-services.sh` copies your current
  `HTTPS_PROXY` into `.env` as `CRUMB_GIT_PROXY` so GitHub fetches work behind a proxy.
- Change the interval: `CRUMB_SYNC_INTERVAL=30 ./install-services.sh`.
- Remove everything: `./uninstall-services.sh`.
- `serve.sh` is the foreground launcher for launchd; `run.sh` is for manual use.
  Don't run both at once (port 8000 clash) — `install-services.sh` stops `run.sh` first.

## Notes

- `.env` holds your keys and is **gitignored** — it never leaves the Mac.
  `.env.example` is the committed template.
- Requires Python 3.10+ (the code uses `X | None` type syntax).
- `rnnoise/` is not needed to run this server (not imported yet).
