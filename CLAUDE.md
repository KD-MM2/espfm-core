<!-- dgc-policy-v11 -->
# Dual-Graph Context Policy

This project uses a local dual-graph MCP server for efficient context retrieval.

## MANDATORY: Adaptive graph_continue rule

**Call `graph_continue` ONLY when you do NOT already know the relevant files.**

### Call `graph_continue` when:
- This is the first message of a new task / conversation
- The task shifts to a completely different area of the codebase
- You need files you haven't read yet in this session

### SKIP `graph_continue` when:
- You already identified the relevant files earlier in this conversation
- You are doing follow-up work on files already read (verify, refactor, test, docs, cleanup, commit)
- The task is pure text (writing a commit message, summarising, explaining)

**If skipping, go directly to `graph_read` on the already-known `file::symbol`.**

## When you DO call graph_continue

1. **If `graph_continue` returns `needs_project=true`**: call `graph_scan` with `pwd`. Do NOT ask the user.

2. **If `graph_continue` returns `skip=true`**: fewer than 5 files  -  read only specifically named files.

3. **Read `recommended_files`** using `graph_read` — **one call per file**.
   - `graph_read` accepts a single `file` parameter (string). Call it separately for each recommended file. Do NOT pass an array.
   - Always use `file::symbol` notation (e.g. `src/auth.ts::handleLogin`)  -  never read whole files.
   - `recommended_files` entries that already contain `::` must be passed verbatim.
   - Example: if `recommended_files` is `["src/auth.ts::handleLogin", "src/db.ts"]`, call `graph_read(file: "src/auth.ts::handleLogin")` and `graph_read(file: "src/db.ts")` as two separate calls (they can be parallel).

4. **Obey confidence caps:**
   - `confidence=high` -> Stop. Do NOT grep or explore further.
   - `confidence=medium` -> `fallback_rg` at most `max_supplementary_greps` times, then `graph_read` at most `max_supplementary_files` more symbols. Stop.
   - `confidence=low` -> same as medium. Stop.

## Session State (compact, update after every turn)

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

Use this state  -  not prose summaries  -  to remember what's been done across turns.

## Token Usage

A `token-counter` MCP is available for tracking live token usage.

- Before reading a large file: `count_tokens({text: "<content>"})` to check cost first.
- To show running session cost: `get_session_stats()`
- To log completed task: `log_usage({input_tokens: N, output_tokens: N, description: "task"})`

Live dashboard URL is printed at startup next to "Token usage".

## Rules

- Do NOT use `rg`, `grep`, or bash file exploration before calling `graph_continue` (when required).
- Do NOT do broad/recursive exploration at any confidence level.
- Do NOT dump full chat history.
- `max_supplementary_greps` and `max_supplementary_files` are hard caps  -  never exceed them.
- Do NOT call `graph_continue` more than once per turn.
- Do NOT call `graph_retrieve` more than once per turn.
- Always use `file::symbol` notation with `graph_read`  -  never bare filenames.
- After edits, call `graph_register_edit` with changed files using `file::symbol` notation.

## Context Store

Whenever you make a decision, identify a task, note a next step, fact, or blocker during a conversation, call `graph_add_memory`.

**To add an entry:**
```
graph_add_memory(type="decision|task|next|fact|blocker", content="one sentence max 15 words", tags=["topic"], files=["relevant/file.ts"])
```

**Do NOT write context-store.json directly** — always use `graph_add_memory`. It applies pruning and keeps the store healthy.

**Rules:**
- Only log things worth remembering across sessions (not every minor detail)
- `content` must be under 15 words
- `files` lists the files this decision/task relates to (can be empty)
- Log immediately when the item arises  -  not at session end

## Session End

When the user signals they are done (e.g. "bye", "done", "wrap up", "end session"), proactively update `CONTEXT.md` in the project root with:
- **Current Task**: one sentence on what was being worked on
- **Key Decisions**: bullet list, max 3 items
- **Next Steps**: bullet list, max 3 items

Keep `CONTEXT.md` under 20 lines total. Do NOT summarize the full conversation  -  only what's needed to resume next session.


## ESP-IDF Build Environment

Before running any `idf.py` command, activate the ESP-IDF environment:

