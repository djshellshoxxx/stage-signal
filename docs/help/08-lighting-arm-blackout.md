# Lighting: arm, blackout, timeouts
- Setup and preview never transmit. The page shows DRY-RUN until you press Arm and confirm.
- A disconnect, reconnect, patch change or restart always leaves lighting disarmed.
- Lighting Blackout and Emergency Blackout (F8) override every cue by the next lighting tick. Blackout values come from the fixture profile.
- Blackout cannot guarantee a dark rig after a cable or network loss: nodes and fixtures may hold the last values. Configure the Art-Net node or sACN receiver loss behaviour and the fixture no-DMX mode, and test it.
- Disable Strobing prevents flash and strobe presets and channels.
- Built-in fixture profiles are generic and not hardware-qualified. USB DMX adapters are not listed as qualified.
