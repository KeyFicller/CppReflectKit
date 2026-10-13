"""Can a real MCP client connect? Pins the client-side connect policy.

The C++ suite (test_mcp.cpp) drives the transport by hand: it pipes
newline-delimited JSON-RPC straight at the server. This file runs the other
half of the contract -- a client library owning discovery, era negotiation and
the HTTP headers, which is where a version mismatch actually surfaces.

`mcp` here is the reference Python SDK (2.x, modern revision). Cursor bundles
the TypeScript SDK, whose newest era is one revision behind, so the tests at the
bottom walk that client's actual session -- handshake, then version-only
headers -- over both transports.

Run from the repository root, after building:
    cmake --build build && pytest test/llm
"""

import asyncio
import contextlib
import json
import os
import re
import subprocess

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))          # test/llm
ROOT = os.path.dirname(os.path.dirname(HERE))               # repository root
BINARY = os.path.join(ROOT, "build", "CppReflectKit")

PROTOCOL_VERSION = "2026-07-28"

# What Cursor's bundled client announces: @modelcontextprotocol/sdk 1.25.1 has
# LATEST_PROTOCOL_VERSION = "2025-11-25" and no knowledge of server/discover.
HANDSHAKE_VERSION = "2025-11-25"

# Streamable HTTP is strict about Accept for every POST; the era changes which
# headers accompany it, not this one.
STREAMABLE_ACCEPT = {"Accept": "application/json, text/event-stream"}

pytestmark = pytest.mark.skipif(
    not os.path.exists(BINARY), reason=f"{BINARY} not built (run: cmake --build build)")

SEARCH_ARGUMENTS = {"query": "reflection", "limit": 1, "exact_match": True}


# --- the connect path every modern client walks -----------------------------


async def _exercise(session):
    """discover -> tools/list -> tools/call, over whatever transport it is."""
    discovered = await session.discover()
    tools = await session.list_tools()
    result = await session.call_tool("search_documents", SEARCH_ARGUMENTS)
    return discovered, tools, result


async def _connect_stdio():
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    params = StdioServerParameters(command=BINARY, args=["--mcp"])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            return await _exercise(session)


async def _connect_http(url):
    import httpx2
    from mcp import ClientSession
    from mcp.client.streamable_http import streamable_http_client

    # The SDK's default client has trust_env=True, and macOS publishes an HTTP
    # proxy for getproxies() to find. Handing it a loopback-safe client keeps
    # the test measuring the server rather than the machine's proxy settings.
    async with httpx2.AsyncClient(trust_env=False) as http_client:
        async with streamable_http_client(url, http_client=http_client) as (read, write):
            async with ClientSession(read, write) as session:
                return await _exercise(session)


async def _connect_stdio_legacy():
    """The reference SDK's own handshake-era path: initialize, then work."""
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    params = StdioServerParameters(command=BINARY, args=["--mcp"])
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            agreed = await session.initialize()
            assert agreed.protocol_version == HANDSHAKE_VERSION
            tools = await session.list_tools()
            result = await session.call_tool("search_documents", SEARCH_ARGUMENTS)
            return tools, result


