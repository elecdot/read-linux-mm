# AGENTS Guide

This guide defines how AI assistants (Copilot Chat, ChatGPT, etc.) and human contributors should collaborate while exploring the Linux kernel v2.4.18 memory management subsystem in this repository. It supplements `README.md`, `COLLABORATION.md`, and `DOCUMENTING.md` by translating their conventions into actionable rules for agent behavior.

## Core Project Context

Goal: Build a clear conceptual and navigational map of the Linux 2.4.18 memory management subsystem and practice large‑scale C source exploration using static analysis tools (Doxygen, ctags, cscope, cflow, ripgrep).

Key Areas (from workspace structure):
- `linux/` – Upstream kernel source snapshot (keep semantics unchanged; comments may be augmented minimally with Doxygen when appropriate).
- `00-concepts/` – High‑level theory & conceptual explanations.
- `01-source-map/` – (Currently discouraged; see warning in `DOCUMENTING.md`.) Intended for per‑file structural summaries.
- `02-callflows/` – YAML callflow definitions for important execution paths.
- `03-notebooks/` – Chronological lab notebooks / daily logs with `__TODO__` and `__DONE__` tags.
- `bin/` – Helper scripts (`doc-init.sh`, docker init scripts).
- `dockerfile` – Containerized analysis environment definition.

Agents must not invent architecture: rely strictly on observed folder structure and documented goals.

## Canonical Knowledge Sources

| Source | Role | Must Consult Before | Never Contradict |
| ------ | ---- | ------------------- | ---------------- |
| `README.md` | Project purpose, environment setup, toolchain overview | Suggesting environment/tool changes; onboarding guidance | Project goal & stated workspace structure |
| `COLLABORATION.md` | Branching patterns, conventional commits, daily workflow | Creating branches, crafting commits, advising PR flow | Branch name patterns; commit message style; small coherent scope rule |
| `DOCUMENTING.md` | Documentation formats, Doxygen usage, directory purposes, tagging | Adding docs, editing comments, creating concept/callflow/notebook files | File naming (lowercase-hyphen), tag conventions (`__TODO__`, `__DONE__`), YAML callflow structure |
| Kernel source (`linux/`) | Ground truth for technical behavior | Explaining function semantics or proposing annotations | Actual control flow & data structures |
| Generated Doxygen (`linux/docs/html/`) | Rendered reference & call graphs | Verifying cross references or documenting functions | In-source symbol relationships |

Agents must re-open (re-read) relevant sources when:
- Starting a new conceptual document.
- Modifying or adding Doxygen comments in kernel files.
- Preparing a PR that spans multiple directories.
- Providing non-trivial analysis of a function or callflow.

If any ambiguity arises (e.g., conflicting interpretations), agents should surface the conflict and request clarification instead of assuming.

## Development Workflow for Agents

When generating code or documentation changes:
- Preserve repository structure; do not reorganize directories unless explicitly instructed.
- Avoid altering kernel logic inside `linux/` (limit changes to non-invasive Doxygen comments or readability whitespace if approved).
- Follow branch patterns from `COLLABORATION.md` exactly.
- Keep commits small and coherent; group related doc changes together, but split unrelated conceptual additions.

Branching:
- Create topic branches from `main` using patterns: `feat/<topic>`, `fix/<topic>`, `docs/<topic>`, `refactor/<topic>`, `test/<topic>`, `chore/<topic>` optionally suffixed with issue numbers (e.g. `feat/parse-mm-docs-12`).
- Never commit directly to `main`—always via PR.

Commit Style (Conventional Commits):
- Format: `<type>(scope?): <concise summary>`; chain logically related doc notes with semicolons only if same topic.
- Types: `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `chore`, `build`, `ci`.
- Keep tense imperative and summary under ~72 chars.

Splitting vs. Squashing:
- Split: Distinct conceptual additions (e.g., new callflow + unrelated concept note).
- Squash (via PR squash merge): Review convenience; agents should not rewrite commit history locally unless asked.

Coding / Annotation Style:
- Doxygen comments: Use block form `/** ... */` for function-level summaries; trailing `//!` only for brief inline clarifications.
- Maintain lowercase-hyphen filenames for Markdown artifacts.
- Use YAML structured callflows—no free-form diagrams in `02-callflows/`.
- Use tag format `__TODO__(id): description` and `__DONE__(id): description` consistently in notebooks.

Tool Assistance:
- Suggest ctags/cscope/cflow commands without altering configuration unless user requests.
- For environment setups, mirror instructions from `README.md` exactly.

## Documentation Workflow for Agents

You must document:
- New conceptual topics (place in `00-concepts/`).
- New structured callflows (place YAML in `02-callflows/`).
- Significant exploration sessions (append/create notebook in `03-notebooks/`).
- Added or clarified kernel function semantics via Doxygen (inline in source or summarized externally if invasive changes are discouraged).

Placement Rules:
- High-level theory: `00-concepts/`.
- Per-function execution path: `02-callflows/` YAML.
- Daily log / ephemeral thoughts: `03-notebooks/`.
- Source annotation (if permitted): Doxygen in kernel file OR conceptual mapping outside (avoid altering logic).

