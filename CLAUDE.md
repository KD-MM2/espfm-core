# espfm-core

ESP-IDF v6.0.1 fan controller with CoAP+Protobuf remote API. Targets ESP32/ESP32-S3.

<!-- dgc-policy-v11 -->

## Dual-Graph Context Policy

This project uses a local dual-graph MCP server for efficient context retrieval.

### MANDATORY: Adaptive graph_continue rule

**Call `graph_continue` ONLY when you do NOT already know the relevant files.**

#### Call `graph_continue` when:

- This is the first message of a new task / conversation
- The task shifts to a completely different area of the codebase
- You need files you haven't read yet in this session

#### SKIP `graph_continue` when:

- You already identified the relevant files earlier in this conversation
- You are doing follow-up work on files already read (verify, refactor, test, docs, cleanup, commit)
- The task is pure text (writing a commit message, summarising, explaining)

**If skipping, go directly to `graph_read` on the already-known `file::symbol`.**

### When you DO call graph_continue

1. **If `graph_continue` returns `needs_project=true`**: call `graph_scan` with `pwd`. Do NOT ask the user.

2. **If `graph_continue` returns `skip=true`**: fewer than 5 files - read only specifically named files.

3. **Read `recommended_files`** using `graph_read`.
   - Always use `file::symbol` notation (e.g. `src/auth.ts::handleLogin`) - never read whole files.
   - `recommended_files` entries that already contain `::` must be passed verbatim.

4. **Obey confidence caps:**
   - `confidence=high` -> Stop. Do NOT grep or explore further.
   - `confidence=medium` -> `fallback_rg` at most `max_supplementary_greps` times, then `graph_read` at most `max_supplementary_files` more symbols. Stop.
   - `confidence=low` -> same as medium. Stop.

### Session State (compact, update after every turn)

Maintain a short JSON block in your working memory. Update it after each turn:

```json
{
  "files_identified": ["path/to/file.py"],
  "symbols_changed": ["module::function"],
  "fix_applied": true,
  "features_added": ["description"],
  "open_issues": ["one-line note"]
}
```

Use this state - not prose summaries - to remember what's been done across turns.

### Token Usage

A `token-counter` MCP is available for tracking live token usage.

- Before reading a large file: `count_tokens({text: "<content>"})` to check cost first.
- To show running session cost: `get_session_stats()`
- To log completed task: `log_usage({input_tokens: N, output_tokens: N, description: "task"})`

### Rules

- Do NOT use `rg`, `grep`, or bash file exploration before calling `graph_continue` (when required).
- Do NOT do broad/recursive exploration at any confidence level.
- `max_supplementary_greps` and `max_supplementary_files` are hard caps - never exceed them.
- Do NOT call `graph_continue` more than once per turn.
- Always use `file::symbol` notation with `graph_read` - never bare filenames.
- After edits, call `graph_register_edit` with changed files using `file::symbol` notation.
- **MANDATORY:** Run clang-format on all `.c`/`.h` files (excluding `espfm.pb.*` and `nanopb/`) before EVERY commit.

### Context Store

Whenever you make a decision, identify a task, note a next step, fact, or blocker during a conversation, append it to `.dual-graph/context-store.json`.

**Entry format:**

```json
{
  "type": "decision|task|next|fact|blocker",
  "content": "one sentence max 15 words",
  "tags": ["topic"],
  "files": ["relevant/file.ts"],
  "date": "YYYY-MM-DD"
}
```

**To append:** Read the file -> add the new entry to the array -> Write it back -> call `graph_register_edit` on `.dual-graph/context-store.json`.

**Rules:**

- Only log things worth remembering across sessions (not every minor detail)
- `content` must be under 15 words
- `files` lists the files this decision/task relates to (can be empty)
- Log immediately when the item arises - not at session end

### Session End

When the user signals they are done (e.g. "bye", "done", "wrap up", "end session"), proactively update `CONTEXT.md` in the project root with:

- **Current Task**: one sentence on what was being worked on
- **Key Decisions**: bullet list, max 3 items
- **Next Steps**: bullet list, max 3 items

Keep `CONTEXT.md` under 20 lines total. Do NOT summarize the full conversation - only what's needed to resume next session.

## ESP-IDF Build Environment

Before running any `idf.py` command, activate the ESP-IDF environment:

```powershell
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py build
idf.py flash          # skip if no device connected
idf.py monitor
```

All build/flash commands MUST use PowerShell tool. Never use Bash tool for ESP-IDF operations.

## Code Formatting

**MANDATORY:** Run `clang-format` on ALL `.c` and `.h` files before EVERY commit. No exceptions.

```powershell
Get-ChildItem -Path components, main -Recurse -Include *.c, *.h |
    Where-Object {
        $_.FullName -notmatch 'f_schema\\espfm\.pb\.(c|h)' -and
        $_.FullName -notmatch 'components\\nanopb'
    } |
    ForEach-Object { clang-format -i $_.FullName }
```

**Exclusions (auto-generated / vendored — never format):**

- `components/f_schema/espfm.pb.c` and `espfm.pb.h` (nanopb auto-generated)
- `components/nanopb/` (vendored library)

