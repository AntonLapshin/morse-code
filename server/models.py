"""Request models for the relay API.

Response bodies are assembled as plain dicts in ``app.py``: they are small,
read-only views over rows, and keeping them dicts makes the wire format obvious
when comparing against the firmware and ``curl`` output.
"""

from __future__ import annotations

from typing import Annotated

from pydantic import BaseModel, Field, field_validator

MAX_TEXT_BYTES = 512
MAX_CLIENT_MSG_ID = 64
FAMILY = "family"

ClientMsgId = Annotated[str, Field(min_length=1, max_length=MAX_CLIENT_MSG_ID)]
UptoSeq = Annotated[int, Field(ge=0)]


class SendMessageRequest(BaseModel):
    client_msg_id: ClientMsgId
    to: str = FAMILY
    text: str

    @field_validator("client_msg_id")
    @classmethod
    def _clean_client_msg_id(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("client_msg_id must not be empty")
        if not all(0x21 <= ord(ch) <= 0x7E for ch in value):
            raise ValueError("client_msg_id must be printable ASCII without spaces")
        return value

    @field_validator("to")
    @classmethod
    def _clean_to(cls, value: str) -> str:
        return value.strip().lower()

    @field_validator("text")
    @classmethod
    def _clean_text(cls, value: str) -> str:
        value = value.strip()
        if not value:
            raise ValueError("text must not be empty")
        if len(value.encode("utf-8")) > MAX_TEXT_BYTES:
            raise ValueError(f"text must be at most {MAX_TEXT_BYTES} bytes")
        return value


class AckRequest(BaseModel):
    upto_seq: UptoSeq
