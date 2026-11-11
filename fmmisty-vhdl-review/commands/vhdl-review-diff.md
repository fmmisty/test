---
description: Review only the VHDL changed on the current git branch
argument-hint: "[base-branch]"
---

Review the VHDL changes on the current branch using the `vhdl-review` skill.

1. Determine the base: use `$ARGUMENTS` if given, otherwise the repository's
   default branch.
2. List changed `.vhd` and `.vhdl` files with `git diff --name-only`
   against the merge base, including uncommitted changes.
3. If no VHDL files changed, say so and stop.
4. For each changed file, read the whole design unit, not only the changed
   hunks, so that findings account for the surrounding processes, clocks, and
   resets.
5. Report findings that the change introduces or exposes. Mention
   pre-existing problems in unchanged code only when they are Critical, and
   label them as pre-existing.

Use the severity levels and report format defined by the skill.
