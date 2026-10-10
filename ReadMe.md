# C++26 static reflection

Minimal notes and sample code for WG21 P2996.

## Requirements

[clang-p2996](https://github.com/bloomberg/clang-p2996/tree/p2996) — experimental Clang for P2996.

## Build

- Build a full `clang-p2996` install and set `LLVM_ROOT` to it: the prefix must contain `bin/clang++`, the reflective `include/c++/v1/meta` header and `lib/libc++.dylib`.
- Execute below command to generate executable.

```sh
cmake -S . -B build -G Ninja  #   Or "cmake.generator": "Ninja" in .vscode/settings.json
cmake --build build
```

## Where reflection could be used

| File | Where it could be used |
|---|---|
| `test_enumeration.cpp` | enum ↔ string, enumerator value/name arrays, bitmask rendering — the same technique the JSON layer uses for its own `enum_from_string` / `enum_to_string` |
| `test_struct.cpp` | Member count / member name / has-member queries; dump+load through yaml-cpp; raw byte serialize/deserialize — all driven by the member list |
| `test_undo_redo.cpp` | Reflection-driven undo/redo: locate a member's index, recognise `Member<T>` holders via template arguments, save/restore each field |
| `test_reflection_widgets.cpp` | Auto-generate ImGui widgets by walking members and base classes, recursing into nested structs |
| `test_forward_chain.cpp` | Compose a forward-chain type at compile time from reflected members |
| `test_json_schema.cpp` | The LLM bridge: JSON Schema from annotations, `from_json` / `to_json`, tool + struct registries, `run_request` and the `--serve` socket server |
| `test_mcp.cpp` | MCP server: tools, prompts and resources derived from markered member functions, driven end to end over a real JSON-RPC loop |