@contextlib.contextmanager
def http_server():
    """Start the app's Streamable HTTP listener on an OS-chosen port.

    Port 0 avoids guessing; the bound port is what the server announces on
    stderr, so the test never races another process for a fixed number.
    """
    process = subprocess.Popen(
        [BINARY, "--http", "0"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        banner = process.stderr.readline()
        match = re.search(r"http://127\.0\.0\.1:(\d+)/mcp", banner)
        assert match, f"server never announced its port; stderr said {banner!r}"
        yield f"http://127.0.0.1:{match.group(1)}/mcp"
    finally:
        process.terminate()
        process.wait(timeout=5)


def assert_exercised(discovered, tools, result):
    assert discovered.supported_versions == [PROTOCOL_VERSION]
    assert [t.name for t in tools.tools] == ["search_documents"]
    assert result.is_error is False
    assert json.loads(result.content[0].text)["query"] == "reflection"


def test_a_modern_stdio_client_connects():
    pytest.importorskip("mcp", reason="the reference SDK is not installed")
    assert_exercised(*asyncio.run(_connect_stdio()))


def test_a_legacy_sdk_client_connects():
    """The reference SDK's handshake-era path, end to end over stdio."""
    pytest.importorskip("mcp", reason="the reference SDK is not installed")
    tools, result = asyncio.run(_connect_stdio_legacy())
    assert [t.name for t in tools.tools] == ["search_documents"]
    assert result.is_error is False


def test_a_modern_http_client_connects():
    pytest.importorskip("mcp", reason="the reference SDK is not installed")
    with http_server() as url:
        assert_exercised(*asyncio.run(_connect_http(url)))


def test_a_handshake_era_http_client_connects():
    """The same era over Streamable HTTP.

    The handshake request carries no modern header; every request after it
    carries MCP-Protocol-Version alone, because that header predates the
    revision that added Mcp-Method. Neither shape may be bounced by the modern
    header contract.
    """
    httpx = pytest.importorskip("httpx", reason="httpx is not installed")
    with http_server() as url:
        # trust_env=False: macOS publishes a system proxy that would otherwise
        # carry the loopback request away from the server under test.
        session = httpx.Client(trust_env=False, timeout=5)
        # What the handshake-era transport knows how to send once it is past the
        # handshake: the version, and no Mcp-Method.
        after_handshake = {**STREAMABLE_ACCEPT, "MCP-Protocol-Version": HANDSHAKE_VERSION}
        try:
            agreed = session.post(
                url, json=initialize({"name": "cursor", "version": "1.25.1"}),
                headers=STREAMABLE_ACCEPT)
            assert agreed.status_code == 200, agreed.text
            assert agreed.json()["result"]["protocolVersion"] == HANDSHAKE_VERSION

            listed = session.post(
                url, json={"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
                headers=after_handshake)
            assert listed.status_code == 200, listed.text
            assert [t["name"] for t in listed.json()["result"]["tools"]] == ["search_documents"]
            assert "_meta" not in listed.json()["result"], "the handshake era carries no result metadata"
            assert "resultType" not in listed.json()["result"]

            called = session.post(
                url,
                json={
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {"name": "search_documents", "arguments": SEARCH_ARGUMENTS},
                },
                headers=after_handshake)
            assert called.status_code == 200, called.text
            assert called.json()["result"]["isError"] is False
            assert "_meta" not in called.json()["result"], "the whole session stays in that era"
        finally:
            session.close()


# --- the era Cursor is still in ---------------------------------------------


def mcp(*requests):
    """Feed newline-delimited JSON-RPC to the stdio entry point."""
    payload = "".join(json.dumps(r) + "\n" for r in requests)
    result = subprocess.run([BINARY, "--mcp"], input=payload, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def initialize(client_info, protocol_version=HANDSHAKE_VERSION, request_id=1):
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "method": "initialize",
        "params": {
            "protocolVersion": protocol_version,
            "capabilities": {},
            "clientInfo": client_info,
        },
    }


def test_a_handshake_era_client_connects():
    """Cursor opens with `initialize` and never probes `server/discover`.

    Its transport then lists and calls with no `Mcp-Method` or
    `MCP-Protocol-Version` header to speak of, so the whole session is the
    handshake era -- the newest one we answer, with none of the modern result
    metadata the handshake predates.
    """
    responses = mcp(
        initialize({"name": "cursor", "version": "1.25.1"}),
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "search_documents", "arguments": SEARCH_ARGUMENTS},
        },
    )
    agreed, listed, called = responses

    result = agreed["result"]
    assert result["protocolVersion"] == HANDSHAKE_VERSION
    assert result["serverInfo"] == {"name": "cppreflect", "version": "0.1.0"}
    assert set(result["capabilities"]) == {"tools", "prompts", "resources"}
    assert "_meta" not in result, "the handshake predates result metadata"
    assert "resultType" not in result

    assert [t["name"] for t in listed["result"]["tools"]] == ["search_documents"]
    assert called["result"]["isError"] is False
    assert "_meta" not in listed["result"], "the handshake era carries no result metadata"
    assert "resultType" not in listed["result"]
    assert "_meta" not in called["result"], "the whole session stays in that era"
    assert "resultType" not in called["result"]


def test_an_older_handshake_revision_is_corrected():
    """Only the newest handshake era is answered; a request for an older
    revision is answered at that newest one, which is what the handshake says
    the server does -- not a silent downgrade to a dialect we do not implement.
    """
    (response,) = mcp(initialize({"name": "stale", "version": "0"}, protocol_version="2024-11-05"))
    assert response["result"]["protocolVersion"] == HANDSHAKE_VERSION

