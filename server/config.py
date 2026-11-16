"""Configuration loading for the Morse relay.

Two files, deliberately separated:

* ``server/config.toml`` — committed. Non-secret settings: bind address, port,
  database path, retention, and the device id/name list.
* ``.env`` at the repo root — gitignored. WiFi credentials, relay address for
  the firmware, and one bearer token per device (``MORSE_TOKEN_DAD``,
  ``MORSE_TOKEN_EMMA``, ...).

Real environment variables win over ``.env``, which wins over TOML defaults.

Environment overrides:

    MORSE_ENV_FILE   path to the env file (default: <repo>/.env)
    MORSE_CONFIG     path to the TOML file (default: server/config.toml)
    MORSE_HOST       bind host
    MORSE_PORT       bind port
    MORSE_DB         SQLite path (relative paths resolve against the config dir)
    MORSE_TOKEN_<ID> per-device bearer token, e.g. MORSE_TOKEN_DAD
    NTFY_TOKEN       optional ntfy access token (Phase 4)
"""

from __future__ import annotations

import hmac
import os
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from dotenv import load_dotenv

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG_PATH = Path(__file__).with_name("config.toml")
DEFAULT_ENV_PATH = REPO_ROOT / ".env"

DEVICE_ID_RE = re.compile(r"^[a-z][a-z0-9_-]{0,31}$")
RESERVED_IDS = frozenset({"family", "all"})
TOKEN_ENV_PREFIX = "MORSE_TOKEN_"
MIN_TOKEN_LEN = 16
PLACEHOLDER_MARKERS = ("REPLACE", "CHANGE_ME", "YOUR_TOKEN")


class ConfigError(RuntimeError):
    """Raised when the config or its environment is missing or malformed."""


def token_env_name(device_id: str) -> str:
    """dad -> MORSE_TOKEN_DAD; a-b -> MORSE_TOKEN_A_B."""
    return TOKEN_ENV_PREFIX + device_id.upper().replace("-", "_")


@dataclass(frozen=True)
class Device:
    id: str
    name: str
    token: str = field(repr=False)
    peer: bool = True

    @property
    def display_name(self) -> str:
        return self.name or self.id.upper()


@dataclass(frozen=True)
class ServerSettings:
    host: str = "0.0.0.0"
    port: int = 8080
    db: str = "data/morse.db"
    family_name: str = "MORSE"
    retention: int = 1000


@dataclass(frozen=True)
class NtfySettings:
    enabled: bool = False
    publish_topic: str = ""
    subscribe_topic: str = ""
    token: str = field(default="", repr=False)


@dataclass(frozen=True)
class Config:
    server: ServerSettings
    devices: tuple[Device, ...]
    ntfy: NtfySettings
    path: Path

    def device_by_token(self, token: str) -> Device | None:
        """Resolve a bearer token to a device.

        Every configured token is compared so the loop does not leak, through
        timing, which device id was close to the supplied token.
        """
        match: Device | None = None
        for device in self.devices:
            if hmac.compare_digest(device.token, token):
                match = device
        return match

    def device_ids(self) -> frozenset[str]:
        return frozenset(d.id for d in self.devices)

    def peer_ids(self, exclude: str | None = None) -> frozenset[str]:
        return frozenset(d.id for d in self.devices if d.peer and d.id != exclude)

    def db_path(self) -> Path:
        path = Path(self.server.db)
        if not path.is_absolute():
            path = self.path.parent / path
        return path


def _load_env(path: Path) -> None:
    """Load .env without overwriting variables already set in the environment."""
    if path.is_file():
        load_dotenv(path, override=False)


def _read_token(device_id: str, env_path: Path) -> str:
    name = token_env_name(device_id)
    token = os.environ.get(name, "").strip()
    if not token:
        raise ConfigError(
            f"missing {name}: add it to {env_path} "
            "(copy .env.example, then ./scripts/init-env.sh)"
        )
    if token.upper().startswith(PLACEHOLDER_MARKERS):
        raise ConfigError(f"{name} is still a placeholder in {env_path}")
    if len(token) < MIN_TOKEN_LEN:
        raise ConfigError(f"{name} must be at least {MIN_TOKEN_LEN} characters")
    return token