**Config:** `.clang-format` at project root (LLVM base, 4-space indent, K&R braces, Allman function braces).

## Architecture

### Stack

| Layer         | Technology                              | Notes                                                 |
| ------------- | --------------------------------------- | ----------------------------------------------------- |
| Transport     | **libcoap-4** (`espressif/coap ^4.3.5`) | UDP CoAP server, single `coap_task` thread            |
| Serialization | **nanopb-0.4.9.1**                      | Proto at `components/f_schema/proto/espfm.proto`      |
| Storage       | **NVS + LittleFS**                      | `f_config` persists fan/source/curve/schedule configs |
| WiFi          | `f_wifi` + captive portal provisioning  | `f_provision` for initial WiFi setup                  |
| mDNS          | `f_mdns`                                | Advertises `_coap._udp` and `_http._http` services    |

### Component Map

| Component       | Responsibility                                                 |
| --------------- | -------------------------------------------------------------- |
| `f_core`        | Boot orchestration, event bus                                  |
| `f_coap`        | CoAP server lifecycle + all route handlers (`f_coap_routes.c`) |
| `f_schema`      | Protobuf schema (`espfm.proto`) + generated nanopb code        |
| `f_fan`         | Fan control (LEDC PWM + PCNT tach), slot registry (max 8)      |
| `f_source`      | Temperature sources (ADC, DS18B20 ROM-code identity), slot registry (max 8) |
| `f_curve`       | Fan curves (temp→duty lookup tables), slot registry (max 8)    |
| `f_schedule`    | Time-based fan scheduling, slot registry (max 8)               |
| `f_control`     | Control loop: reads sources, evaluates curves, sets fan duty   |
| `f_constraints` | Safety limits (min/max duty, critical temps)                   |
| `f_config`      | NVS+LittleFS persistent config save/load                       |
| `f_wifi`        | WiFi STA+AP management, reconnect logic                        |
| `f_provision`   | WiFi provisioning captive portal                               |
| `f_mdns`        | mDNS service advertisement                                     |
| `f_ledc`        | LEDC PWM driver abstraction                                    |
| `f_pcnt`        | Pulse counter (fan tachometer)                                 |
| `f_adc`         | ADC driver with ESP-IDF calibration (curve/line fitting)       |
| `f_ds18b20`     | 1-Wire temperature sensor driver (ROM-code identity, batch conversion) |
| `f_gpio`        | GPIO pin registry (compile-time ESP32/S3 reserved pin tables)  |

### Key Files

| File                                    | Purpose                                                                           |
| --------------------------------------- | --------------------------------------------------------------------------------- |
| `main/main.c`                           | Boot sequence: NVS → GPIO → drivers → registries → config → control → WiFi → CoAP |
| `components/f_coap/f_coap.c`            | CoAP server lifecycle (start/stop/restart), `coap_task`                           |
| `components/f_coap/f_coap_routes.c`     | All 22+ CoAP route handlers                                                       |
| `components/f_coap/f_coap_internal.h`   | `struct f_coap` definition, internal declarations                                 |
| `components/f_schema/proto/espfm.proto` | Protobuf schema (all request/response messages)                                   |
| `components/f_schema/espfm.pb.h`        | Generated nanopb header (do not edit manually)                                    |
| `tools/espfm_shell.py`                  | Interactive CoAP shell client                                                     |
| `tools/gen_proto.ps1`                   | Nanopb code generation script                                                     |

### CoAP Endpoint Catalog

| Path               | Methods           | Handler                              |
| ------------------ | ----------------- | ------------------------------------ |
| `fans`             | GET, POST         | List fans, create fan                |
| `fans/{0..7}`      | GET, PUT, DELETE  | Get/update/delete fan by ID          |
| `sources`          | GET, POST         | List sources, create source          |
| `sources/{0..7}`   | GET, POST, DELETE | Get/update/delete source by ID       |
| `sources/temp`     | POST              | Manual temperature update            |
| `curves`           | GET, POST         | List curves, create curve            |
| `curves/{0..7}`    | GET, PUT, DELETE  | Get/update/delete curve by ID        |
| `schedules`        | GET, POST         | List schedules, create schedule      |
| `schedules/{0..7}` | GET, PUT, DELETE  | Get/update/delete schedule by ID     |
| `system/info`      | GET               | Version, uptime, heap, entity counts |
| `system/hostname`  | PUT               | Set device hostname                  |
| `system/reboot`    | POST              | Reboot device (2s delay)             |
| `ds18b20/scan`     | GET               | Scan 1-Wire bus, list ROM codes+temps |
| `ds18b20/config`   | PUT               | Set DS18B20 bus GPIO (runtime)       |
| `wifi/scan`        | GET               | Scan for APs                         |
| `wifi/status`      | GET               | Current WiFi connection status       |
| `wifi/connect`     | POST              | Connect to AP                        |

### Adding a New CoAP Endpoint

