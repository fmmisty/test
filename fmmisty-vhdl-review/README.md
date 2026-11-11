# VHDL Review

A Claude plugin that reviews VHDL and FPGA RTL for the defects that pass
simulation and then fail in synthesis, in timing, or on the board.

## What it checks

- Unintended latches and incomplete sensitivity lists
- Clock-domain crossings: missing synchronizers, multi-bit crossings, lost pulses
- Reset strategy: polarity, synchronous release, registers left out of reset
- Finite state machines: unreachable recovery, missing default outputs
- Numeric correctness: bit growth, truncation, signed and unsigned mixing, fixed-point scaling
- Non-synthesizable constructs in RTL and memory inference problems
- Handshake protocol violations and port or generic mismatches
- Naming consistency within the project

Each finding has a severity (Critical, Major, or Minor), a file and line, the
consequence in hardware, and a concrete fix.

## Components

| Component | Name | Purpose |
| --- | --- | --- |
| Skill | `vhdl-review` | Full review of the VHDL files, entities, or design you point it at |
| Command | `/fmmisty-vhdl-review:vhdl-review-diff` | Review only the VHDL changed on the current git branch |

## Install

In Claude Code:

```
/plugin install fmmisty-vhdl-review --marketplace fmmisty/test
```

## Usage

Ask Claude in plain language, for example:

- "Review `rtl/fir_filter.vhd` for synthesis problems."
- "Check this design for clock-domain crossing bugs."
- "Why does this FSM work in simulation but hang on the board?"

Or review a branch before merging:

```
/fmmisty-vhdl-review:vhdl-review-diff main
```

## What the plugin runs, sends, and fetches

The plugin contains only Markdown instructions. It has no hooks, no MCP
servers, no scripts, and no dependencies. It makes no network requests and
stores no data. During a review Claude reads the VHDL files in your project,
and the diff command asks Claude to run read-only `git diff` commands. Files
are not modified unless you ask for fixes.

## Limitations

The review is a reading of the source. It does not run a simulator,
synthesis, or static timing analysis, and it cannot see constraints or
vendor IP that are not in the repository. Treat it as a complement to those
tools, not a replacement.

## License

MIT. See the `LICENSE` file.
