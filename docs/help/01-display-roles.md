# Display role assignment
Every logical output (Audience, Program, Controls) is bound to a physical display by its stable identity, never by the Windows display number.

## Steps
- Press Identify displays (Setup tab or View menu): each display shows its number and name for about five seconds.
- Choose the output and the display, then Assign.
- Mark the display holding the operator window as the Controls display. Audience windows can never cover it.

## What the states mean
- Active: the audience window covers that display exactly (borderless, no exclusive mode, no resolution change).
- Disconnected: the display is gone. The window is removed, the controls stay usable, and the route comes back only when the same display returns.
- Needs reassign: two identical monitors cannot be told apart after a topology change. The route stays idle until you assign it again.
- Blocked: the choice would cover the controls display or reuse another output's display.

Audience windows never show diagnostics unless you tick the diagnostics box for that output.
