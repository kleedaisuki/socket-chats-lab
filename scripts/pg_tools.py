"""Small libpq connection-string adapter for secret-free psql arguments."""

from __future__ import annotations

import os
from urllib.parse import unquote, urlsplit, urlunsplit


def connection_options(dsn: str) -> dict[str, str]:
    """Parse libpq keyword/value syntax, including single quotes and escapes."""
    fields = {}
    index = 0
    while index < len(dsn):
        while index < len(dsn) and dsn[index].isspace():
            index += 1
        if index == len(dsn):
            break
        start = index
        while index < len(dsn) and dsn[index] != "=" and not dsn[index].isspace():
            index += 1
        key = dsn[start:index]
        while index < len(dsn) and dsn[index].isspace():
            index += 1
        if not key or index == len(dsn) or dsn[index] != "=":
            raise ValueError("invalid PostgreSQL keyword/value connection string")
        index += 1
        while index < len(dsn) and dsn[index].isspace():
            index += 1
        quoted = index < len(dsn) and dsn[index] == "'"
        if quoted:
            index += 1
        value = []
        closed = not quoted
        while index < len(dsn):
            char = dsn[index]
            index += 1
            if char == "\\":
                if index == len(dsn):
                    raise ValueError("trailing escape in PostgreSQL connection string")
                value.append(dsn[index])
                index += 1
            elif quoted and char == "'":
                closed = True
                break
            elif not quoted and char.isspace():
                break
            else:
                value.append(char)
        if not closed or (quoted and index < len(dsn) and not dsn[index].isspace()):
            raise ValueError("invalid quoted PostgreSQL connection value")
        fields[key] = "".join(value)
    return fields


def psql_connection(dsn: str) -> tuple[list[str], dict[str, str]]:
    """Keep passwords in the environment, not command-line process listings."""
    environment = dict(os.environ)
    if dsn.startswith(("postgresql://", "postgres://")):
        uri = urlsplit(dsn)
        # URI query parameters have libpq-specific semantics; reject ambiguous
        # password placement rather than accidentally expose it on the command line.
        if "password" in uri.query.lower():
            raise ValueError("put PostgreSQL URI password in userinfo or use keyword/value syntax")
        netloc = uri.netloc
        if uri.password is not None:
            environment["PGPASSWORD"] = unquote(uri.password)
            netloc = netloc.rsplit("@", 1)[0].split(":", 1)[0] + "@" + netloc.rsplit("@", 1)[1]
        public = urlunsplit((uri.scheme, netloc, uri.path, uri.query, uri.fragment))
    else:
        options = connection_options(dsn)
        password = options.pop("password", None)
        if password is not None:
            environment["PGPASSWORD"] = password
        def quote(value):
            return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"
        public = " ".join(f"{key}={quote(value)}" for key, value in options.items())
    return ["--dbname", public], environment
