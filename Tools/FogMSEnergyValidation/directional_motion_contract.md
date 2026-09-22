# Directional density motion contract

New FogMS actors default to Directional Wind. Previously serialized actors with no `FogMSDirectionalMotion` custom version are loaded as Legacy Velocity Vectors. Their three existing vector property names, values and absolute-time animation behavior are retained. Migration is explicit through `Use Directional Motion`; no scene asset is automatically saved or rewritten.

## Artist controls

Select `WindDirectionComponent` and rotate its arrow to set the world wind direction. This actual runtime `UArrowComponent` supplies the sampled direction; there is no separate rotatable child visualization. It has absolute world rotation/scale, so rotating/resizing the Box changes clipping bounds without turning the wind. It is hidden in game, has no collision/navigation/shadow/ray-tracing participation and retains its direction in cooked runtime.

`Wind Speed` is a nonnegative double value in cm/s along the direction component's world X axis. All three octaves inherit this velocity. The existing Relative Detail / Relative Second Detail world velocity vectors are advanced controls, default zero; leaving them zero advects the complete shape coherently. This slice does not change how detail contributes to the density mask or introduce fluid simulation.

`Texture Offset (World)` translates the pattern in world XYZ centimetres, for static and animated density. Positive X moves a texture feature toward positive X. It does not move Box bounds. Zero offset with animation disabled uses the original static phase path exactly. Local coordinate mode ignores the world offset.

## Continuity and saved state

Directional motion serializes hidden `DensityMotionReference`: epoch time, three world displacement anchors and three velocities, all in double precision. This implementation struct is absent from the artist Details panel. Its `BlueprintReadOnly` property supports reflection snapshots; the finite-validating `restore_density_motion_reference(reference)` API restores it after the matching wind controls. It must not be changed to non-editable `BlueprintReadWrite`: UE `AActor::ResetPropertiesForConstruction` resets such actor properties to the class default on normal property edits, losing the continuity anchor. Manual-time controls and relative detail velocities are Advanced Display. For each octave:

```
D(t) = anchorDisplacement + anchorVelocity * (t - anchorTime)
phase = frac(frac(center * frequency) - frac(D(t) * frequency) - frac(TextureOffsetWorld * frequency))
```

When arrow direction, Wind Speed or advanced relative velocities change, the old segment is evaluated at the current shared CPU time; its displacement becomes the new anchor and only then is velocity changed. Repeated edits at the same frame/manual time therefore cannot move the phase. Normal time advancement leaves the reference unchanged and does not increment the global history revision. Authored edits/reference restoration remain discontinuities for the revision key, even when an individual edit preserved the density phase.

The existing per-world/per-frame CPU clock supplies the material and packet phases. No GPU clock or extra packet rows are added. Local reactive history and current shadow-cache phases remain the parent renderer's responsibility.

For a newly initialized Directional reference, first live activation starts at zero displacement at that snapshot. First manual activation uses epoch zero, enabling an absolute manual-time reference. Explicit Legacy conversion instead anchors at the conversion time and preserves the currently displayed phase, including when legacy animation was disabled/unsupported.

Freeze/Resume preserve effective time and phase as before. Wind changes while frozen change the future slope without moving the frozen image. `Animate Density = false` restores the authored static mapping; use Freeze to pause at the current moving phase. The reference remains saved while animation is disabled. Re-enabling after a time interval is not a pause/resume operation.

Manual seeks are reproducible with the **reference and wind controls restored together**. This is the currently saved linear segment, not a full historical animation curve: seeking before a prior velocity edit extrapolates the saved segment. Runtime world time restarts on a new editor/play session; use a frozen/manual reference when exact cross-session phase reproduction is required. A wind control value and a time alone do not encode the past velocity edits.

The Blueprint/Python `reset_motion_origin()` API sets all three displacement anchors to zero at the current time and retains the wind controls, offset and manual time. It is an explicit phase reset. `use_legacy_motion()` restores the retained original absolute-time vector behavior; it is an explicit phase change, not a continuous inverse conversion. These two recovery APIs do not add Details buttons. Neither alters density texture, threshold, detail strength, lighting quality or Box bounds. Artist buttons remain Use Directional Motion, Freeze and Resume.

## Verification

`python -P Tools/FogMSEnergyValidation/directional_motion_contract.py` runs 20 CPU contracts for large elapsed-time speed changes, direction changes, same-frame edits, zero-speed hold, pause/freeze/resume, coherent octaves, relative-detail changes, serialized manual seek replay, active/static legacy conversion, exact static parity, offset sign and explicit reset semantics. The lifecycle regression reads the current actor property specifiers and applies the UE 5.8 construction-reset predicate before evaluating motion. It reproduces the formerly hidden writable property's manual `t=10000` jump to 7,500,000 cm and proves the corrected property preserves the old displacement across speed/arrow edits, including a serialized live reference at a late world time. This is a source-backed CPU lifecycle model, not execution of UE construction.

`directional_motion_probe.py` is the explicit native acceptance probe. It records phase/reference snapshots around edits, verifies the actual arrow's world rotation and absolute transform settings survive actor reconstruction, checks public reference replay and restores the original controls, reference and arrow without saving the map. An existing Directional actor is also accepted; this probe does not save/reload a level to manufacture migration evidence. The script requires the public restore API before any mutation.

Native acceptance remains required: strict build/UHT, existing-map Legacy selection through custom version, new actor Directional default (temporary unsaved actor only if authorized), independent direction component UI, no phase jump on manual frozen edits, serialization/reload of the reference, legacy AnimationProbe parity and a camera-stationary live visual test. The CPU tests do not prove these Unreal integration paths.
