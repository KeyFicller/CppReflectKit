"""Black-box contract tests for the C++ binary that llm_client.py drives.

Everything here talks to the built executable over its CLI and MCP surfaces, so
it needs no model API key and no network. The point is to pin the contract the
Python side relies on: that `--emit-tools` and MCP `tools/list` are the same
table, that `--call` / `--parse` speak JSON, and that the MCP error codes are
the protocol's rather than something ad hoc.

Run from the repository root, after building:

    cmake --build build && pytest test/llm
"""

import json
import os
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))          # test/llm
ROOT = os.path.dirname(os.path.dirname(HERE))               # repository root
SCAFFOLD = os.path.join(HERE, "scaffold")                   # the app under test
BINARY = os.path.join(ROOT, "build", "CppReflectKit")

PROTOCOL_VERSION = "2026-07-28"

pytestmark = pytest.mark.skipif(
    not os.path.exists(BINARY), reason=f"{BINARY} not built (run: cmake --build build)"
)


# --- harness ----------------------------------------------------------------


def run_cli(*args, stdin=None):
    """One CLI request; stdout is always a single JSON document."""
    payload = "" if stdin is None else json.dumps(stdin)
    result = subprocess.run([BINARY, *args], input=payload, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def mcp(*requests):
    """Feed newline-delimited JSON-RPC and return the decoded responses in order."""
    payload = "".join(json.dumps(r) + "\n" for r in requests)
    result = subprocess.run([BINARY, "--mcp"], input=payload, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def request(method, params=None, request_id=1):
    rpc = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        rpc["params"] = params
    return rpc


def openai_tools():
    return run_cli("--emit-tools")


def tool_by_name(tools, name):
    for tool in tools:
        if tool["function"]["name"] == name:
            return tool["function"]
    raise AssertionError(f"no tool {name!r} in {[t['function']['name'] for t in tools]}")


# --- CLI surface ------------------------------------------------------------


def test_emit_tools_is_openai_shaped():
    tools = openai_tools()
    assert isinstance(tools, list) and tools, "expected at least one tool"

    fn = tool_by_name(tools, "search_documents")
    params = fn["parameters"]
    assert params["type"] == "object"
    assert set(params["properties"]) == {"query", "limit", "exact_match"}
    # The reflected signature decides what is required; none of the three is optional.
    assert set(params["required"]) == {"query", "limit", "exact_match"}


def test_call_decodes_arguments_into_the_cpp_signature():
    out = run_cli(
        "--call",
        "search_documents",
        stdin={"query": "reflection", "limit": 2, "exact_match": True},
    )
    assert out["query"] == "reflection"
    assert out["limit"] == 2
    assert out["exact_match"] is True
    assert len(out["hits"]) == 2


def test_search_documents_returns_real_corpus_hits():
    """The tool reads the repository's own docs, so a hit is a real place."""
    out = run_cli(
        "--call",
        "search_documents",
        stdin={"query": "reflection", "limit": 5, "exact_match": True},
    )
    assert out["hits"], "the corpus yields hits for 'reflection'"
    for hit in out["hits"]:
        path = os.path.join(ROOT, hit["file"])
        assert os.path.exists(path), f"hit names a file that exists: {hit['file']}"
        assert "reflection" in hit["text"], "an exact-match hit contains the query"
        assert hit["line"] >= 1


def test_call_reports_bad_arguments_as_an_error():
    result = subprocess.run(
        [BINARY, "--call", "search_documents"],
        input=json.dumps({"query": "x", "limit": "not-an-int", "exact_match": True}),
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "search_documents" in result.stderr


def test_emit_schemas_covers_the_registered_models():
    # Each entry is an OpenAI tool wrapper around the reflected parameter object.
    schemas = run_cli("--emit-schemas")
    assert {"Range", "Query", "SearchRequest"} <= set(schemas)
    for name, wrapper in schemas.items():
        assert wrapper["type"] == "function"
        assert wrapper["function"]["name"] == name
        assert wrapper["function"]["parameters"]["type"] == "object"
        assert wrapper["function"]["parameters"]["additionalProperties"] is False
    # A required member of a nested struct propagates up into its parent.
    assert schemas["SearchRequest"]["function"]["parameters"]["required"] == ["query"]


def test_parse_reconstructs_a_struct_from_model_json():
    payload = {
        "query": {"text": "static reflection", "ranges": [], "fuzzy": True},
        "top_k": [3, 5, 8],
        "debug": False,
    }
    out = run_cli("--parse", "SearchRequest", stdin=payload)
    assert out["query"]["text"] == "static reflection"
    assert out["query"]["fuzzy"] is True
    assert out["top_k"] == [3, 5, 8]
    assert out["debug"] is False


def test_parse_rejects_an_unknown_model():
    result = subprocess.run(
        [BINARY, "--parse", "NoSuchModel"],
        input=json.dumps({}),
        capture_output=True,
        text=True,
    )
    assert result.returncode != 0
    assert "NoSuchModel" in result.stderr


# --- MCP surface ------------------------------------------------------------


def test_discover_reports_the_protocol_version_and_identity():
    (response,) = mcp(request("server/discover"))
    result = response["result"]
    assert result["supportedVersions"] == [PROTOCOL_VERSION]
    assert result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "cppreflect"


def test_tools_list_and_emit_tools_are_one_table():
    """The two emitters must never drift: OpenAI tools vs MCP tools/list."""
    (response,) = mcp(request("tools/list"))
    listed = {t["name"]: t["inputSchema"] for t in response["result"]["tools"]}
    emitted = {t["function"]["name"]: t["function"]["parameters"] for t in openai_tools()}
    assert listed == emitted


def test_tool_call_returns_text_content():
    (response,) = mcp(
        request(
            "tools/call",
            {
                "name": "search_documents",
                "arguments": {"query": "docs", "limit": 1, "exact_match": False},
            },
        )
    )
    result = response["result"]
    assert result["isError"] is False
    assert result["content"][0]["type"] == "text"
    assert json.loads(result["content"][0]["text"])["query"] == "docs"


def test_prompts_and_resources_are_exposed():
    prompts, resources = mcp(request("prompts/list", request_id=1), request("resources/list", request_id=2))
    assert [p["name"] for p in prompts["result"]["prompts"]] == ["explain"]
    uris = [r["uri"] for r in resources["result"]["resources"]]
    assert "file:///docs/about_llm.md" in uris


def test_resource_read_serves_the_named_file():
    (response,) = mcp(request("resources/read", {"uri": "file:///docs/about_llm.md"}))
    text = response["result"]["contents"][0]["text"]
    with open(os.path.join(ROOT, "docs", "about_llm.md"), encoding="utf-8") as handle:
        assert text == handle.read(), "the resource serves the file the URI names"


def test_unknown_tool_is_invalid_params():
    (response,) = mcp(request("tools/call", {"name": "nope", "arguments": {}}))
    assert response["error"]["code"] == -32602


def test_malformed_json_is_a_parse_error():
    result = subprocess.run([BINARY, "--mcp"], input="not json\n", capture_output=True, text=True)
    (response,) = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    assert response["error"]["code"] == -32700


def test_unsupported_protocol_version_is_rejected():
    (response,) = mcp(
        request(
            "tools/list",
            {"_meta": {"io.modelcontextprotocol/protocolVersion": "1999-01-01"}},
        )
    )
    assert response["error"]["code"] == -32022
    assert response["error"]["data"]["supported"] == [PROTOCOL_VERSION]


# --- the Python client's own pure logic -------------------------------------


@pytest.fixture(scope="module")
def llm_client():
    # llm_client.py is part of the scaffolding, in scaffold/ beside this file.
    if SCAFFOLD not in sys.path:
        sys.path.insert(0, SCAFFOLD)
    return pytest.importorskip("llm_client", reason="langchain/fastmcp deps not installed")


def test_parse_mcp_args_splits_prompt_arguments_from_the_question(llm_client):
    prompt, args, resource, question = llm_client.parse_mcp_args(
        ["--prompt", "explain", "topic=reflection", "what", "is", "meta"]
    )
    assert prompt == "explain"
    assert args == {"topic": "reflection"}
    assert resource is None
    assert question == "what is meta"


def test_parse_mcp_args_reads_a_resource(llm_client):
    prompt, args, resource, question = llm_client.parse_mcp_args(
        ["--resource", "file:///docs/about_llm.md", "summarize"]
    )
    assert (prompt, args, question) == (None, {}, "summarize")
    assert resource == "file:///docs/about_llm.md"


def test_request_timeout_honours_the_env_override(llm_client, monkeypatch):
    monkeypatch.delenv("CPP_REFLECT_TIMEOUT", raising=False)
    assert llm_client.request_timeout() == llm_client.DEFAULT_TIMEOUT
    monkeypatch.setenv("CPP_REFLECT_TIMEOUT", "0")
    assert llm_client.request_timeout() is None
    monkeypatch.setenv("CPP_REFLECT_TIMEOUT", "5")
    assert llm_client.request_timeout() == 5.0


def test_client_binary_path_matches_this_harness(llm_client):
    assert llm_client.BINARY == BINARY
