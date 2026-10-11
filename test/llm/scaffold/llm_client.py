"""LLM clients over the C++ reflection code. Three modes, one entry point.

The C++ binary owns the contract in both directions; Python never redeclares a
signature:

  tools       --emit-tools        -> the OpenAI tool schemas the model is given
              --call <name>       <- tool-call arguments, real C++ call out
  mcp         (langchain.mcp)     -> the same tool table, discovered over MCP
              MCPAdapter          <- tool calls, on one FastMCP-owned session
  structured  --emit-schemas      -> OpenAI tool wrappers for registered structs
              --parse <name>      <- the model's JSON, reconstructed struct out

Usage:
  python test/llm/scaffold/llm_client.py tools      [question]
  python test/llm/scaffold/llm_client.py mcp        [--prompt NAME [key=value ...]] [--resource URI] [question]
  python test/llm/scaffold/llm_client.py structured [request] [format]

Positional order is strict: the first argument is always the request text, the
second always the format (registered struct name, default "SearchRequest").
Pass both or neither, so a lone argument is rejected rather than silently taken
as either slot. To target a format, pass the request too, e.g.
`structured "find reflection docs" Query`.

Endpoint quirks this works around:
  * response_format "json_schema" is rejected ("unavailable now"), so a schema
    is sent as a tool/function schema instead, which does work here.
  * that path forces tool_choice, and the model runs in thinking mode by
    default, which refuses a forced tool_choice ("Thinking mode does not
    support this tool_choice"). reasoning_effort="none" clears it.
  * the server is not bound by "strict" even then, so conformance is proven on
    the C++ side, where parsing fails loudly on a bad enum or type.

Joint debugging: set CPP_REFLECT_SOCKET to a running `CppReflectKit --serve`
socket and run_cpp() talks to it instead of spawning a process, so a debugger
holding that process keeps its breakpoints across a whole LLM loop.
"""

import asyncio
import json
import os
import socket
import subprocess
import sys

from fastmcp import Client
from langchain.mcp import MCPAdapter
from langchain_core.messages import AIMessage, HumanMessage, SystemMessage, ToolMessage
from langchain_deepseek import ChatDeepSeek

# This file lives in test/llm/scaffold/; both the binary it drives and the .env
# it reads sit at the repository root.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
BINARY = os.path.join(ROOT, "build", "CppReflectKit")
MODEL = "deepseek-flash"

# One C++ request must not hang forever. Override with CPP_REFLECT_TIMEOUT
# (seconds); 0 means wait indefinitely, which is what joint debugging needs when
# the debugger is holding the process at a breakpoint.
DEFAULT_TIMEOUT = 30.0

DEFAULT_QUESTION = (
    "Find documents about C++26 static reflection using Fahrenheit, "
    "top 3 results, and require an exact match."
)
DEFAULT_REQUEST = (
    "I want exact matches for 'static reflection', in Fahrenheit, "
    "with results between 1 and 10 and above 20, matching loosely, "
    "weighted 0.75, and I need 3, 5 and 8 hits. No debug output."
)


def load_env(path: str) -> None:
    """Minimal .env loader, so python-dotenv is not a dependency."""
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def request_timeout() -> float | None:
    """Per-request timeout in seconds, or None for no limit (CPP_REFLECT_TIMEOUT)."""
    raw = os.environ.get("CPP_REFLECT_TIMEOUT")
    if raw is None:
        return DEFAULT_TIMEOUT
    seconds = float(raw)
    return seconds if seconds > 0 else None


def run_cpp(*args: str, stdin: dict | None = None) -> str:
    """Run one request and return its stdout text, raising on a non-zero exit.

    With CPP_REFLECT_SOCKET set, the request goes to an already-running `--serve`
    process over a Unix socket instead of a freshly spawned one. That process is
    launched by the debugger, which keeps its breakpoints live across the whole
    session; the channel has to be out of band, because stdin/stdout belong to
    the debugger.
    """
    address = os.environ.get("CPP_REFLECT_SOCKET")
    timeout = request_timeout()
    if address:
        return _run_over_socket(address, args, stdin, timeout=timeout)

    try:
        result = subprocess.run(
            [BINARY, *args],
            input=json.dumps(stdin) if stdin is not None else None,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(
            "C++ binary not found at {}; build it with `cmake --build build`".format(BINARY)
        ) from exc
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(
            "{} timed out after {}s".format(" ".join(args), timeout)
        ) from exc
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "{} failed".format(" ".join(args)))
    return result.stdout


