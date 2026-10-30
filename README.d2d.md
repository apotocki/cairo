# Cairo with a Direct2D surface

The `d2d` branch of this repository is Cairo 1.18.4 with:

- a **Direct2D surface** in `src/win32/d2d/` (`cairo_d2d_*` in `cairo-win32.h`, `CAIRO_HAS_D2D_SURFACE`), derived from
  the D2D backend Mozilla wrote for cairo, on **Direct3D 10.1** and on **Direct3D 11** (`cairo_d2d_create_device11`);
- the non-separable CSS blend modes (`CAIRO_OPERATOR_MULTIPLY` … `EXCLUSION`) on that surface, by pixel shaders
  (`cairo-d2d-blend.fx`, `cairo-d3d11-shaders.hlsl`; their compiled forms are the headers beside them, so no shader
  compiler is needed to build);
- a GPU drop shadow (`cairo_d2d_surface_draw_shadow`);
- clean shutdown in `cairo_debug_reset_static_data()`: the Win32 device ([issue #19](https://gitlab.freedesktop.org/cairo/cairo/-/issues/19)),
  the DirectWrite, Direct2D and WIC singletons, and pixman's implementations when the pixman found provides
  `pixman_reset_static_data()`;
- `CAIRO_STATUS_UNSUPPORTED` (100, outside upstream's sequence).

The Direct3D 11 backend is described in `doc/d2d/d3d11-backend.md`; `test/d2d/` holds two standalone test programs.
Direct3D is loaded at run time, so the library links no Direct3D import library.

## Building

With Cairo's meson build, where the surface is the `d2d` option (disabled by default; it needs `dwrite` and the
Direct3D 10.1 / 11 and Direct2D 1.1 headers of the Windows SDK):

```sh
meson setup build -Ddwrite=enabled -Dd2d=enabled
meson compile -C build
```

## Licensing

Cairo is under the **LGPL 2.1 or the MPL 1.1**, at the recipient's choice (`COPYING`), and so are the changes of this
branch, the Direct2D and Direct3D backends included: they are derived from Mozilla's code under that license, whose
notices the files keep. The new files without such a notice carry Cairo's.
