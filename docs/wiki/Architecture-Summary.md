# Architecture Summary

From the specification (sections 8 to 10) and the core layout.

- **Operator and audience separation.** The operator uses the primary screen, the audience sees a separate output, and a third screen can carry controls (specification section 1).
- **Hardware floor.** The baseline is an 800 MHz processor with 512 MB of RAM. Qualification assumes one core and no required modern SIMD, with a fallback for any accelerated path (section 1).
- **Modular core.** Portable modules under `core/` (for example `audio`, `midi`, `lighting`, `record`, `broadcast`) with a JUCE app on top (`app/`), as in the specification's module catalogue (section 9).
- **Contracts.** Functions, APIs and protocols are specified in section 10.