def _run_over_socket(
    address: str, args: tuple[str, ...], stdin: dict | None, *, timeout: float | None
) -> str:
    """One request/response line against a `--serve` process."""
    request = json.dumps({"args": list(args), "stdin": stdin}) + "\n"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        if timeout is not None:
            sock.settimeout(timeout)
        try:
            sock.connect(address)
            sock.sendall(request.encode())
            with sock.makefile("r", encoding="utf-8") as stream:
                line = stream.readline()
        except socket.timeout as exc:
            raise RuntimeError(
                "request to --serve at {} timed out after {}s "
                "(set CPP_REFLECT_TIMEOUT=0 if a breakpoint is holding it)".format(address, timeout)
            ) from exc
        except OSError as exc:
            raise RuntimeError("cannot reach --serve at {}: {}".format(address, exc)) from exc
    if not line:
        raise RuntimeError("--serve at {} closed the connection without replying".format(address))
    reply = json.loads(line)
    if "error" in reply:
        raise RuntimeError(reply["error"])
    return reply["stdout"]


def make_model(*, force_tool: bool = False) -> ChatDeepSeek:
    """A chat model wired for this endpoint.

    force_tool disables thinking mode. A forced tool_choice is otherwise rejected
    ("Thinking mode does not support this tool_choice"), and forcing one is what
    with_structured_output does. Plain tool calling forces nothing, so thinking
    can stay on and the model gets more room to decide.
    """
    return ChatDeepSeek(
        model=MODEL,
        temperature=0,
        **({"reasoning_effort": "none"} if force_tool else {}),
    )


# --- tools mode --------------------------------------------------------------


def emit_tools() -> list[dict]:
    """Ask the C++ binary for the tool schemas it exposes."""
    return json.loads(run_cpp("--emit-tools"))


def call_tool(name: str, arguments: dict) -> str:
    """Run one tool call in C++ with the model-supplied arguments."""
    return run_cpp("--call", name, stdin=arguments).strip()


def run_tools(question: str) -> int:
    tools = emit_tools()
    print("tools from C++: {}\n".format([t["function"]["name"] for t in tools]))

    model = make_model()
    model_with_tools = model.bind_tools(tools)

    messages = [HumanMessage(question)]
    reply = model_with_tools.invoke(messages)

    tool_calls = getattr(reply, "tool_calls", None) or []
    if not tool_calls:
        print("model answered without a tool call:\n", reply.content)
        return 0

    messages.append(reply)
    for call in tool_calls:
        print("model called {} with {}".format(call["name"], json.dumps(call["args"])))
        output = call_tool(call["name"], call["args"])
        print("C++ returned: {}\n".format(output))
        messages.append(ToolMessage(output, tool_call_id=call["id"]))

    final = model.invoke(messages)
    print("final answer:\n", final.content)
    return 0


# --- mcp mode ----------------------------------------------------------------


def mcp_server_config() -> dict:
    """The `mcpServers` entry that launches our own server as a stdio child.

    A `Path` target would only name a script, with no way to pass `--mcp`, so the
    config dict is what carries the argument.
    """
    return {"mcpServers": {"cppreflect": {"command": BINARY, "args": ["--mcp"]}}}


async def list_catalog(adapter: MCPAdapter) -> list:
    """Discover what the server offers, then hand back the tools.

    Listing is a client-side operation for all three primitives; only tools are
    then given to the model.
    """
    client = adapter.client
    tools = await adapter.list_tools()
    print("tools     from MCP: {}".format([t.name for t in tools]))
    for prompt in await client.list_prompts():
        print(
            "prompt    from MCP: {} {}  <- {}".format(
                prompt.name, [a.name for a in (prompt.arguments or [])], prompt.description
            )
        )
    for resource in await client.list_resources():
        print("resource  from MCP: {} [{}]".format(resource.uri, resource.mime_type))
    print()
    return tools


# MCP prompts may only speak as the user or the assistant; there is no system role.
PROMPT_ROLES = {"user": HumanMessage, "assistant": AIMessage}


async def seed_from_prompt(client: Client, name: str, arguments: dict) -> list:
    """prompts/get: the user picked a template, so its messages open the chat.

    Prompts are user-controlled, so this is opt-in from the command line -- never
    something the model chooses.
    """
    result = await client.get_prompt(name, arguments)
    messages = []
    for message in result.messages:
        factory = PROMPT_ROLES.get(message.role)
        if factory is None:
            raise SystemExit("prompt {!r} used unsupported role {!r}".format(name, message.role))
        messages.append(factory(message.content.text))
    print("prompts/get {} -> {} message(s)\n".format(name, len(messages)))
    return messages


async def attach_resource(client: Client, uri: str) -> SystemMessage:
    """resources/read: the *application* chose this URI, so it becomes context.

    Resources are application-controlled -- the model never asks for one; it only
    sees the text because we attached it.
    """
    contents = await client.read_resource(uri)
    text = "".join(c.text for c in contents if hasattr(c, "text"))
    print("resources/read {} -> {} char(s) attached as context\n".format(uri, len(text)))
    return SystemMessage("Attached resource {}:\n{}".format(uri, text))


