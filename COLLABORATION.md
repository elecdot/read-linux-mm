# Collaborate with Others

This guide explains the common workflow we'll use while studying the Linux v2.4.18 memory management subsystem.

## Daily workflow

Your daily workflow should be:
- Prepare:
    - Regenerate the HTML docs with `doxygen Doxyfile`.
    - Check for outstanding TODOs with `rg TODO`.
        - Troubleshooting: This can be noisy because many TODOs appear in the kernel source.
- Decide what you're going to do and mark it with a TODO tag.
    - If you're planning to read a function or source file, add a Doxygen-style comment (`/** ... */`) above the function implementation.
    - Otherwise, create a notebook entry using `bash /workspace/bin/doc-init.sh`.
- Do your work while following the conventions in [DOCUMENTING.md](./DOCUMENTING.md).
- Mark the item as DONE.

## Git Workflow

### Day-1 Setup

#### Connect GitHub via SSH

```bash
ssh-keygen -t ed25519 -C "your_email@example.com" -f "$HOME/.ssh/id_ed25519"
# A passphrase will be required.
# Choose a strong passphrase and keep it safe.
# Make sure you can remember it.
cat ~/.ssh/id_ed25519.pub
# Copy the output.
```
Go to GitHub → Settings → SSH and GPG keys → New SSH key → paste the key.

Then run `ssh-add ~/.ssh/id_ed25519`, or add these lines to `~/.ssh/config` (optional, helpful behind restrictive networks):
```
# ~/.ssh/config
Host github.com
        HostName ssh.github.com
        Port 443
        User git
        IdentityFile ~/.ssh/id_ed25519
```

Verify with:
```bash
ssh -T git@github.com
# Hi <yourname>! You've successfully authenticated, but GitHub does not provide shell access.
```

#### Global Git config

```bash
git config --global user.name "Your Name"
git config --global user.email "your_email@example.com"
git config --global init.defaultBranch main
```

### Concepts

#### Version control system

Git is a version control system that supports:
- Version tracking
- Branching and merging
- Remote collaboration
- Open-source workflows

For a brief introduction, see this [YouTube video](https://www.youtube.com/watch?v=vA5TTz6BXhY).

#### Branch & Commit conventions

This gives you a quick start with the conventions. For more detail, see [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/).

If you're unsure, ask for advice from Chat before you branch or commit.

##### Branch

For any work, create and switch to a topic branch:

|Type|Pattern|Example|
|---|---|---|
|feature|`feat/<short-topic>[-<issue#>]`|`feat/parse-mm-docs-12`|
|fix|`fix/<short-topic>[-<issue#>]`|`fix/typo-in-slab-notes`|
|docs|`docs/<short-topic>[-<issue#>]`|`docs/mm-overview-appendix`|
|chore|`chore/<short-topic>`|`chore/ci-cache-tuning`|
|refactor|`refactor/<short-topic>`|`refactor/split-mm-notes`|
|test|`test/<short-topic>`|`test/parser-regressions`|

### Conventional Commits (samples)

Whenever you make a small, coherent change that should be recorded, commit.

- `feat(parser): extract page flags from headers`
- `feat(notes): add high-level VM roadmap`
- `fix(parser): handle null pgd pointer`
- `fix(ci): correct pytest path`
- `docs(mm): clarify buddy allocator flow`
- `docs(readme): add setup steps for WSL2`
- `refactor(parser): split lexer and reducer`
- `perf(parser): cache symbol lookups`
- `test(parser): add cases for swap entries`
- `chore(repo): enable fetch.prune by default`
- `build(make): add debug symbols`
- `ci(actions): run tests only when present`

> **Commit size**: keep commits small and coherent; **PR scope**: one topic/issue per PR.

### Daily Loop

Before you start, make sure you have this repo:
```bash
git clone git@github.com:elecdot/read-linux-mm.git
```
As you work day to day, use the pull → branch → edit → add → commit → push loop:
```bash
# 1) Sync local main
git checkout main
git pull --ff-only origin main

# 2) Create a topic branch
git switch -c feat/parse-mm-docs-12

# 3) Work & stage
# edit files...
git add path/to/file1 path/to/file2

# 4) Commit (Conventional Commits)
git commit -m "feat(parser): support v2.4 pgtable macros"

# 5) Push & set upstream
git push -u origin HEAD

# 6) Open PR (via UI) or CLI (GitHub CLI if installed)
# gh pr create --fill --base main --head feat/parse-mm-docs-12

# You generally shouldn't do this step; it's a bit trickier.
# 7) Address review: create fixup commits
#git commit --fixup <commit-sha-to-fix>
#git rebase -i --autosquash origin/main

# 8) Update PR
#git push

# Do this after you get a positive response from the maintainer.
# 9) After merge (squash by maintainer), clean up locally
git checkout main
git pull --rebase origin main
git branch -d feat/parse-mm-docs-12
git fetch -p
```