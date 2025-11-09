#!/bin/bash

# doc-init.sh: A script to initialize documentation files for the read-linux-mm project.

# --- Helper Functions ---

# Function to print a log message
# Color definitions (only when output is a terminal)
if [ -t 1 ]; then
    GREEN='\033[0;32m'
    RED='\033[0;31m'
    RESET='\033[0m'
else
    GREEN=''
    RED=''
    RESET=''
fi

# Function to print a log message (colored when appropriate)
log() {
    echo -e "${GREEN}[LOG]${RESET} $1"
}

# Function to print an error message and exit (colored when appropriate)
error() {
    echo -e "${RED}[ERROR]${RESET} $1" >&2
    exit 1
}

# --- Main Script ---

# 1. Prompt for document type
echo "Select the type of document to create:"
echo "  [0] 00-concepts: For a high-level concept."
echo "  [1] 01-source-map: For annotating a source file."
echo "  [2] 02-callflows: For a YAML-based callflow."
echo "  [3] 03-notebooks: For a daily progress log."
read -p "Enter a number (0-3): " doc_type

# 2. Set directory and conventions based on type
case $doc_type in
    0)
        DIR="00-concepts"
        CONVENTION="Use lowercase and hyphens (e.g., 'buddy-allocator.md')."
        ;;
    1)
        DIR="01-source-map"
        CONVENTION="Use lowercase and hyphens (e.g., 'page-alloc-c.md')."
        ;;
    2)
        DIR="02-callflows"
        CONVENTION="Use lowercase and hyphens, with a .yml extension (e.g., 'mmap-flow.yml')."
        ;;
    3)
        DIR="03-notebooks"
        CONVENTION="Use a 'count-author-topic-keyword.md' format (e.g., '01-elecdot-initial-setup.md')."
        ;;
    *)
        error "Invalid selection. Please run the script again."
        ;;
esac

# 3. Prompt for filename
log "Selected type: $DIR"
echo
echo "Naming convention: $CONVENTION"
echo "NOTE: You should enter the whole filename including extension."
echo
read -p "Enter the filename: " filename

# Check if filename is provided
if [ -z "$filename" ]; then
    error "Filename cannot be empty."
fi

FILE_PATH="$DIR/$filename"

# Check if file already exists
if [ -f "$FILE_PATH" ]; then
    error "File '$FILE_PATH' already exists."
fi

# 4. Create the file with the appropriate template
log "Creating file at '$FILE_PATH'..."

case $doc_type in
    0) # 00-concepts
        cat > "$FILE_PATH" << EOF
---
related:
- "[Relevant Concept](/path/to/relevant-concept.md)"
tags:
- concept
sources:
- "[path/to/source.c](/path/to/source.c)"
- "[Another Source](http://link.to/source)"
---

# Title

## In a Word



## Why This Concept



## Deep Dive


EOF
        ;;
    1) # 01-source-map
        cat > "$FILE_PATH" << EOF
---
related:
- "[path/to/source.c](/path/to/source.c)"
- "[Relevant Concept](/path/to/relevant-concept.md)"
tags:
- source-map
sources:
- "[path/to/source.c](/path/to/source.c)"
---

# Source Map: path/to/source.c

## File Overview



## Key Data Structures

- **\`struct ...\`**:

## Core Functions

### \`function_name()\`

- **Purpose**:
- **Inputs/Outputs**:
- **Typical Callers**:
- **Related Files**:
EOF
        ;;
    2) # 02-callflows
        cat > "$FILE_PATH" << EOF
---
related:
- "[Relevant Flow](/path/to/flow.yml)"
tags:
- callflow
---
name: "Title of the Flow"
description: "A brief description of what this callflow illustrates."
entrypoint: "initial_function"
steps:
  - function: "initial_function"
    file: "path/to/file.c"
    notes: "Description of this step."
    calls: "next_function"
  - function: "next_function"
    file: "path/to/other_file.c"
    notes: "..."
EOF
        ;;
    3) # 03-notebooks
        cat > "$FILE_PATH" << EOF
---
related:
- "[Topic Link](/path/to/topic.md)"
tags:
- daily-log
---

# YYYY-MM-DD: Title of Today Log

## Progress Today

- DONE: 


## Findings



## Blockers



## Next Steps

- TODO: 


EOF
        ;;
esac

# 5. Output summary
echo
log "Script finished successfully."
log "File created: $FILE_PATH"
log "Remember to fill in the placeholder content."