async def run_mcp(
    question: str,
    prompt_name: str | None = None,
    prompt_arguments: dict | None = None,
    resource_uri: str | None = None,
) -> int:
    """`tools` mode, with the whole MCP surface in play -- each part by its own actor.

    LangChain's own `MCPAdapter` (FastMCP underneath) owns the transport, the
    session and protocol negotiation -- including picking the modern era by
    probing `server/discover`, so the revision stays the C++ side's to declare.

    The three primitives are used the way MCP defines them: tools are
    model-controlled, the prompt is the user's choice from argv, and the resource
    is this application's choice of context.
    """
    async with MCPAdapter(mcp_server_config()) as adapter:
        client = adapter.client
        tools = await list_catalog(adapter)

        messages = []
        if prompt_name is not None:
            messages += await seed_from_prompt(client, prompt_name, prompt_arguments or {})
        if resource_uri is not None:
            messages.append(await attach_resource(client, resource_uri))
        if question:
            messages.append(HumanMessage(question))

        by_name = {t.name: t for t in tools}
        model = make_model().bind_tools(tools)
        reply = await model.ainvoke(messages)

        tool_calls = getattr(reply, "tool_calls", None) or []
        if not tool_calls:
            print("model answered without a tool call:\n", reply.content)
            return 0

        messages.append(reply)
        for call in tool_calls:
            print("model called {} with {}".format(call["name"], json.dumps(call["args"])))
            output = await by_name[call["name"]].ainvoke(call["args"])
            print("MCP returned: {}\n".format(output))
            messages.append(ToolMessage(output, tool_call_id=call["id"]))

        final = await model.ainvoke(messages)
        print("final answer:\n", final.content)
    return 0


def parse_mcp_args(rest: list[str]) -> tuple[str | None, dict, str | None, str]:
    """Split `mcp` argv into (prompt name, prompt args, resource URI, question).

    Prompts and resources are not model-selectable, so both are opt-in here:
    `--prompt` is the user picking a template, `--resource` is this application
    picking context. `key=value` tokens directly after a prompt name are its
    arguments; bare words are the question.
    """
    prompt: str | None = None
    arguments: dict[str, str] = {}
    resource: str | None = None
    words: list[str] = []

    index = 0
    while index < len(rest):
        token = rest[index]
        if token == "--prompt":
            if index + 1 >= len(rest):
                raise SystemExit("--prompt needs a name")
            prompt = rest[index + 1]
            index += 2
            while index < len(rest) and "=" in rest[index] and not rest[index].startswith("--"):
                key, _, value = rest[index].partition("=")
                arguments[key] = value
                index += 1
            continue
        if token == "--resource":
            if index + 1 >= len(rest):
                raise SystemExit("--resource needs a URI")
            resource = rest[index + 1]
            index += 2
            continue
        words.append(token)
        index += 1
    return prompt, arguments, resource, " ".join(words)


# --- structured mode ---------------------------------------------------------


def emit_response_schemas() -> dict:
    """Every registered model's OpenAI tool wrapper, keyed by name."""
    return json.loads(run_cpp("--emit-schemas"))


def parse_model(name: str, payload: dict) -> str:
    """Hand the model's JSON to C++ and get the reconstructed struct back."""
    return run_cpp("--parse", name, stdin=payload).rstrip()


def run_structured(request: str, format: str) -> int:
    schemas = emit_response_schemas()
    if format not in schemas:
        print("unknown format {!r}; registered: {}".format(format, list(schemas)))
        return 1

    # The C++ side already emits a full OpenAI tool, so it is passed through
    # untouched; no wrapper is assembled here.
    tool = schemas[format]
    print("registered formats: {}".format(list(schemas)))
    print("tool schema for {} ({} bytes)\n".format(format, len(json.dumps(tool))))

    structured = make_model(force_tool=True).with_structured_output(
        tool, method="function_calling"
    )

    extracted = structured.invoke([HumanMessage(request)])
    print("model returned:")
    print(json.dumps(extracted, indent=2), "\n")

    print("parsed back into app::{} by C++:".format(format))
    print(parse_model(format, extracted))
    return 0


# --- entry point -------------------------------------------------------------


def main() -> int:
    load_env(os.path.join(ROOT, ".env"))

    mode = sys.argv[1] if len(sys.argv) > 1 else "tools"
    rest = sys.argv[2:]

    if mode == "tools":
        return run_tools(rest[0] if rest else DEFAULT_QUESTION)
    if mode == "mcp":
        prompt_name, prompt_arguments, resource_uri, question = parse_mcp_args(rest)
        if not question and prompt_name is None:
            question = DEFAULT_QUESTION
        return asyncio.run(run_mcp(question, prompt_name, prompt_arguments, resource_uri))
    if mode == "structured":
        # Strict positional order: request first, format second. Either pass
        # both or neither, so a lone argument can never be silently mistaken
        # for the other slot.
        if len(rest) not in (0, 2):
            print(
                "structured expects 0 or 2 arguments (got {}); "
                "usage: structured [request] [format]".format(len(rest))
            )
            return 2
        return run_structured(
            rest[0] if rest else DEFAULT_REQUEST,
            rest[1] if rest else "SearchRequest",
        )

    print("unknown mode {!r}; use 'tools', 'mcp' or 'structured'".format(mode))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
