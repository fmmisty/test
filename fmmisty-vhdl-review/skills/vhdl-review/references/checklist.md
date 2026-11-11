# VHDL review checklist

Use this as a list of things to look for, not as a list of things to report.
Report an item only when the code actually exhibits it.

## 1. Combinational logic

- **Inferred latches.** In a combinational process every output must be
  assigned on every path. Look for `if` without `else`, `case` without
  `when others`, and outputs assigned in only some branches. Default
  assignments at the top of the process are the usual fix.
- **Sensitivity lists.** Every signal read in a combinational process must be
  listed (or use `process(all)` in VHDL-2008). A missing signal simulates as
  memory but synthesizes as pure logic.
- **Combinational loops.** A signal that depends on itself without a register.
- **Variables read before assignment** in a combinational process imply storage.

## 2. Sequential logic

- Clocked processes use `rising_edge(clk)` / `falling_edge(clk)`, not
  `clk'event and clk = '1'` mixed with other conditions.
- The sensitivity list of a clocked process contains only the clock and any
  asynchronous reset.
- Nothing is assigned outside the `if rising_edge` branch except the
  asynchronous reset branch.
- Mixed edges on one clock, or both edges in one process, are intentional and
  supported by the target.
- A signal has exactly one driving process. Multiple drivers on a resolved
  type compile cleanly and then produce `'X'` or a synthesis error.

## 3. Reset

- Polarity and style (synchronous or asynchronous) are consistent across the
  design, or the mix is deliberate.
- Asynchronous resets are asserted asynchronously but **released
  synchronously** in each clock domain (reset synchronizer).
- Control state (FSM state, valid flags, counters that gate behavior) is
  reset. Pure datapath registers may be left without reset on purpose.
- In an asynchronous-reset process, registers missing from the reset branch
  get an unintended clock enable driven by reset.
- Signal initial values are relied on only where the target supports them
  (most SRAM FPGAs do; ASIC and some flash-based devices do not).

## 4. Clock-domain crossing

- Every signal read in a clock domain other than the one that drives it is a
  crossing. List them.
- Single-bit level signals pass through at least two flip-flops in the
  destination domain, with no logic between the source register and the first
  synchronizer stage.
- Pulses crossing to a slower or unrelated clock are stretched or converted
  to a toggle/handshake; a plain two-flop synchronizer can miss them.
- Multi-bit values are never synchronized bit by bit. Use Gray coding (for
  counters and FIFO pointers), a handshake with a stable data hold, or an
  asynchronous FIFO.
- Synchronizer registers carry the vendor attribute that keeps them adjacent
  and excluded from retiming or shift-register extraction where relevant.
- External asynchronous inputs (buttons, UART RX, interrupt lines) are
  synchronized before use, and never fan out before the synchronizer.
- No derived or gated clocks built from fabric logic; use clock enables or
  dedicated clocking resources.

## 5. Finite state machines

- `when others` exists and leads to a safe state when the encoding has
  unused codes.
- Every state has an exit; no state depends on an event that may never come
  without a timeout where the protocol requires one.
- Outputs are assigned in every state or have defaults.
- Outputs that leave the device or feed another clock domain are registered.

## 6. Numeric and types

- `ieee.numeric_std` is used; `std_logic_arith`, `std_logic_unsigned`, and
  `std_logic_signed` are non-standard and conflict with it.
- Result widths: addition needs one extra bit, multiplication needs the sum of
  the operand widths. Check for silent truncation on `resize` and slicing.
- `signed` and `unsigned` are not mixed without an explicit conversion; sign
  extension is correct when widening.
- Fixed-point scaling: binary point positions match across adds, and rounding
  or saturation is applied where the algorithm needs it rather than wrap-around.
- Integer signals have explicit ranges; unconstrained `integer` becomes 32 bits.
- Counters wrap or saturate deliberately; comparisons use the right bound
  (`N-1` versus `N`).
- Ranges built from generics cannot become null or negative
  (`WIDTH-1 downto 0` with `WIDTH = 0`, `log2` of 1).
- `to_integer` on a vector that may contain metavalues, and array indexing
  that can exceed the bounds.

## 7. Synthesizability

- No `wait for`, `after` delays, file I/O, or `real` arithmetic in RTL
  (constants computed from `real` at elaboration are fine).
- Loops have static bounds.
- Memories follow the vendor's inference template when block RAM is
  intended: synchronous read, no reset on the array, supported read-during-write mode.
- Tri-states and `inout` appear only at the top-level pins.
- `'U'`, `'X'`, `'Z'`, `'-'` are not compared against in RTL logic.

## 8. Interfaces and structure

- Handshakes follow their protocol. For ready/valid style interfaces: valid
  does not wait for ready, data stays stable while valid is high and ready is
  low, and there is no combinational path from ready back to valid.
- Port directions are correct; `buffer` is avoided; outputs read internally go
  through an internal signal (or VHDL-2008 is in use).
- Unconnected or `open` ports and unused signals are intentional.
- Component declarations match entity ports and generics, or direct entity
  instantiation is used.
- Generic defaults are sensible and constrained with assertions where a bad
  value would silently break the design.

## 9. Naming and readability

- Naming follows the project's own convention consistently (clock and reset
  names, active-low markers, registered-signal suffixes, constant casing).
- No magic numbers where a constant or generic exists or should.
- Comments describe intent and interface timing, not the syntax.
- Dead code, commented-out blocks, and leftover debug signals.