1. Add protobuf messages to `components/f_schema/proto/espfm.proto` if needed
2. Regenerate nanopb (see Protobuf Workflow below)
3. Add handler function in `components/f_coap/f_coap_routes.c` following existing pattern
4. Register resource in `f_coap_register_resources()` using `add_resource(ctx, h, "path", get, post, put, del)`
5. Build: `idf.py build`
6. Add shell command in `tools/espfm_shell.py` if user-facing

## Protobuf Workflow

Proto file: `components/f_schema/proto/espfm.proto`
Generated files: `components/f_schema/espfm.pb.h`, `components/f_schema/espfm.pb.c`

To regenerate after editing the proto:

```powershell
# Activate ESP-IDF (provides nanopb generator)
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'

# Run generator
python C:/Espressif/tools/python/v6.0.1/venv/Lib/site-packages/nanopb/generator/nanopb_generator.py `
  -I components/f_schema/proto `
  -I components/f_schema `
  -D components/f_schema/ `
  components/f_schema/proto/espfm.proto
```

Or use the shortcut script: `tools/gen_proto.ps1`

Commit the proto file AND generated files together:

```bash
git add components/f_schema/proto/espfm.proto components/f_schema/espfm.pb.h components/f_schema/espfm.pb.c
git commit -m "feat(proto): description"
```

## Shell Tool

Interactive CoAP client at `tools/espfm_shell.py`.

```bash
python tools/espfm_shell.py                           # connect to default (mDNS scan)
python tools/espfm_shell.py --host 192.168.0.22       # connect to specific IP
```

Dependencies: `pip install protobuf rich prompt_toolkit zeroconf`

Key commands: `connect`, `fans list/get/create/update/enable/disable`, `sources list/create/temp` (`--rom` for DS18B20), `curves list/create/update`, `schedules list/create/update`, `ds18b20 scan/config`, `system info/reboot`, `wifi scan/status/connect`, `dashboard`, `export/import`.

Generated protobuf bindings: `tools/espfm_pb2.py` (regenerate with `tools/gen_proto.ps1`)

## Project Skills

When working on this project, invoke the relevant skill for the task at hand. Skills are defined in `.claude/ultracode/INVENTORY.md` and `.claude/skills/`.

| Skill                | When to Use                                                                                                           |
| -------------------- | --------------------------------------------------------------------------------------------------------------------- |
| `convention`         | **Always.** Auto-load for any code edit.                                                                              |
| `coap-route-handler` | Creating or modifying CoAP route handlers in `components/f_coap/`.                                                    |
| `esp-idf-component`  | Creating or modifying an ESP-IDF component directory, CMakeLists.txt, or component source/header under `components/`. |
| `service-module`     | Creating or modifying an f\_\* component that holds state and exposes a handle-based API.                             |
| `registry-pattern`   | Creating or modifying a slot-based registry component with fixed MAX_COUNT arrays.                                    |
| `header-interface`   | Creating or modifying a public component header (`include/*.h`).                                                      |
| `freertos-task`      | Adding or modifying a FreeRTOS task lifecycle (start/stop) inside an f\_\* component.                                 |
| `module-hub`         | Locating which area/module a path belongs to.                                                                         |
| `cpp-pro`            | Writing, optimizing, or debugging C/C++ code with modern C++20/23 features, templates, CMake.                         |
| `embedded-systems`   | Developing firmware for microcontrollers, RTOS, peripheral drivers, interrupt handlers, power optimization.           |
| `build`              | Build, upload, monitor, or clean the ESP-IDF project.                                                                 |
| `gpio-config`        | GPIO pin configuration, wiring, pin mapping, pin conflicts on ESP32.                                                  |

### Skill Application Mapping

| File type being changed                                                 | Skills to load                              |
| ----------------------------------------------------------------------- | ------------------------------------------- |
| `components/f_*/CMakeLists.txt`                                         | `esp-idf-component`, `convention`           |
| `components/f_*/*.c`                                                    | `service-module`, `convention`              |
| `components/f_*/include/*.h`                                            | `header-interface`, `convention`            |
| `components/f_coap/f_coap_routes.c`                                     | `coap-route-handler`, `convention`          |
| `components/f_fan/*.c`, `f_source/*.c`, `f_curve/*.c`, `f_schedule/*.c` | `registry-pattern`, `convention`            |
| `components/f_*/f_*.c` (any with background task)                       | `freertos-task`, `convention`               |
| `main/*.c`                                                              | `embedded-systems`, `convention`            |
| `*.h` (any header)                                                      | `cpp-pro`, `header-interface`, `convention` |

## Feature Development Workflow

Two-phase workflow for non-trivial features:

1. **Spec & Plan** — `/superpowers:brainstorming` to explore, design, and write specs + plans
   - Outputs: `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md` and `docs/superpowers/plans/YYYY-MM-DD-<topic>.md`
   - Specs/plans are project-local scratch (gitignored), not committed

2. **Implement** — `/ultracode:orchestrate` with the spec and plan to execute
   - Spawns implement, code-review, and test subagents per plan phase
   - Handles build verification, code review loops, and git commits

Typical flow:

```
/superpowers:brainstorming "add feature X"
  → user approves design → user approves plan
/ultracode:orchestrate implement plan at docs/superpowers/plans/YYYY-MM-DD-feature-x.md
```
