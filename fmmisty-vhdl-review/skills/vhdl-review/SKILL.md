---
name: vhdl-review
description: Review VHDL / FPGA RTL for bugs that simulation often misses - unintended latches, incomplete sensitivity lists, unsafe clock-domain crossings, reset problems, numeric width and signedness errors, and non-synthesizable constructs. Use when the user asks to review, check, audit, or lint VHDL (.vhd, .vhdl) files, an entity, an architecture, or an FPGA design, or asks why a design works in simulation but fails in hardware.
---

# VHDL review

Review VHDL the way an experienced FPGA engineer would before sign-off: find
the defects that survive simulation and show up in synthesis, timing, or on
the board. Reply in the language the user writes in.

## Workflow

1. **Scope.** Identify the files to review. If the user named none, look for
   `.vhd` / `.vhdl` files and ask which ones only when the set is large or
   ambiguous. Skip vendor-generated IP wrappers and netlists unless asked.
2. **Context.** Before judging any file, establish:
   - the VHDL revision in use (93, 2002, 2008, 2019) from project files or syntax;
   - the target (vendor and family, or ASIC) when project files reveal it;
   - every clock and reset, and which processes belong to which clock domain;
   - whether the file is synthesizable RTL or a testbench. Testbenches may
     use `wait`, file I/O, and delays; do not flag those there.
3. **Read whole design units.** Read each entity together with its
   architecture and the packages it uses. Follow a signal to its driver and
   its loads before reporting on it; most false positives come from judging a
   line in isolation.
4. **Check** against `references/checklist.md`. Load it before reviewing.
5. **Verify each finding.** Re-read the cited lines and confirm the failure
   really happens. Drop anything you cannot tie to a concrete consequence.
6. **Report** in the format below.

Do not modify files during a review unless the user asks for fixes.

## Severity

- **Critical** - wrong behavior in hardware, or simulation/synthesis
  mismatch: unsynchronized CDC, inferred latch, multiple drivers, incomplete
  sensitivity list, truncation that corrupts data, unreachable FSM recovery.
- **Major** - likely to break under some configuration or to fail timing:
  reset release not synchronized, unregistered outputs on long paths, generic
  combinations that produce zero or negative ranges, gated clocks.
- **Minor** - maintainability and portability: `std_logic_arith` usage,
  magic numbers, inconsistent naming, dead code.

## Report format

Start with one or two sentences giving the overall verdict. Then list
findings, most severe first:

```
[Critical] path/to/file.vhd:120 - short title
What is wrong, and what happens in hardware because of it.
Fix: the concrete change, with a short code snippet when it helps.
```

End with a short list of what was checked and found clean, and anything that
could not be checked (for example, timing constraints not present in the
repository). If there are no findings, say so plainly rather than inventing
minor ones.

## Judgment notes

- Prefer few, certain findings over a long list of style remarks.
- Respect the project's existing conventions (reset polarity, prefix and
  suffix naming, one-process versus two-process FSMs). Flag inconsistency
  within the project, not deviation from your own preference.
- An asynchronous signal entering a clock domain is a finding even when the
  simulation passes; metastability does not appear in RTL simulation.
- Vendor primitives and attributes (`ASYNC_REG`, `syn_preserve`, XPM
  macros, and similar) are legitimate; check they are applied correctly rather than
  flagging them as non-portable.
