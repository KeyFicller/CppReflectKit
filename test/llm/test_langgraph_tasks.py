"""Run LangGraph tools through FastMCP's Tasks extension against the C++ server."""

import asyncio
import json
import re
import threading
from collections import Counter
from contextlib import asynccontextmanager, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import httpx2
import pytest
from fastmcp import Client
from fastmcp.client.transports import StreamableHttpTransport
from langchain.mcp import MCPAdapter
from langchain_core.language_models.fake_chat_models import GenericFakeChatModel
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage
from langgraph.graph import START, MessagesState, StateGraph
from langgraph.prebuilt import ToolNode, tools_condition

BINARY = Path(__file__).resolve().parents[2] / "build" / "CppReflectKit"
RECORDS = range(4)
TIMEOUT = 5

pytestmark = pytest.mark.skipif(
    not BINARY.is_file(), reason=f"{BINARY} not built (run: cmake --build build)"
)


@contextmanager
def upstream_server():
    started = threading.Event()
    release = threading.Event()
    expected = {str(i) for i in RECORDS}
    seen = set()
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            record = self.path.removeprefix("/data?record=")
            with lock:
                seen.add(record)
                if seen == expected:
                    started.set()
            if not release.wait(2 * TIMEOUT):
                self.send_error(503)
                return
            body = f"url-{record}".encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        listener = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05})
        listener.start()
        try:
            yield server.server_port, started, release
        finally:
            release.set()
            server.shutdown()
            listener.join()


@asynccontextmanager
async def io_server(directory, upstream_port):
    process = await asyncio.create_subprocess_exec(
        BINARY, "--test-io-http", directory, str(upstream_port),
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        banner = await asyncio.wait_for(process.stderr.readline(), TIMEOUT)
        match = re.search(rb"http://127\.0\.0\.1:\d+/mcp", banner)
        assert match, banner
        yield match.group().decode()
    finally:
        if process.returncode is None:
            process.terminate()
        await asyncio.wait_for(process.wait(), TIMEOUT)


def run_graph(tools, name):
    calls = [
        {"name": name, "args": {"record": i}, "id": f"{name}-{i}"}
        for i in RECORDS
    ]
    model = GenericFakeChatModel(messages=iter([
        AIMessage(content="", tool_calls=calls),
        AIMessage(content="done"),
    ]))

    async def plan(state):
        return {"messages": [await model.ainvoke(state["messages"])]}

    graph = StateGraph(MessagesState)
    graph.add_node("agent", plan)
    graph.add_node("tools", ToolNode(tools, handle_tool_errors=False))
    graph.add_edge(START, "agent")
    graph.add_conditional_edges("agent", tools_condition)
    graph.add_edge("tools", "agent")
    return graph.compile().ainvoke({"messages": [HumanMessage("Read the fixture records")]})


def assert_graph_results(state, tool, prefix):
    assert state["messages"][-1].content == "done"
    messages = [m for m in state["messages"] if isinstance(m, ToolMessage)]
    assert len(messages) == len(RECORDS)
    assert all(message.status == "success" for message in messages)
    assert {m.tool_call_id: m.content[0]["text"] for m in messages} == {
        f"{tool}-{i}": f"{prefix}-{i}" for i in RECORDS
    }


async def exercise(url, started, release):
    submitted = {}
    polled = {}
    working_polls = Counter()

    async def observe(response):
        method = response.request.headers.get("Mcp-Method")
        if method not in {"tools/call", "tasks/get"}:
            return
        reply = json.loads(await response.aread())
        assert "error" not in reply, reply
        params = json.loads(response.request.content)["params"]
        result = reply["result"]
        if method == "tools/call":
            assert result["resultType"] == "task"
            submitted[result["taskId"]] = params["name"]
        else:
            task_id = params["taskId"]
            assert response.request.headers["Mcp-Name"] == task_id
            polled[task_id] = result["status"]
            if (
                result["status"] == "working"
                and result.get("statusMessage") == "waiting for HTTP"
            ):
                working_polls[task_id] += 1

    def http_client(**kwargs):
        return httpx2.AsyncClient(
            **kwargs, trust_env=False, event_hooks={"response": [observe]}
        )

    async def unexpected_elicitation(*args):
        raise AssertionError("The I/O tools do not request input")

    # An explicit handler keeps MCPAdapter on FastMCP's task-aware call path.
    client = Client(
        StreamableHttpTransport(url, httpx_client_factory=http_client),
        elicitation_handler=unexpected_elicitation,
        timeout=TIMEOUT,
    )
    async with MCPAdapter(client) as adapter:
        tools = await adapter.list_tools()
        assert {tool.name for tool in tools} == {"read_file", "fetch_url"}

        async with asyncio.TaskGroup() as runs:
            url_run = runs.create_task(run_graph(tools, "fetch_url"))
            try:
                assert await asyncio.to_thread(started.wait, TIMEOUT)
                async with asyncio.timeout(TIMEOUT):
                    while sum(count >= 2 for count in working_polls.values()) < len(RECORDS):
                        await asyncio.sleep(0.01)
                url_ids = {key for key, tool in submitted.items() if tool == "fetch_url"}
                assert set(working_polls) == url_ids
                assert all(polled[key] == "working" for key in url_ids)

                async with asyncio.timeout(TIMEOUT):
                    files = await run_graph(tools, "read_file")
                assert_graph_results(files, "read_file", "file")
                assert not url_run.done()
                assert await adapter.client.ping()
            finally:
                release.set()

        assert_graph_results(url_run.result(), "fetch_url", "url")
        assert Counter(submitted.values()) == {
            "fetch_url": len(RECORDS), "read_file": len(RECORDS)
        }
        assert polled == dict.fromkeys(submitted, "completed")


def test_langgraph_polls_concurrent_mcp_tasks(tmp_path):
    # Import registers FastMCP's client extension and its task result resolver.
    pytest.importorskip("fastmcp_tasks", reason="Install fastmcp[tasks] to run this test")
    for i in RECORDS:
        (tmp_path / str(i)).write_text(f"file-{i}", encoding="utf-8")

    with upstream_server() as (port, started, release):
        async def run():
            async with io_server(tmp_path, port) as url:
                async with asyncio.timeout(3 * TIMEOUT):
                    await exercise(url, started, release)

        asyncio.run(run())
