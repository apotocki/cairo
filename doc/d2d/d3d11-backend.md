# cairo-d2d: D3D11 backend

The Direct2D surface of `src/win32/d2d/` runs on Direct3D 10.1 or on
Direct3D 11. This document describes the Direct3D 11 API and the backend
abstraction behind both.

## Status

- Backend abstraction (`cairo-d3d-backend-private.h`), D3D10.1 extraction
  (`cairo-d3d10-backend.cpp`) and the refactored `cairo-d2d-surface.cpp` are
  in place.
- D3D11 backend (`cairo-d3d11-backend.cpp`) is implemented against
  `cairo-d3d11-shaders.h`, which holds the compiled bytecode (built with
  fxc from the Windows 10.0.22621.0 SDK by the commands in "Regenerating
  shaders" below).
- The comparison tool (`test/d2d/d3d-backend-compare-test.cpp`) and the
  shadow test (`test/d2d/d2d-shadow-effect-test.cpp`) are standalone
  programs, not part of cairo's test suite.

## New public API

All declared in `cairo-win32.h`, guarded by `CAIRO_HAS_D2D_SURFACE`.

```c
cairo_device_t *cairo_d2d_create_device11(void);
cairo_device_t *cairo_d2d_create_device_from_d3d11device(struct ID3D11Device *device);
struct ID3D11Device *cairo_d2d_device_get_device11(cairo_device_t *device);
cairo_surface_t *cairo_d2d_surface_create_for_texture11(cairo_device_t *device,
                                                         struct ID3D11Texture2D *texture,
                                                         cairo_content_t content);
cairo_status_t cairo_d2d_surface_draw_shadow(cairo_surface_t *dst,
                                             cairo_surface_t *source,
                                             double offset_x, double offset_y,
                                             double sigma,
                                             double red, double green,
                                             double blue, double alpha);
```

- `cairo_d2d_create_device11()` creates a hardware `ID3D11Device` (feature
  levels 11.1 → 10.0, `D3D11_CREATE_DEVICE_BGRA_SUPPORT |
  D3D11_CREATE_DEVICE_SINGLETHREADED`) and wraps it.
- `cairo_d2d_create_device_from_d3d11device()` wraps a caller-owned device;
  it must have been created with `D3D11_CREATE_DEVICE_BGRA_SUPPORT`.
- The device kind is fixed at creation and stored on `cairo_d2d_device_t`;
  all surfaces created from a device inherit its kind.
- `cairo_d2d_surface_draw_shadow()` runs the D2D1.1 `CLSID_D2D1Shadow`
  effect (Gaussian blur of source's alpha, tinted, composited SOURCE_OVER
  into dst) as a self-contained side pass on a private
  `ID2D1DeviceContext` created lazily over the backend's own D3D11 device.
  See the doc comment in `cairo-win32.h`.

## Per-kind behavior of existing (version-agnostic) entries

| Entry point | D3D10-backed device | D3D11-backed device |
|---|---|---|
| `cairo_d2d_device_get_device()` | returns the `ID3D10Device1*` | returns `NULL` |
| `cairo_d2d_device_get_device11()` | returns `NULL` | returns the `ID3D11Device*` |
| `cairo_d2d_surface_create_for_texture()` (D3D10 texture) | works | returns a surface in `CAIRO_STATUS_DEVICE_TYPE_MISMATCH` error state |
| `cairo_d2d_surface_create_for_texture11()` (D3D11 texture) | returns a surface in `CAIRO_STATUS_DEVICE_TYPE_MISMATCH` error state | works |
| `cairo_d2d_surface_draw_shadow()` | returns `CAIRO_STATUS_DEVICE_TYPE_MISMATCH` | works |
| `cairo_d2d_surface_get_texture()` (`ID3D10Texture2D*` getter) | works | returns `NULL` |
| `cairo_d2d_surface_create()`, `cairo_d2d_surface_create_for_hwnd()`, `cairo_d2d_surface_create_for_handle()`, `cairo_d2d_finish_device()`, `cairo_d2d_present_backbuffer()`, `cairo_d2d_scroll()`, `cairo_d2d_get_dc()`/`release_dc()`, `cairo_addref_device()`/`cairo_release_device()` | work | work |

`cairo_d2d_surface_create_for_handle()` (legacy `HANDLE` shared resource) is
version-agnostic on both kinds: D3D10.1 and D3D11 devices can `OpenSharedResource`
a handle created by either version's `D3D1x_RESOURCE_MISC_SHARED` texture, in
the same process, during migration.

`cairo_d2d_surface_create_for_hwnd()` is fully implemented for D3D11 devices
too — it falls out of the abstraction for free (the legacy BLT swap chain is
created off `IDXGIDevice`, obtained generically via
`cairo_d3d_backend::device_unknown()`).

Mixing surfaces from devices of different kinds in one operation (e.g.
painting a D3D11-backed surface onto a D3D10-backed one) is unsupported and
fails with `CAIRO_INT_STATUS_UNSUPPORTED`, the same way cross-device
operations already did before this port.

## Internal backend abstraction

See `cairo-d3d-backend-private.h` for the full `cairo_d3d_backend` interface.
`cairo-d2d-surface.cpp` no longer includes any D3D-version-specific header;
`cairo_d2d_surface_t::surface` / `bufferTexture` / `buffer_rt_view` /
`buffer_sr_view` are `RefPtr<IUnknown>` and are only ever passed back into
the owning device's backend, which knows how to interpret them.

- `cairo-d3d10-backend.cpp` — mechanical extraction of the original
  D3D10.1 + Effects-framework code; behavior is unchanged from before the
  port.
- `cairo-d3d11-backend.cpp` — new. No Effects runtime on D3D11, so the fx
  techniques become three explicit shader/state combinations created once
  per device and reused across draws (`draw_sample_texture`,
  `draw_blend_texture`, `draw_text_texture` on the interface).

## Regenerating shaders

`cairo-d3d11-shaders.hlsl` is the plain-HLSL port of `cairo-d2d-blend.fx`
(one vertex shader, three pixel shaders — semantics kept identical to the
`.fx` source; the fx `technique10` render-state blocks became explicit
`ID3D11BlendState`/sampler objects in `cairo-d3d11-backend.cpp` instead).

`cairo-d3d11-shaders.h` holds the precompiled bytecode and must be
regenerated whenever the `.hlsl` changes, using the Windows SDK's `fxc.exe`
(e.g. `C:\Program Files (x86)\Windows Kits\10\bin\<version>\x64\fxc.exe`),
run from `src/win32/d2d`:

```
fxc /nologo /T vs_4_0 /E VS_Quad        /O2 /Vn g_d3d11_VS_Quad        /Fh vs_quad.h        cairo-d3d11-shaders.hlsl
fxc /nologo /T ps_4_0 /E PS_Sample      /O2 /Vn g_d3d11_PS_Sample      /Fh ps_sample.h      cairo-d3d11-shaders.hlsl
fxc /nologo /T ps_4_0 /E PS_SampleText  /O2 /Vn g_d3d11_PS_SampleText  /Fh ps_sampletext.h  cairo-d3d11-shaders.hlsl
fxc /nologo /T ps_4_0 /E PS_Blend       /O2 /Vn g_d3d11_PS_Blend       /Fh ps_blend.h       cairo-d3d11-shaders.hlsl
```

Then replace the four byte arrays in `cairo-d3d11-shaders.h` with the
generated ones. `vs_4_0`/`ps_4_0` (not `*_4_0_level_9_*`)
matches the FL10.0+ minimum this backend targets, and mirrors the `fx_4_0`
level the D3D10 backend's precompiled effect (`g_main` in
`cairo-d2d-private-fx.h`) already uses.

