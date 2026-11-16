"""Config tests: tokens come from .env, never from config.toml."""

from __future__ import annotations

from pathlib import Path

import pytest

from config import ConfigError, load_config, token_env_name

TOKENS = {"dad": "d" * 24, "emma": "e" * 24}

BASE_CONFIG = """
[server]
host = "127.0.0.1"
port = 8180
db = "data/morse.db"
retention = 1000

[[devices]]
id = "dad"
name = "DAD"

[[devices]]
id = "emma"
name = "EMMA"
peer = true
"""


def write_config(tmp_path: Path, text: str = BASE_CONFIG) -> Path:
    path = tmp_path / "config.toml"
    path.write_text(text, encoding="utf-8")
    return path


def write_env(tmp_path: Path) -> Path:
    path = tmp_path / ".env"
    path.write_text(
        "\n".join(f'{token_env_name(device)}="{token}"' for device, token in TOKENS.items())
        + "\n",
        encoding="utf-8",
    )
    return path


@pytest.fixture(autouse=True)
def clean_env(monkeypatch, tmp_path):
    for device in TOKENS:
        monkeypatch.delenv(token_env_name(device), raising=False)
    monkeypatch.delenv("MORSE_HOST", raising=False)
    monkeypatch.delenv("MORSE_PORT", raising=False)
    monkeypatch.delenv("MORSE_DB", raising=False)
    monkeypatch.setenv("MORSE_ENV_FILE", str(tmp_path / "no-such.env"))
    yield


def test_tokens_load_from_env_file(tmp_path, monkeypatch) -> None:
    env = write_env(tmp_path)
    monkeypatch.setenv("MORSE_ENV_FILE", str(env))

    config = load_config(write_config(tmp_path))

    assert {d.id: d.token for d in config.devices} == TOKENS
    assert config.peer_ids(exclude="dad") == {"emma"}
    assert config.db_path() == tmp_path / "data" / "morse.db"


def test_real_environment_wins_over_env_file(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("MORSE_ENV_FILE", str(write_env(tmp_path)))
    monkeypatch.setenv("MORSE_TOKEN_DAD", "f" * 24)

    config = load_config(write_config(tmp_path))

    assert config.devices[0].token == "f" * 24


def test_missing_token_names_the_env_variable(tmp_path, monkeypatch) -> None:
    env = write_env(tmp_path)
    env.write_text(f"MORSE_TOKEN_EMMA={TOKENS['emma']}\n", encoding="utf-8")
    monkeypatch.setenv("MORSE_ENV_FILE", str(env))

    with pytest.raises(ConfigError, match="MORSE_TOKEN_DAD"):
        load_config(write_config(tmp_path))


def test_token_in_toml_is_rejected(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("MORSE_ENV_FILE", str(write_env(tmp_path)))
    text = BASE_CONFIG.replace(
        'id = "dad"\nname = "DAD"',
        'id = "dad"\nname = "DAD"\ntoken = "leaked"',
    )

    with pytest.raises(ConfigError, match="must not live in config.toml"):
        load_config(write_config(tmp_path, text))


def test_placeholder_token_is_rejected(tmp_path, monkeypatch) -> None:
    env = tmp_path / ".env"
    env.write_text(
        "MORSE_TOKEN_DAD=REPLACE_WITH_openssl_rand_hex_24\n"
        f"MORSE_TOKEN_EMMA={TOKENS['emma']}\n",
        encoding="utf-8",
    )
    monkeypatch.setenv("MORSE_ENV_FILE", str(env))

    with pytest.raises(ConfigError, match="placeholder"):
        load_config(write_config(tmp_path))


def test_duplicate_tokens_are_rejected(tmp_path, monkeypatch) -> None:
    env = tmp_path / ".env"
    same = "a" * 24
    env.write_text(
        f"MORSE_TOKEN_DAD={same}\nMORSE_TOKEN_EMMA={same}\n", encoding="utf-8"
    )
    monkeypatch.setenv("MORSE_ENV_FILE", str(env))

    with pytest.raises(ConfigError, match="duplicate token"):
        load_config(write_config(tmp_path))
