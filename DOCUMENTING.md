# Documenting Your Exploration

This guide explains the documentation conventions we'll be using for our study of the Linux v2.4.18 memory management subsystem. Following these standards helps us maintain clarity and consistency, which makes the project easier for everyone to follow, especially for those new to the codebase.

## Global Conventions

- **Format**: For consistency, all documentation is written in Markdown.
- **File Naming**: Filenames should be lowercase, using hyphens (`-`) for separators (e.g., `vm-overview.md`). This avoids issues with case-sensitivity across different operating systems.
- **Tag system**: Use a simple tag system to track tasks (e.g., `rg '<tag>'`). Include a short personal identifier to avoid collisions with other contributors, for example `__TODO__(gzh)`.
  - When creating a tag for a task, include a one-line summary or outcome after the identifier. Example: `__TODO__(gzh): introduce tag system`.
  - **__TODO__ & __DONE__**: Prefer the underlined forms (`__TODO__`, `__DONE__`) so they are easier to match and less likely to collide with TODO/DONE markers in the Linux source tree.
- **Metadata Header**: It's helpful for each document to start with a YAML front matter block. This lets us track related topics, tags, and source files.

  ```markdown
  ---
  related:
  - "[Relevant Concept](/path/to/relevant-concept.md)"
  tags:
  - memory-management
  - page-allocation
  sources:
  - "[mm/page_alloc.c](/linux/mm/page_alloc.c)"
  ---

  # Title
  ```
  > **Tip**: There's a helpful script, `/workspace/bin/doc-init.sh`, that generates a new documentation file with this header automatically. You can run it with `bash /workspace/bin/doc-init.sh <filename>`.

## Directory Structure

Our documentation is organized into a few key directories, each with a specific purpose.

### `00-concepts`

This is where we keep high-level explanations of core memory management concepts. Think of these as background reading to provide the theoretical foundation needed before diving into the source code.

**Content Guidelines**:
- **Key Terms**: It's a good idea to cover fundamental terms from your OS course (like "Virtual Memory" or "Paging") as well as kernel-specific mechanisms (e.g., "Zone Allocator," "Buddy System").
- **No Callflows**: To keep things clear, this section is best for concepts, not for documenting function call sequences. Instead, you can link to a concept when you mention it (e.g., `Swapping: The process of moving an inactive [page](./page.md) to disk.`).

**A good structure for a concept file includes**:
1.  **`## In a Word` (Required)**: A concise summary (under 100 words) is required at the top. This is really useful for a quick refresher.
2.  **`## Why This Concept` (Recommended)**: It's also helpful to briefly explain the concept's role in the kernel. This clarifies its purpose and why it's important.
3.  **`## Deep Dive` (Optional)**: For more complex topics, you might want to add a more detailed explanation. After you've drafted the other parts, you could even use an AI assistant to help you write this section.

### `01-source-map`

>[!tip] Each document in this directory should correspond to a single source file. If you're annotating a function, use Doxygen-style comments. For an example, see the `__free_pages_ok()` function in `linux/mm/page_alloc.c`, available in the generated HTML at `linux/docs/html/page__alloc_8c.html` (see [Getting Started](./README.md#getting-started)).

Here, we create annotations and summaries of specific source files. The goal is to build a "map" that makes the dense kernel code easier to navigate. Each document in this directory should correspond to a single source file.

**A helpful structure for a source map file is**:
1.  **`## File Overview`**: What is the purpose of this file? What kernel subsystem does it belong to?
2.  **`## Key Data Structures`**: Describing the main `struct`s, `enum`s, and `typedef`s helps clarify the code. It's useful to explain their fields and why they are important.
3.  **`## Core Functions`**: Listing and explaining the most important functions in the file is key. For each function, a "Reading Recipe" can be very effective:
    - **Purpose**: What does it do?
    - **Inputs/Outputs**: What are its key parameters and return value?
    - **Typical Callers**: Where is this function usually called from?
    - **Related Files**: What other files does it interact with?

### `02-callflows`

This directory is for documenting the step-by-step execution paths for critical operations like memory allocation, page faults, or `mmap()`. These are essential for understanding how different parts of the kernel work together.

**Format**:
To support automated analysis and visualization later on, we've decided that all callflows should be defined in a structured `YAML` format.

**Example (`allocate-page-flow.yml`):**
```yaml
name: "GFP_KERNEL Page Allocation"
description: "Callflow for a standard kernel memory allocation via alloc_pages."
entrypoint: "alloc_pages"
steps:
  - function: "alloc_pages"
    file: "mm/page_alloc.c"
    notes: "High-level entry point for page allocation."
    calls: "alloc_pages_node"
  - function: "alloc_pages_node"
    file: "mm/page_alloc.c"
    notes: "Placeholder for NUMA-aware allocation. On non-NUMA, it's a simple wrapper."
    calls: "__alloc_pages"
  - function: "__alloc_pages"
    file: "mm/page_alloc.c"
    notes: "The core allocation function. Implements the zone-based buddy algorithm."
    calls: "get_page_from_freelist"
  - function: "get_page_from_freelist"
    file: "mm/page_alloc.c"
    notes: "Retrieves a page from the appropriate zone's free list."
```

### `03-notebooks`

This directory serves as a project journal or "lab notebook" for recording daily progress, thoughts, and experimental findings. It's a space for informal, chronological entries that document your learning journey.

**Content Guidelines**:
- **Daily Logs**: You can create a new file for each study session to jot down what you worked on, what you learned, any problems you encountered, and ideas for next steps.
- **Code Snippets & Results**: It's a great place to paste temporary code snippets, command outputs, or interesting observations that aren't ready to be formalized into the other documentation sections.
- **Brainstorming**: You can use these files for free-form thinking and brainstorming before structuring your thoughts into a formal concept or source map.
- **TODO & DONE tags**: Use `__TODO__` and `__DONE__` tags to track outstanding tasks and mark completed items (for example, `rg __TODO__`).

**File Naming Convention**:
- To keep things organized, it's helpful to name files with a prefix, author, and topic. For example: `00-author-topic-keyword.md`.
- While this format is recommended, it's not a strict rule. The main goal is to make the files easy to browse.