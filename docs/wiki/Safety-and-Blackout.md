# Safety and Blackout

**Read this before running StageSignal in front of an audience.**

StageSignal can produce flashing light, fast colour changes and bright full-screen output. Some people with photosensitive epilepsy can have seizures triggered by flashing light. StageSignal cannot make a show safe for them. It can only give you tools and limits, and it does not replace your own judgement or your venue's rules.

## Strobe lock

**Disable strobing** blocks flashing content everywhere StageSignal controls it.

When it is on:

- Scenes marked as flash-capable (ColorField's full-frame add and Starburst's white add) switch to a bounded, non-flashing fallback.
- Bloom Pulse is held in a narrow range so it cannot flash.
- Flash-capable scenes are removed from the auto pool, drops and cues. A live flash scene is switched to a non-flash scene straight away.
- Recalling a flash scene from a snapshot is refused.
- Lighting holds strobe channels at their safe value.
- The lock is sticky. Loading a show does not clear it. Only the explicit Enable strobing action does.

The lock is on by default for new shows, and the Setup tab shows it as a toggle.

What the lock covers (S-01):

- Software scenes, effects, mappings, and fixture channels declared with the Strobe function.
- Not covered: a strobe mode hidden in a Macro range of a fixture whose profile does not mark it as Strobe.

## Imported media

Imported GIFs and videos can contain flashing content that the lock cannot see. The flash check is a heuristic. A clean result does not guarantee a safe video (S-02). Imported media carry a visible warning.

## Blackout

| Control | Key | Scope |
|---|---|---|
| Visual blackout | Ctrl+F8 | Screens only |
| Lighting blackout | Ctrl+Shift+F8 | DMX output only |
| Emergency blackout | F8 | Both screens and DMX. Never touches audio recording. Works from any focus, including text fields. |
| Restore | Shift+F8 | Clears the latches you set |

Blackouts latch. Pressing them again does nothing, and restoring is always an explicit action. A lighting blackout remains latched through a later Arm, so lights start dark.

### What blackout cannot do (S-05)

- It only acts on StageSignal's outputs. It cannot switch off a fixture that ignores DMX, a dimmer pack with its own look, or a projector's own feed.
- If power or the cable fails, StageSignal cannot guarantee the lights go dark. Node timeouts and safe states have not been tested (L-02).
- Keep a physical kill or a venue-level switch within reach. StageSignal does not replace one.

## Rules for the show

1. Test blackout (F8) on an empty rig before the doors open.
2. Check flash content in the preview before you publish it to an audience output.
3. Keep the strobe lock on unless you have a reason and a plan.
4. Keep the house lights and emergency lighting outside StageSignal's control.
5. Have someone on the desk who knows where the blackout key is.

## Privacy and the audience

- Audience windows never show operator panels or diagnostics unless you enable the diagnostics overlay for that output.
- Crowd and silhouette features, if built, would process people in the venue. They are off by default and not built. Biometric and consent rules differ by place. Check your local law (S-04).

## Engineering bound

The strobe limits follow the WCAG 2.3.1 "three flashes per second" threshold as an engineering bound. This is not a safety certification, and StageSignal is not a medical or compliance product (S-03).
