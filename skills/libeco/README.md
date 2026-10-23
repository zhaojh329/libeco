# libeco agent skill

This English-language skill helps coding agents write, review, and debug programs
using libeco's Linux C++20 public API. It includes API contracts, integration
instructions, and a complete example for libeco 0.1.0.

## Install in Codex

From the libeco repository root, copy the entire skill directory to your personal
skills directory. The following command installs a new copy and refuses to merge
it into an existing installation:

```sh
mkdir -p "$HOME/.agents/skills" &&
    test ! -e "$HOME/.agents/skills/libeco" &&
    cp -R skills/libeco "$HOME/.agents/skills/libeco"
```

For an update, replace your existing copy after preserving any local edits.
Copying the directory also copies its references and license; the installation
does not depend on keeping this repository checkout. Installing the skill does
not install the libeco library or build tools.

Codex discovers user skills in `~/.agents/skills`. If it does not appear, restart
Codex. See the [official skill documentation](https://learn.chatgpt.com/docs/build-skills)
for discovery locations and other installation options.

Invoke it explicitly, for example:

```text
Use $libeco to implement a nonblocking TCP client with read and write timeouts.
Use $libeco to review this coroutine's resource ownership and error handling.
Use $libeco to run a child process and drain stdout and stderr concurrently.
```

Codex may also select the skill automatically for matching libeco tasks. Other
agents that support `SKILL.md` can use the same directory in their own skill
location; `agents/openai.yaml` is optional Codex UI metadata.

## Contents

- `SKILL.md`: scope, runtime model, and coding/review guidance.
- `references/api.md`: public signatures and lifecycle contracts.
- `references/building.md`: build integration and a complete C++ example.
- `agents/openai.yaml`: Codex display metadata.
- `LICENSE`: MIT license for this independently copyable skill.

When libeco changes, update the affected reference alongside its public API.
Prefer the target installation's public headers when its version differs from
this skill's snapshot.