Formats:
- Markdown only for conceptual/notebook files.
- YAML schema for callflows: keys = `name`, `description`, `entrypoint`, `steps` (each step: `function`, `file`, `notes`, optional `calls`).
- Doxygen tags: `@brief`, `@param`, `@return`, `@note`, `@see`, `@warning`.

Agent Rules:
- Before adding a concept file, verify uniqueness—search similar topics in `00-concepts/`.
- If introducing new callflow, confirm entry function exists in `linux/` tree.
- For large new concept proposals, optionally scaffold file with YAML front matter per `DOCUMENTING.md` guidelines.
- Prefer incremental doc stubs over speculative deep dives; mark incomplete sections with `__TODO__`.

Documentation Checklist for Agents:
- Confirm target directory matches artifact type.
- Use lowercase-hyphen filename.
- Add YAML front matter if concept or source-map style.
- Include `## In a Word` (concept) or `## File Overview` (source-map).
- Validate links to source paths (relative, existing).
- Add Doxygen tags only where they clarify behavior.
- Tag outstanding tasks with `__TODO__(id)`.

## Testing & Quality Expectations

Current repository focus is exploratory—no dedicated test suite folder detected. Quality gates are procedural:
- Validate Doxygen builds (`cd linux && doxygen Doxyfile`).
- Ensure Markdown lint neutrality (agents should self-check headings, lists, code fences).
- If introducing parsing/analysis scripts (future), agents must propose minimal tests (e.g., shell-based validation).
- Run static navigation tools (ctags, cscope) after major structural doc additions to ensure references remain coherent.

Agents Must:
- Suggest verification commands rather than claiming success blindly.
- Avoid adding test frameworks without explicit human approval.

Suggested Commands:
```bash
docker build -t kernel-tools .
docker run -it --rm -v "$(pwd):/workspace" --user $(id -u):$(id -g) kernel-tools
cd linux && doxygen Doxyfile
rg __TODO__
```

## Git & Pull Request Practices with AI

PR Creation:
- Open PR from topic branch to `main` after pushing.
- Scope: one coherent topic (e.g., new concept doc OR new callflow, not both unless tightly coupled).

AI Support on PRs:
- Title: Derive from primary Conventional Commit, e.g. `docs(concepts): add vm-overview`.
- Description Template:
  - Summary
  - Directories touched
  - Verification steps (commands run)
  - Outstanding `__TODO__` items (if any)
- Reviewer Checklist (agent may draft): filenames follow convention; no kernel semantic changes; Doxygen builds; no unrelated mixed concerns.

Do NOT Bundle:
- Refactors + new concepts.
- Multiple unrelated callflows.
- Kernel comment changes + large notebook narrative rewrite.

Merge Conflicts:
- Describe conflicting files; suggest standard commands:
```bash
git fetch origin
git rebase origin/main
# resolve, then
git add <files>
git rebase --continue
```
- Never fabricate historical commits—report ambiguity if conflict source unclear.

Decision Guide:
- Small focused change: single concept, single callflow, incremental Doxygen comment.
- Large refactor: only on explicit human request with rationale.
- Ask for clarification: when required directory is unclear or overlapping doc exists.

## Safe Usage Patterns for Agents

✅ Do:
- Re-read canonical docs before multi-file edits.
- Preserve existing comments, license headers, and upstream kernel semantics.
- Use documented branch & commit conventions.
- Keep PRs minimal and well-described.
- Surface uncertainties instead of guessing.

❌ Don’t:
- Introduce new major tools/frameworks casually.
- Reorganize directory hierarchy without instruction.
- Bulk modify kernel functional code.
- Mix unrelated documentation domains in one PR.
- Remove existing tags or attribution in source files.

Clarification Protocol:
- If a requested change conflicts with documented rules, ask the user to confirm override.
- Highlight duplicate concept risk before adding near-identical files.

## Quickstart for Humans Using AI

Request Pattern:
"You are an assistant working in this repo. Task: <clear objective>. Constraints: follow branch naming, doc conventions, and this AGENTS guide. Output: patch + verification steps."

Good Requests:
- "Add a concept doc for the buddy allocator; follow existing concept structure." 
- "Create a YAML callflow for GFP_KERNEL allocation in `02-callflows/`."
- "Add minimal Doxygen comment for `__free_pages_ok` without changing logic."

Poor Requests:
- "Rewrite memory management subsystem." (Too broad)
- "Add tests" (No context on target or behavior.)
- "Improve everything in docs" (Non-specific)

Human Reminders:
- Always review diffs locally.
- Run Doxygen after doc/comment changes.
- Confirm branch & commit format before pushing.
- Maintain small scope; request clarification if large change proposed.

## Checklist for New Agents

- [ ] Read `README.md`, `COLLABORATION.md`, `DOCUMENTING.md`.
- [ ] Understand project goal (exploratory mapping, not feature development).
- [ ] Follow branch naming & Conventional Commits.
- [ ] Use correct directories: concepts / callflows / notebooks.
- [ ] Apply Doxygen style only where it clarifies.
- [ ] Keep kernel semantics untouched.
- [ ] Update or create docs when adding insights.
- [ ] Provide verification commands with changes.
- [ ] Keep PRs small & single-topic.
- [ ] Ask for clarification on ambiguity instead of assuming.

---
If this guide appears outdated relative to other docs, agents should flag the discrepancy and request a sync update rather than proceeding with conflicting instructions.