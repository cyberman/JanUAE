# JanUÆ

**The virtual 68k system for productive AmigaOS applications. Running on AROS.**

JanUAE is the AROS-focused continuation of Janus-UAE. It combines a 68k Amiga execution environment with native AROS integration so AmigaOS applications can participate in the host desktop instead of living only inside a separate emulator environment.

JanUAE is intentionally narrow: one product, one defined public virtual-system and integration profile, and a focus on productive application use rather than a broad emulator configuration matrix.

## Integration

The current source tree contains integration for rootless AmigaOS application windows and screens, AROS/AmigaOS clipboard exchange, host/guest mouse coordination, launching AmigaOS applications from AROS, host/guest communication, and AROS Commodity/Exchange control.

## Target

The current project target is **AROS/pc-i386**.

## Repository layout

- `src/` -- current JanUAE/UAE host-side sources
- `amiga/` -- AmigaOS-side integration components
- `docs/` -- current public product documentation
- `vendor/` -- preserved Janus-UAE 1.x reference material

Historical Janus-UAE build, installation, usage, and release notes remain under `vendor/`. They document the historical line and are not current JanUAE instructions.

## Heritage

JanUAE descends from Janus-UAE, which is based on E-UAE 0.8.29-WIP4. The original Janus-UAE AROS-integration history, including the Power2People bounty context, is preserved with the historical material under `vendor/`.

## License

JanUAE is distributed under the GNU General Public License. See [`COPYING`](COPYING).