```powershell
& 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
idf.py build
idf.py flash          # skip if no device connected
idf.py monitor
```

All build/flash commands MUST use PowerShell tool. Never use Bash tool for ESP-IDF operations.

## Obsidian Vault — ESP-IDF Knowledge Base

This project uses an Obsidian vault connected via MCP. The vault stores ESP-IDF documentation and an LLM-maintained wiki.

### Vault Zone Structure

| Zone | Path | Rule |
|------|------|------|
| **Raw sources** | `raw/` | READ-ONLY. Never edit, rename, or move. Only read, cite, reference via wikilinks. |
| **LLM wiki** | `wiki/` | LLM-maintained. Create, edit, refactor freely. Every page MUST have frontmatter: `title`, `type`, `tags`, `sources`. Every page MUST have ≥1 wikilink. |
| **Dev notes** | `dev/` | Collaborative. Technical notes, ADRs, debriefs, snippets. |

### raw/ — ESP-IDF Official Docs

Imported ESP-IDF documentation, one subfolder per chip variant:

| Path | Contents |
|------|----------|
| `raw/esp32/` | ESP-IDF docs for ESP32 |
| `raw/esp32s3/` | ESP-IDF docs for ESP32-S3 |
| `raw/esp32c6/` | ESP-IDF docs for ESP32-C6 |

Each variant folder contains: `api-reference/`, `api-guides/`, `hw-reference/`, `get-started/`, `migration-guides/`, `security/`, `libraries-and-frameworks/`, etc.

### ESP-IDF Docs Lookup via Obsidian MCP

**Search ESP-IDF docs in the vault:**

```
# Full-text search across raw/ (fast)
mcp__obsidian__search_simple --query "LEDC PWM timer configuration"

# JsonLogic query (structured, precise)
mcp__obsidian__search_query --query {"glob": ["raw/esp32s3/api-reference/**", {"var": "path"}]}

# Read a specific doc page
mcp__obsidian__vault_read --path "raw/esp32s3/api-reference/peripherals/ledc.md"
```

**Key Obsidian MCP tools:**
- `mcp__obsidian__search_simple` — full-text search with relevance scoring
- `mcp__obsidian__search_query` — JsonLogic queries against frontmatter + path
- `mcp__obsidian__vault_read` — read any file with frontmatter, links, backlinks
- `mcp__obsidian__vault_list` — list directory contents
- `mcp__obsidian__vault_write` — create/overwrite files (wiki/ and dev/ only)
- `mcp__obsidian__vault_patch` — surgical edits to headings/frontmatter/blocks
- `mcp__obsidian__vault_append` — append to file
- `mcp__obsidian__vault_get_document_map` — heading structure + frontmatter keys

Use Obsidian MCP when `run-example-search` catalog doesn't cover the specific API question.

### wiki/ — LLM-Maintained Knowledge Base

The wiki follows the **LLM-WIKI pattern**: an incrementally built, structured, interlinked collection of markdown files maintained by Claude. Key principles:

- **Persistent, compounding artifact** — knowledge compiled once, kept current, not re-derived per query
- **Three-layer architecture**: raw sources (immutable) → wiki (LLM-owned) → schema (this CLAUDE.md)
- **Index-driven navigation**: `wiki/index.md` catalogs all pages; read index first, then drill into pages
- **Chronological log**: `wiki/log.md` records all ingests/queries/lints with timestamps

#### Operations

**Ingest** (when I say "process X" or "ingest X"):
1. Read source file(s) from `raw/`.
2. Discuss key takeaways with me.
3. Create/update wiki entity pages for each concept found.
4. Cross-link new pages to existing relevant pages.
5. Flag contradictions or tensions with existing wiki content.
6. Create a summary page bridging between the clipping and the concepts.
7. Update `wiki/index.md` if something is genuinely new.
8. Report what was done — concepts created/updated, links added.

**Query**: Search wiki pages, read relevant ones, synthesize answer with citations. Good answers can be filed back into wiki as new pages.

**Lint**: Periodically check wiki health — contradictions, stale claims, orphan pages, missing cross-references, data gaps.

#### Strict Limits