## The D3D10 vs D3D11 comparison tool

`test/d2d/d3d-backend-compare-test.cpp` (not part of the cairo library)
renders the same battery on both device kinds — the operator/
blend-mode grid (all Porter-Duff operators plus the CSS blend-mode ones,
which is what actually exercises `draw_sample_texture` /
`draw_blend_texture`), ClearType text over opaque and transparent
backgrounds, and three surface-pattern cases (integer-translated fastblit
copy, non-OVER operator forcing the brush + temp-rt path, and a scaled
transform) — then reads both back via the public
`cairo_surface_map_to_image()`/`cairo_surface_unmap_image()` API (this
backend doesn't implement `map_to_image` directly, but cairo core's
`_cairo_image_surface_clone_subimage()` fallback goes through
`acquire_source_image`, which is implemented, so the public entry point
works unmodified) and compares them pixel-by-pixel with a small
per-channel tolerance (AA/blend-shader rounding, not exactness).

It isn't built by meson — compile it standalone against a cairo built with
`-Dd2d=enabled`, e.g. from the root of the source tree:

```
cl /EHsc /std:c++17 /I <cairo-build-dir>\src ^
   /I src /I src\win32 ^
   test\d2d\d3d-backend-compare-test.cpp ^
   /link <path-to-cairo-lib>.lib d2d1.lib dwrite.lib dxgi.lib ^
   d3d10_1.lib d3d11.lib ole32.lib
```

Run the resulting `.exe`; it prints a mismatch
percentage and exits nonzero on failure or if either device fails to
initialize (e.g. no D3D11 driver available).
