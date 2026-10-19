"""The elicitation round trip, run as a LangGraph agent with a fake model.

`ask_confirmation` cannot finish without the user: the first `tools/call`
returns an `input_required` result holding the form, and the client is expected
to answer and retry the *same* call with `inputResponses`. That turns the
exchange into a human-in-the-loop step, so the graph gives it a node of its own:

    agent --tool_calls--> tools --pending--> responder --answer--> tools --> agent
                           |                                          |
                           +---------- no form: an ordinary tool -----+

No language model is involved: langchain's fake chat model plays both model
parts -- the planner picks the tool, and the responder plays the user filling
the form -- so the run is deterministic and needs no API key.

The server is stateless, so every leg is its own `--mcp` process: the
capability, the form and the answer all travel on the request, never on the
connection. To debug the C++ side, point `CPP_REFLECT_MCP_SOCKET` at a running
`--serve` and the legs go over that socket instead, keeping one process under
the debugger for the whole run (see `.vscode/launch.json`).

Run from the repository root, after building:
    cmake --build build && pytest test/llm
"""

import json
import os
import socket
import subprocess
import time

import pytest
from langchain_core.language_models.fake_chat_models import GenericFakeChatModel
from langchain_core.messages import AIMessage, HumanMessage, ToolMessage
from langgraph.graph import END, START, MessagesState, StateGraph

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BINARY = os.path.join(ROOT, "build", "CppReflectKit")

PROTOCOL_VERSION = "2026-07-28"

pytestmark = pytest.mark.skipif(
    not os.path.exists(BINARY), reason=f"{BINARY} not built (run: cmake --build build)")

# What a modern client declares on every request. Without `form` the server
# must not ask at all.
FORM_CAPABILITY = {
    "io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION,
    "io.modelcontextprotocol/clientCapabilities": {"elicitation": {"form": {}}},
}
NO_CAPABILITY = {"io.modelcontextprotocol/protocolVersion": PROTOCOL_VERSION}

QUESTION = "Delete the stale index?"


#: Set to a `--serve` socket path to send MCP through a process the debugger
#: already holds, instead of spawning one per leg. See `.vscode/launch.json`.
SOCKET_ENV = "CPP_REFLECT_MCP_SOCKET"


def over_socket(address, request):
    """One `--mcp` exchange on a running `--serve` process.

    The socket carries `{"args": [...], "stdin": ...}` one per line, exactly
    what llm_client.py sends for the CLI commands; `--mcp` is the bridge's MCP
    entry. Retries on connect so a compound debug launch can start both sides at
    once.
    """
    line = json.dumps({"args": ["--mcp"], "stdin": request}) + "\n"
    last_error = None
    for _ in range(50):
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.connect(address)
                sock.sendall(line.encode())
                with sock.makefile("r", encoding="utf-8") as stream:
                    reply = json.loads(stream.readline())
            assert "error" not in reply, reply["error"]
            return reply["stdout"]
        except (FileNotFoundError, ConnectionRefusedError) as exc:
            last_error = exc
            time.sleep(0.1)
    raise AssertionError(f"no --serve at {address}: {last_error}")