- NEVER delete files without explicit confirmation.
- NEVER run `git add`, `git commit`, or `git push`. I handle all version control manually.
- You may SUGGEST a commit message when a logical unit of work is done, but don't execute it.
- NEVER edit CLAUDE.md itself (ask me).
- If an operation affects more than 5 files, SHOW the plan before executing.
- If unsure which zone a file belongs to, ASK.

## Mandatory Feature/Fix Cycle

Every feature and fix MUST follow this cycle — no shortcuts:

1. **Understand** — `/embedded-systems` to analyze the feature/bug and define what the change should look like at the hardware/peripheral level
2. **Lookup** — `/run-example-search` to find standard ESP-IDF patterns (never reinvent)
3. **Explore** — `/opsx:explore` to investigate APIs, design decisions, pin constraints, and trade-offs before committing to a proposal
4. **Propose** — `/opsx:propose` with the change, referencing explored findings and catalog examples
5. **Apply** — `/opsx:apply` to implement tasks
6. **Build** — `idf.py build` to verify compilation
7. **Flash** — `idf.py flash` to test on device (skip if no device found)
8. **Archive** — `/opsx:archive` after build passes
9. **Commit** — `git add` + `git commit` with conventional commit format

## OpenSpec Workflow

This project uses **OpenSpec** for spec-driven development. Changes are proposed, reviewed, implemented, and archived through the `openspec/` directory.

### Slash commands

| Command | Purpose |
|---------|---------|
| `/opsx:explore` | Explore existing specs, changes, and their status |
| `/opsx:propose` | Propose a new change with spec, design, and tasks |
| `/opsx:apply` | Apply (implement) the tasks defined in an active change |
| `/opsx:archive` | Archive a completed change after implementation is verified |

### Directory layout

```
openspec/
├── config.yaml              # OpenSpec project configuration
├── changes/                 # Active (in-progress) change specs
│   ├── <change-name>/
│   │   ├── .openspec.yaml   # Change metadata (schema, created date)
│   │   ├── proposal.md      # Why this change, what it does
│   │   ├── design.md        # Technical decisions, trade-offs, risks
│   │   ├── tasks.md         # Implementation checklist
│   │   └── specs/           # Per-capability requirement specs
│   └── ...
└── changes/archive/         # Completed and archived changes
    └── YYYY-MM-DD-<name>/
```

### Workflow

1. **Propose** — `/opsx:propose` creates a new change with `proposal.md`, `design.md`, `tasks.md`, and capability specs under `specs/`. Use this for any non-trivial feature, module, or refactor.
2. **Review** — Review the generated spec and design docs. Edit to match the actual intended behavior before implementing.
3. **Apply** — `/opsx:apply` implements the tasks. Task items in `tasks.md` are marked `[x]` as completed.
4. **Archive** — `/opsx:archive` moves the completed change to `archive/` with a date prefix.
5. **Update IMPLEMENTATION.md** — After archive, update the implemented feature in `IMPLEMENTATION.md`:
   - Mark the task checkbox: `- [ ]` → `- [x]`
   - Update the `Progress Tracking` table counts at the bottom of the file
   - Do **NOT** change the feature description text, do **NOT** add or remove lines, do **NOT** modify anything else in the file. Only the checkbox and the two numbers in the progress table row.
6. **Commit** — Commit all changes with `git add` and `git commit`. Use conventional commit format: `feat(component): description`. Do NOT commit `.claude/settings.local.json` or `.clang-format`.

Before creating a new change, always use `/opsx:explore` to check for existing specs that may overlap or inform the work.

For full OpenSpec conventions, see `AGENTS.md` (if present) or the `.claude/commands/opsx/` directory.

## Project Skills

When working on this project, invoke the relevant skill for the task at hand:

| Skill | When to Use |
|-------|-------------|
| `cpp-pro` | **Essential.** All C/C++ source code — writing headers, implementing functions, refactoring, code review. |
| `embedded-systems` | Embedded-specific tasks — peripheral drivers (LEDC, PCNT, ADC, GPIO), ISR patterns, FreeRTOS tasks, hardware constraints. |
| `run-example-search` | **Essential.** Look up ESP-IDF example catalog before implementing — prevents reinventing standard patterns. |
| `build` | Compiling firmware (`idf.py build`), flashing (`idf.py flash`), serial monitoring, filesystem uploads. |