def load_config(path: str | os.PathLike[str] | None = None) -> Config:
    env_path = Path(os.environ.get("MORSE_ENV_FILE") or DEFAULT_ENV_PATH).expanduser()
    _load_env(env_path)

    if path is None:
        path = os.environ.get("MORSE_CONFIG") or DEFAULT_CONFIG_PATH
    config_path = Path(path).expanduser().resolve()

    try:
        raw = tomllib.loads(config_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ConfigError(
            f"{config_path} not found; run from a repo checkout "
            "(server/config.toml is committed)"
        ) from exc
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"{config_path}: invalid TOML: {exc}") from exc

    server_raw = raw.get("server", {})
    if not isinstance(server_raw, dict):
        raise ConfigError("[server] must be a table")

    try:
        port = int(os.environ.get("MORSE_PORT", server_raw.get("port", 8080)))
        retention = int(server_raw.get("retention", 1000))
    except (TypeError, ValueError) as exc:
        raise ConfigError(f"port/retention must be integers: {exc}") from exc
    if not 1 <= port <= 65535:
        raise ConfigError(f"server.port must be a TCP port, got {port}")
    if retention < 10:
        raise ConfigError("server.retention must be at least 10")

    server = ServerSettings(
        host=os.environ.get("MORSE_HOST", str(server_raw.get("host", "0.0.0.0"))),
        port=port,
        db=os.environ.get("MORSE_DB", str(server_raw.get("db", "data/morse.db"))),
        family_name=str(server_raw.get("family_name", "MORSE")),
        retention=retention,
    )

    devices_raw = raw.get("devices", [])
    if not isinstance(devices_raw, list) or not devices_raw:
        raise ConfigError("at least one [[devices]] entry is required")

    devices: list[Device] = []
    seen_ids: set[str] = set()
    seen_tokens: set[str] = set()
    for index, entry in enumerate(devices_raw, start=1):
        if not isinstance(entry, dict):
            raise ConfigError(f"devices[{index}] must be a table")
        if "token" in entry:
            device_id = str(entry.get("id", "?")).strip().lower()
            raise ConfigError(
                f"devices[{index}] ({device_id}): tokens must not live in "
                f"config.toml; put {token_env_name(device_id)} in .env"
            )

        device_id = str(entry.get("id", "")).strip().lower()
        name = str(entry.get("name", "")).strip() or device_id.upper()

        if not DEVICE_ID_RE.match(device_id):
            raise ConfigError(
                f"devices[{index}].id {device_id!r} must match {DEVICE_ID_RE.pattern}"
            )
        if device_id in RESERVED_IDS:
            raise ConfigError(f"device id {device_id!r} is reserved")
        if device_id in seen_ids:
            raise ConfigError(f"duplicate device id {device_id!r}")

        token = _read_token(device_id, env_path)
        if token in seen_tokens:
            raise ConfigError(f"duplicate token for device {device_id!r}")

        seen_ids.add(device_id)
        seen_tokens.add(token)
        devices.append(
            Device(
                id=device_id,
                name=name.upper(),
                token=token,
                peer=bool(entry.get("peer", True)),
            )
        )

    ntfy_raw = raw.get("ntfy", {})
    if not isinstance(ntfy_raw, dict):
        raise ConfigError("[ntfy] must be a table")
    if "token" in ntfy_raw:
        raise ConfigError("[ntfy].token must not live in config.toml; use NTFY_TOKEN")
    ntfy = NtfySettings(
        enabled=bool(ntfy_raw.get("enabled", False)),
        publish_topic=str(ntfy_raw.get("publish_topic", "")),
        subscribe_topic=str(ntfy_raw.get("subscribe_topic", "")),
        token=os.environ.get("NTFY_TOKEN", "").strip(),
    )

    return Config(server=server, devices=tuple(devices), ntfy=ntfy, path=config_path)
