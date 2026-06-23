#!/usr/bin/env bash
# First-time setup: create venv, install deps, scaffold .env.
# Run once after cloning (or after requirements change):  ./setup.sh
set -euo pipefail
cd "$(dirname "$0")"

echo "==> Creating virtual environment (.venv)"
python3 -m venv .venv
source .venv/bin/activate

echo "==> Installing dependencies"
pip install --upgrade pip
pip install -r requirements.txt

if [ ! -f .env ]; then
  cp .env.example .env
  echo "==> Created .env from template."
  echo "    EDIT software/.env and put your real API keys in it before running."
else
  echo "==> .env already exists, leaving it untouched."
fi

echo "==> Setup done. Next: edit software/.env, then run ./run.sh"
