# Art direction
Status and approved scope: exploring (upgrade look only). The base look is the original game and is not up for change.
The intended look and feeling: the Switch remake's toy-diorama look, sharper. 4K, clean anti-aliasing, stable shadows, no smearing, no color shift.
Current visual decision: none approved. Upgrade look options are explored in docs/lookbook/README.md.

## Preserve and avoid
Accepted examples and features: original color grading, tilt-shift depth of field, outlines, UI layout and fonts, 2D/3D sprite timing.
Rejected directions and why: none yet. Avoid: AI upscaled textures replacing originals by default, changed color grading, stretched UI.

## Relevant production rules
- Palette/contrast and camera: the original camera, framing and grading are the reference. Every upgrade is compared side by side with faithful mode at the same frame.
- 2D/UI: UI scales with resolution and keeps the 16:9 layout; text stays crisp.
- 3D: higher internal resolution, AA and shadow resolution only change sharpness, never content.
- Audio: original mix and timing; no resampling artifacts.
- Motion: animations interpolate at high frame rate only where the original plays them continuously; timed-hit windows never change.
- Source/license: no game assets in Git. Mod and upgrade assets carry a manifest in art/manifests/.

Technical PASS does not approve appearance or feel. The owner approves each upgrade's look.