def rpc(method, params, request_id):
    """One JSON-RPC request: a fresh `--mcp` process, or the debugger's socket."""
    request = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
    address = os.environ.get(SOCKET_ENV)
    if address:
        return json.loads(over_socket(address, request))
    result = subprocess.run(
        [BINARY, "--mcp"],
        input=json.dumps(request) + "\n",
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    (line,) = [line for line in result.stdout.splitlines() if line.strip()]
    return json.loads(line)


def call_tool(request_id, params):
    """One `tools/call`, from parameters the graph keeps so it can retry them."""
    return rpc("tools/call", params, request_id)


def text_of(result):
    """The text a `complete` tools/call carries -- what a client would show."""
    return result["content"][0]["text"]


class Run(MessagesState):
    """Messages plus the elicitation the graph had to answer on the way."""

    first: dict | None  # the first tools/call result
    pending: bool  # whether an answer is still owed
    params: dict | None  # the call being retried
    answer: dict | None  # the retried call's result


def run_graph(planner_messages, responder_message, meta=FORM_CAPABILITY):
    """Compile and run the graph, returning its final state.

    `planner_messages` is what the fake model emits in order; the responder's
    single message is the user's answer, read only once a form is pending.
    """
    planner = GenericFakeChatModel(messages=iter(planner_messages))
    responder = GenericFakeChatModel(messages=iter([responder_message]))

    def agent(state):
        return {"messages": [planner.invoke(state["messages"])]}

    def tools(state):
        if not state.get("pending"):
            call = state["messages"][-1].tool_calls[0]
            params = {"name": call["name"], "arguments": call["args"], "_meta": meta}
            first = call_tool(call["id"], params)["result"]
            return {
                "first": first,
                "pending": first.get("resultType") == "input_required",
                "params": params,
            }

        # The user has answered: retry the same call with the answer attached.
        answer = json.loads(state["messages"][-1].content)
        params = {**state["params"], "inputResponses": {"elicit": answer}}
        result = call_tool("retry", params)["result"]
        return {
            "pending": False,
            "params": params,
            "answer": result,
            "messages": [ToolMessage(text_of(result), tool_call_id="retry")],
        }

    def responder_node(state):
        return {"messages": [responder.invoke(state["messages"])]}

    graph = StateGraph(Run)
    graph.add_node("agent", agent)
    graph.add_node("tools", tools)
    graph.add_node("responder", responder_node)
    graph.add_edge(START, "agent")
    graph.add_conditional_edges(
        "agent",
        lambda state: "tools" if state["messages"][-1].tool_calls else END,
    )
    graph.add_conditional_edges(
        "tools", lambda state: "responder" if state.get("pending") else "agent"
    )
    graph.add_edge("responder", "tools")
    return graph.compile().invoke({"messages": [HumanMessage("clean the index up")]})


def plan_then_finish():
    """What the planner model says: call the tool, then report back."""
    return [
        AIMessage(
            content="",
            tool_calls=[
                {
                    "name": "ask_confirmation",
                    "args": {"question": QUESTION},
                    "id": "call-1",
                }
            ],
        ),
        AIMessage(content="done"),
    ]


def test_the_graph_answers_the_form_and_retries():
    state = run_graph(
        plan_then_finish(),
        AIMessage(
            content=json.dumps(
                {"action": "accept", "content": {"confirm": True, "reason": "stale"}}
            )
        ),
    )

    # The first leg asks rather than finishing, and the form is the reflected
    # struct -- not a hand-written schema.
    assert state["first"]["resultType"] == "input_required"
    request = state["first"]["inputRequests"]["elicit"]
    assert request["method"] == "elicitation/create"
    assert request["params"]["mode"] == "form"
    assert request["params"]["message"] == QUESTION
    assert request["params"]["requestedSchema"] == {
        "type": "object",
        "properties": {"confirm": {"type": "boolean"}, "reason": {"type": "string"}},
        "required": ["confirm", "reason"],
    }

    # The answer reached the server as `inputResponses`, and the retry is a
    # complete result the tool composed itself -- the user's form is an input
    # to the tool, not its output.
    assert state["params"]["inputResponses"] == {
        "elicit": {"action": "accept", "content": {"confirm": True, "reason": "stale"}}
    }
    assert state["answer"]["resultType"] == "complete"
    assert text_of(state["answer"]) == "confirmed: stale"
    assert state["messages"][-1].content == "done"


def test_an_accepted_form_can_still_mean_no():
    """`confirm: false` is an accepted form whose answer is no -- the tool
    decides, so the two are not the same as `decline`."""
    state = run_graph(
        plan_then_finish(),
        AIMessage(
            content=json.dumps(
                {"action": "accept", "content": {"confirm": False, "reason": "keep it"}}
            )
        ),
    )

    assert state["first"]["resultType"] == "input_required"
    assert text_of(state["answer"]) == "the user said no: keep it"


def test_a_declined_form_is_reported_as_such():
    state = run_graph(
        plan_then_finish(),
        AIMessage(content=json.dumps({"action": "decline"})),
    )

    assert state["first"]["resultType"] == "input_required"
    assert text_of(state["answer"]) == "declined"


def test_without_the_capability_the_tool_never_asks():
    state = run_graph(
        plan_then_finish(),
        AIMessage(content=json.dumps({"action": "accept"})),
        meta=NO_CAPABILITY,
    )

    # A complete result on the first leg: nothing to answer, so the responder
    # node is never entered. The tool reports that it could not ask, rather
    # than pretending the user refused.
    assert state["first"]["resultType"] == "complete"
    assert text_of(state["first"]) == (
        "could not ask: the client declared no form capability"
    )
    assert "answer" not in state, "the responder node never ran"
    assert "inputResponses" not in state["params"]
    assert state["messages"][-1].content == "done"
