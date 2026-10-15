"""Authenticated standard Streamable HTTP transport, independent of any AI vendor."""
from __future__ import annotations
import hmac
import ipaddress
import os
import sys
from pathlib import Path
from urllib.parse import urlsplit

from mcp.server.transport_security import TransportSecuritySettings
from starlette.responses import JSONResponse


def connection_data_directory() -> Path:
    if value := os.environ.get("VGS_EDITOR_MCP_DATA_DIR"):
        return Path(value)
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library/Application Support"
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    return base / "VGS" / "VGS Editor" / "mcp-client"


def validate_token(token: str) -> str:
    token = token.strip()
    if len(token) < 32 or not token.isascii() or any(c.isspace() for c in token):
        raise ValueError("HTTP token must contain at least 32 non-whitespace ASCII characters.")
    return token


def read_token(path: Path | None) -> str:
    if token := os.environ.get("VGS_EDITOR_MCP_HTTP_TOKEN"):
        return validate_token(token)
    if path is None or not path.is_file():
        raise ValueError("HTTP requires VGS_EDITOR_MCP_HTTP_TOKEN or a token file. Run install.ps1 to create per-user connection profiles.")
    if path.stat().st_size > 4096:
        raise ValueError("Invalid HTTP token file.")
    return validate_token(path.read_text(encoding="utf-8"))


def transport_security(host: str, public_url: str | None = None) -> TransportSecuritySettings:
    try:
        loopback = ipaddress.ip_address(host).is_loopback
    except ValueError:
        loopback = host == "localhost"
    if not loopback and not public_url:
        raise ValueError("A non-loopback bind requires --public-url with the HTTPS endpoint provided by your proxy.")
    hosts = ["127.0.0.1:*", "localhost:*", "[::1]:*"]
    origins = ["http://127.0.0.1:*", "http://localhost:*", "http://[::1]:*"]
    if public_url:
        url = urlsplit(public_url)
        if (url.scheme != "https" or not url.hostname or url.username or url.password
                or url.path.rstrip("/") != "/mcp" or url.query or url.fragment):
            raise ValueError("--public-url must be an HTTPS URL ending in /mcp, without credentials, query or fragment.")
        hosts.append(url.netloc)
        origins.append(f"https://{url.netloc}")
    return TransportSecuritySettings(enable_dns_rebinding_protection=True, allowed_hosts=hosts, allowed_origins=origins)


class BearerAuth:
    """Protect HTTP requests while passing ASGI lifespan events to the SDK unchanged."""
    def __init__(self, app, token: str):
        self.app = app
        self.expected = validate_token(token).encode("ascii")

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http":
            supplied = [v for k, v in scope.get("headers", []) if k.lower() == b"authorization"]
            scheme, _, credential = supplied[0].partition(b" ") if len(supplied) == 1 else (b"", b"", b"")
            if scheme.lower() != b"bearer" or not hmac.compare_digest(credential, self.expected):
                response = JSONResponse({"error": "unauthorized"}, status_code=401,
                                        headers={"WWW-Authenticate": "Bearer"})
                await response(scope, receive, send)
                return
        await self.app(scope, receive, send)


def build_http_app(server, token: str, host: str = "127.0.0.1", public_url: str | None = None):
    security = transport_security(host, public_url)
    # Stateless HTTP works with both older initialize clients and current discovery.
    # A running endpoint controls one editor selection; separate endpoints isolate users.
    app = server.streamable_http_app(host=host, stateless_http=True, json_response=True,
                                     transport_security=security)
    return BearerAuth(app, token)
