# Cairo with a Direct2D surface

This repository is a fork of [Cairo](https://gitlab.freedesktop.org/cairo/cairo). Its branches:

| Branch | What it is |
|---|---|
| `d2d` | Cairo 1.18.6 with a **Direct2D surface** on Direct3D 10.1 and 11, and the fixes below. |
| `legacy` | Cairo 1.18.6 with the fixes below only: the same library as upstream's, with nothing new in its API. |
| `master` | Upstream's `master`, unchanged. |

The tag `d2d-1.18.4` is the branch `d2d` as it was on Cairo 1.18.4.

## The fixes (both branches)

- `cairo_debug_reset_static_data()` destroys the device the Win32 surfaces share, which was never freed
  ([issue #19](https://gitlab.freedesktop.org/cairo/cairo/-/issues/19)): leak checkers no longer report it.
- `cairo_debug_reset_static_data()` frees pixman's implementations, which builds without destructors (MSVC) never
  free, when the pixman found provides `pixman_reset_static_data()` (meson defines `HAVE_PIXMAN_RESET_STATIC_DATA`
  then; upstream's pixman does not have it).
- Cairo 1.18.6 lost a WIC factory on every glyph surface it made with DirectWrite
  (`WICImagingFactory::Instance()` took a reference of its own on the factory `CoCreateInstance` returned): no more.

On `d2d`, `cairo_debug_reset_static_data()` also releases the DirectWrite singletons and the Direct2D factory of the
calling thread; the next use of Cairo creates them anew.

## What `d2d` adds

- A Direct2D surface type, `CAIRO_SURFACE_TYPE_D2D`, in `src/win32/d2d/`, derived from the Direct2D backend Mozilla
  wrote for Cairo: surfaces of a size, of a window (a DXGI swap chain), of a Direct3D texture, on a **device** that is
  either Direct3D 10.1 or Direct3D 11.
- The non-separable CSS blend modes `CAIRO_OPERATOR_MULTIPLY`, `SCREEN`, `OVERLAY`, `DARKEN`, `LIGHTEN`, `COLOR_DODGE`,
  `COLOR_BURN`, `HARD_LIGHT`, `SOFT_LIGHT`, `DIFFERENCE` and `EXCLUSION` on the GPU, by pixel shaders. Their compiled
  forms are in the tree (`cairo-d2d-private-fx.h`, `cairo-d3d11-shaders.h`): building needs no shader compiler.
- A GPU drop shadow, `cairo_d2d_surface_draw_shadow()` (Direct3D 11 devices).
- DirectWrite text drawn by Direct2D, with ClearType on surfaces that allow it (see "Text" below).
- `cairo_surface_set_subpixel_antialiasing()` and `cairo_surface_get_subpixel_antialiasing()` for every surface type.
- `CAIRO_STATUS_UNSUPPORTED` (100, outside upstream's sequence of statuses), which Cairo's error handling accepts.

Direct3D and Direct2D are loaded at run time (`LoadLibrary`): the library links no Direct3D import library, and a
device fails to be created, rather than Cairo failing to load, where they are missing.

## Building

The surface is the meson option `d2d` (`disabled` by default). It needs `dwrite` and the Direct3D 10.1 / 11 and
Direct2D 1.1 headers of the Windows SDK; MSVC or clang-cl, C++14 at least:

```sh
meson setup build -Ddwrite=enabled -Dd2d=enabled
meson compile -C build
```

`cairo-features.h` then defines `CAIRO_HAS_D2D_SURFACE`, and the functions below are declared in `cairo-win32.h`.

With [otok](https://github.com/otok-dev/otok), the recipe `cairo` for Windows builds this branch with its option `d2d`
(and `legacy`'s fixes without it):

```toml
[deps.cairo]
version = "1.18.6"
options = { d2d = true }
```

## The API

All in `cairo-win32.h`, behind `#if CAIRO_HAS_D2D_SURFACE`.

### Devices

A D2D device is a Direct3D device with Cairo's state for it. Every D2D surface belongs to one; surfaces of different
devices, or of devices of different kinds, are not drawn onto each other.

| Function | |
|---|---|
| `cairo_d2d_create_device11()` | A device on a new hardware Direct3D 11 device. The one to use. |
| `cairo_d2d_create_device_from_d3d11device(ID3D11Device *)` | A device on yours; it must have been created with `D3D11_CREATE_DEVICE_BGRA_SUPPORT`. |
| `cairo_d2d_create_device()` | A device on a new Direct3D 10.1 device (no drop shadow). |
| `cairo_d2d_create_device_from_d3d10device(ID3D10Device1 *)` | A device on yours. |
| `cairo_d2d_device_get_device11()`, `cairo_d2d_device_get_device()` | The Direct3D device (`NULL` if of the other kind). |
| `cairo_addref_device()`, `cairo_release_device()` | The device's references. |
| `cairo_d2d_finish_device()` | Flushes the device and waits for the GPU to finish its work. |
| `cairo_d2d_get_surface_vram_usage()`, `cairo_d2d_get_image_surface_cache_usage()` | Estimates of video memory in use. |

The creating functions return `NULL` on failure. **A D2D device is released with `cairo_release_device()`, never with
`cairo_device_destroy()`**, which corrupts it. Every surface holds a reference to its device, so the device can be
released as soon as its surfaces are made.

### Surfaces

| Function | |
|---|---|
| `cairo_d2d_surface_create(device, format, width, height)` | An offscreen surface (`CAIRO_FORMAT_ARGB32`, `RGB24` or `A8`). |
| `cairo_d2d_surface_create_for_hwnd(device, hwnd, content)` | A surface on a swap chain of the window, of its client area's size. |
| `cairo_d2d_surface_create_for_texture11(device, ID3D11Texture2D *, content)` | A surface drawing into your texture (Direct3D 11 devices). |
| `cairo_d2d_surface_create_for_texture(device, ID3D10Texture2D *, content)` | The same for Direct3D 10.1. |
| `cairo_d2d_surface_create_for_handle(device, HANDLE, content)` | A surface on a shared texture handle. |
| `cairo_d2d_surface_get_texture()` | The Direct3D 10 texture of a surface (`NULL` on a Direct3D 11 device). |
| `cairo_d2d_surface_resize(surface, width, height)` | New size; returns whether the contents were kept. |
| `cairo_d2d_present_backbuffer(surface)` | Shows what was drawn on a window surface. |
| `cairo_d2d_scroll(surface, dx, dy, clip)` | Moves the pixels of `clip` by `(dx, dy)`. |
| `cairo_d2d_get_dc(surface, retain_contents)`, `cairo_d2d_release_dc(surface, updated)` | A GDI device context on the surface, between the two calls. |
| `cairo_d2d_surface_get_width()`, `cairo_d2d_surface_get_height()` | Its size. |
| `cairo_d2d_surface_draw_shadow(dst, source, dx, dy, sigma, r, g, b, a)` | A Gaussian shadow of `source`'s alpha, composited into `dst`. |

Textures given to a surface are `DXGI_FORMAT_B8G8R8A8_UNORM` (or `A8_UNORM`) render targets; `content` says whether
their alpha counts. Drawing with a `cairo_t` on a D2D surface is ordinary Cairo.

### Threading

A device and its surfaces are used by one thread at a time: calls on the same device are serialized by the caller (the
Direct3D devices are single-threaded), though they may come from different threads.

## Examples

The examples leave errors out; every function returning a surface returns an error surface on failure
(`cairo_surface_status()`), and the device creators return `NULL`.

### Drawing offscreen and reading the pixels back

```c
#include <cairo.h>
#include <cairo-win32.h>

cairo_device_t *device = cairo_d2d_create_device11();
if (!device)
    device = cairo_d2d_create_device();           /* Direct3D 10.1 instead */

cairo_surface_t *surface = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, 400, 300);
cairo_release_device(device);                      /* the surface keeps it */

cairo_t *cr = cairo_create(surface);
cairo_set_source_rgb(cr, 1, 1, 1);
cairo_paint(cr);
cairo_set_source_rgba(cr, 0.9, 0.2, 0.1, 0.8);
cairo_arc(cr, 200, 150, 100, 0, 2 * M_PI);
cairo_fill(cr);
cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);   /* a blend mode, on the GPU */
cairo_set_source_rgb(cr, 0.2, 0.4, 1.0);
cairo_rectangle(cr, 150, 100, 200, 150);
cairo_fill(cr);
cairo_destroy(cr);

/* the pixels: paint the D2D surface onto an image surface */
cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 400, 300);
cr = cairo_create(image);
cairo_set_source_surface(cr, surface, 0, 0);
cairo_paint(cr);
cairo_destroy(cr);
cairo_surface_write_to_png(image, "out.png");

cairo_surface_destroy(image);
cairo_surface_destroy(surface);                    /* and with it the device */
```

### A window

The surface of a window draws into the back buffer of a swap chain; `cairo_d2d_present_backbuffer()` shows it. When
the window changes its size, so does the surface.

```c
static cairo_surface_t *window_surface;

/* once, the window created */
cairo_device_t *device = cairo_d2d_create_device11();
window_surface = cairo_d2d_surface_create_for_hwnd(device, hwnd, CAIRO_CONTENT_COLOR);
cairo_release_device(device);

/* in the window procedure */
case WM_SIZE:
    if (window_surface)
        cairo_d2d_surface_resize(window_surface, LOWORD(lparam), HIWORD(lparam));
    return 0;

case WM_PAINT: {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    cairo_t *cr = cairo_create(window_surface);
    draw(cr);                                       /* your drawing */
    cairo_destroy(cr);
    cairo_d2d_present_backbuffer(window_surface);
    EndPaint(hwnd, &ps);
    return 0;
}

case WM_DESTROY:
    cairo_surface_destroy(window_surface);
    window_surface = NULL;
    PostQuitMessage(0);
    return 0;
```

A window surface is `CAIRO_CONTENT_COLOR` for an opaque window, `CAIRO_CONTENT_COLOR_ALPHA` for a transparent one.

### Drawing into your Direct3D 11 texture

With your own Direct3D 11 renderer, Cairo can draw into a texture of it (a render target, the back buffer of your swap
chain), on your device:

```cpp
// your device, with BGRA support, which Direct2D needs
ID3D11Device *d3d;
D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                  nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
cairo_device_t *device = cairo_d2d_create_device_from_d3d11device(d3d);

D3D11_TEXTURE2D_DESC desc = {};
desc.Width = 512;
desc.Height = 512;
desc.MipLevels = 1;
desc.ArraySize = 1;
desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
desc.SampleDesc.Count = 1;
desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
ID3D11Texture2D *texture;
d3d->CreateTexture2D(&desc, nullptr, &texture);

cairo_surface_t *surface = cairo_d2d_surface_create_for_texture11(device, texture, CAIRO_CONTENT_COLOR_ALPHA);
cairo_t *cr = cairo_create(surface);
draw(cr);
cairo_destroy(cr);
cairo_surface_flush(surface);        // Cairo's drawing is in the texture now: sample it, present it

cairo_surface_destroy(surface);
cairo_release_device(device);
texture->Release();
d3d->Release();
```

Before another Direct3D device reads the texture (a shared one, through `cairo_d2d_surface_create_for_handle()`),
`cairo_d2d_finish_device()` waits until the GPU has done Cairo's work.

### A drop shadow

The shadow of what a surface holds (its alpha), blurred and tinted, drawn into another surface of the same Direct3D 11
device. It ignores the `cairo_t` state (transform, clip, operator) and composites with `OVER`:

```c
cairo_device_t *device = cairo_d2d_create_device11();
cairo_surface_t *card = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, 200, 120);
cairo_surface_t *scene = cairo_d2d_surface_create(device, CAIRO_FORMAT_ARGB32, 400, 300);
cairo_release_device(device);

cairo_t *cr = cairo_create(card);
cairo_set_source_rgb(cr, 1, 1, 1);
cairo_rectangle(cr, 10, 10, 180, 100);
cairo_fill(cr);
cairo_destroy(cr);

/* offset (8, 8) in scene, blur sigma 6 px, black at 50 % */
cairo_status_t status = cairo_d2d_surface_draw_shadow(scene, card, 100 + 8, 90 + 8, 6.0, 0, 0, 0, 0.5);

cr = cairo_create(scene);                          /* then the card over its shadow */
cairo_set_source_surface(cr, card, 100, 90);
cairo_paint(cr);
cairo_destroy(cr);
```

It returns `CAIRO_STATUS_SURFACE_TYPE_MISMATCH` for surfaces that are not D2D surfaces of one device (or the same
surface twice), `CAIRO_STATUS_DEVICE_TYPE_MISMATCH` on a Direct3D 10.1 device, `CAIRO_STATUS_DEVICE_ERROR` without
Direct2D 1.1 (Windows 7 without its Platform Update) or when Direct2D fails; both surfaces stay usable.

### GDI on a D2D surface

```c
HDC dc = cairo_d2d_get_dc(surface, TRUE);         /* TRUE: with what the surface holds */
DrawTextW(dc, L"GDI", -1, &rect, DT_SINGLELINE);
cairo_d2d_release_dc(surface, NULL);               /* NULL: the whole surface was updated */
```

`retain_contents` set reads the surface back from the GPU, which is slow; unset, the DC starts transparent black.

### Text

DirectWrite fonts (`cairo_dwrite_font_face_create_for_dwrite_fontface()`, and the toy API, which takes DirectWrite
when Cairo is built with it) are drawn by Direct2D. On a surface with an alpha channel, ClearType (`CAIRO_ANTIALIAS_SUBPIXEL`) is drawn
only where the surface permits subpixel antialiasing, which it does by default: say so when the text may fall on
transparent pixels, and it is drawn in grayscale.

```c
cairo_surface_set_subpixel_antialiasing(surface, CAIRO_SUBPIXEL_ANTIALIASING_DISABLED);
```

`cairo_dwrite_scaled_font_allow_manual_show_glyphs(scaled_font, FALSE)` (`cairo-dwrite.h`) turns off, for one scaled
font, the way ClearType text is composited onto such surfaces by hand.

### Leak checkers

Before the program ends, after the last surface and device are gone:

```c
cairo_debug_reset_static_data();
```

Cairo's caches, the Win32 device, the DirectWrite singletons, the Direct2D factory of this thread and, with a pixman
that can, pixman's implementations are freed.

## More

- `doc/d2d/d3d11-backend.md`: the Direct3D 11 backend, the abstraction behind both backends, regenerating the shaders.
- `test/d2d/`: two standalone test programs, the comparison of both backends and the drop shadow.

## Licensing

Cairo is under the **LGPL 2.1 or the MPL 1.1**, at the recipient's choice (`COPYING`), and so are the changes of these
branches, the Direct2D and Direct3D backends included: they are derived from Mozilla's code under that license, whose
notices the files keep. The new files without such a notice carry Cairo's.
