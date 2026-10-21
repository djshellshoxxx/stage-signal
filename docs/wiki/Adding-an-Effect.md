# Adding an Effect

Effects are per-scene amounts applied after the scene is drawn. Two exist today: Prism Mirror and Bloom Pulse. This page describes the path for a third one. Read [Scenes and Effects](Scenes-and-Effects) and `docs/specs/render.md` first.

## What is stored today

Look data for a scene is in `model::SceneParams`:

- `effectAmount[2]`: index 0 is Prism Mirror, index 1 is Bloom Pulse. Range 0 to 1, where 0 is off.
- `prismSegments`: integer 2 to 16, default 6.

In the show file these live under the scene's `params` object (`effectAmount` as a two-number array, `prismSegments` as an integer). `model::EffectKind` (`None`, `PrismMirror`, `BloomPulse`, `Count`) names the effects for the render path.

## Steps

1. **Model.** Append to `model::EffectKind` before `Count`. If the effect needs parameters, add them to `SceneParams` with a default, and update the equality operator the model uses for "same params" checks.
2. **Amount slot.** The amount array is fixed size. Growing it means changing `effectAmount[2]`, its validation in `persist`, and the `checkKeys` list in `show_json.cpp`. That is a schema change: bump the minor version and add a test for loading an older file.
3. **Render.** Implement the effect in `core/render/src/effects.cpp` behind `applyEffect`. Follow the rules:
   - Amount 0, NaN or negative leaves the frame untouched.
   - No allocation after `prepare()`. Use the caller-owned `EffectScratch`. Check buffer size before allocating.
   - Cost must fit the BASE budget, or the benchmark must refuse it with a reason. Note the relative cost in the spec.
4. **Strobe rule.** If the effect can flash, it must respect `SceneInput.strobingDisabled`. The fallback has to be a bounded, non-flashing range (see the Bloom Pulse clamp). Only the show's Bloom Pulse cap is wired today, so a new flash-capable effect also needs a show-engine change (`Tunables::strobeLockedBloomMax` is specific to Bloom Pulse).
5. **Show.** Add the look control to the undoable list in `core/show/src/engine.cpp` (section 6 of `docs/specs/show.md`). Add an `ActionId` if the operator needs a button, appended at the end of the enum (see [Adding an Action](Adding-an-Action)).
6. **Tests.** Name them with the requirement, for example `NEW-0x: <effect> ...`. Cover:
   - pass-through at amount 0 (bit-exact)
   - golden frame in `tests/unit/render/golden/frames.txt`
   - allocation test after `prepare()`
   - strobe-lock behaviour, if it can flash
   - round trip through the show file, including the new fields
7. **Docs.** Update `docs/specs/render.md` (effects section and Status), `docs/specs/show.md` if the show rules changed, and the traceability row for the effect. The traceability evidence must point at real tests.

## Checklist

- [ ] Enum appended, not reordered
- [ ] Defaults give pass-through
- [ ] No heap use after `prepare()`
- [ ] Flash rule and fallback tested
- [ ] Show file round trip tested
- [ ] Spec and traceability updated in the same PR
