# Read Linux Memory Management Source

This project is dedicated to studying the Linux kernel v2.4.18 memory management subsystem. The primary goal is to build a clear conceptual map of its architecture and to practice navigating a large C codebase using professional static analysis tools.

## Workspace Structure

The repository is organized to separate the kernel source from your notes and analysis artifacts.

- `linux/`: Contains the unmodified Linux kernel v2.4.18 source code.
- `00-concepts/`: A place for high-level conceptual notes, diagrams, and summaries.
- `01-source-map/`: Intended for source code annotations, cross-references, and generated call maps.
- `02-callflows/`: Stores call flow diagrams and visualizations (e.g., using Mermaid or PlantUML).
- `dockerfile`: Defines the Docker image with all the necessary analysis tools pre-installed.
- `DOCUMENTING.md`: Explains the project's documentation conventions.
- `CONTEXT.md`: Shares context with your AI assistant to ensure it provides relevant help.

## Getting Started

This project uses a Docker container to provide a consistent and pre-configured toolchain.

### 1. Prerequisites

Ensure you have [Docker](https://docs.docker.com/get-docker/) installed and running on your system.

### 2. Build the Docker Image

First, build the Docker image that contains the kernel source and all the analysis tools. This command only needs to be run once.

```bash
# From the project root directory
docker build -t kernel-tools .
```

This command reads the `dockerfile`, downloads a base image, installs tools like `ctags`, `cscope`, and `cflow`, and prepares your environment.

### 3. Launch the Analysis Environment

Once the image is built, you can start an interactive container session. Your project directory will be mounted inside the container at `/workspace`.

```bash
# From the project root directory
docker run -it --rm -v "$(pwd):/workspace" kernel-tools
```

- `docker run`: Starts a new container.
- `-it`: Allocates an interactive terminal.
- `--rm`: Automatically removes the container when you exit, keeping your system clean.
- `-v "$(pwd):/workspace"`: Mounts your current directory (`.`) into the container at `/workspace`. This means any changes you make to files in `/workspace` inside the container will be reflected in your project folder on your host machine.

You are now inside the container, ready to start analyzing the code.

## Core Tasks

Inside the container, you can perform the following tasks:

- **Generate Tags**: Use `ctags` to create a `tags` file for quick jump-to-definition support in editors like Vim or VS Code.
- **Cross-Reference Code**: Use `cscope` to find all references to a symbol, its definition, functions it calls, and functions that call it.
- **Analyze Call Graphs**: Use `cflow` to generate call trees for specific functions, helping you understand complex code paths.
- **Search Code**: Use `ripgrep` (`rg`) for fast, pattern-based searches across the entire kernel source.

## Documentation & Collaboration

Before creating notes or diagrams, please read [DOCUMENTING.md](./DOCUMENTING.md). It explains the project's documentation conventions to ensure consistency.

## Resources

- [Elixir Cross Referencer](https://elixir.bootlin.com/linux/2.4.18/source): A web-based tool for browsing source code with hyperlinks.
- [TLDP Memory Management Guide](https://tldp.org/LDP/tlk/mm/memory.html): A beginner-friendly introduction to paging and memory zones in Linux.
