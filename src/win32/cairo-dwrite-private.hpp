/* -*- Mode: c; tab-width: 8; c-basic-offset: 4; indent-tabs-mode: t; -*- */
/* Cairo - a vector graphics library with display and print output
 *
 * Copyright © 2010 Mozilla Foundation
 *
 * This library is free software; you can redistribute it and/or
 * modify it either under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * (the "LGPL") or, at your option, under the terms of the Mozilla
 * Public License Version 1.1 (the "MPL"). If you do not alter this
 * notice, a recipient may use your version of this file under either
 * the MPL or the LGPL.
 *
 * You should have received a copy of the LGPL along with this library
 * in the file COPYING-LGPL-2.1; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * You should have received a copy of the MPL along with this library
 * in the file COPYING-MPL-1.1
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY
 * OF ANY KIND, either express or implied. See the LGPL or the MPL for
 * the specific language governing rights and limitations.
 *
 * The Original Code is the cairo graphics library.
 *
 * The Initial Developer of the Original Code is the Mozilla Foundation
 *
 * Contributor(s):
 *	Bas Schouten <bschouten@mozilla.com>
 */

#include "cairoint.h"
#include "cairo-win32-refptr.hpp"
#include "dwrite-extra.hpp"
#include "d2d1-extra.hpp"

#include "d2d/cairo-d2d-private.h"

/* #cairo_scaled_font_t implementation */
struct _cairo_dwrite_scaled_font {
    cairo_scaled_font_t base;
    cairo_matrix_t mat;
    cairo_matrix_t mat_inverse;
    cairo_antialias_t antialias_mode;
    IDWriteFontFace *dwriteface; /* Can't use RefPtr because this struct is malloc'd.  */
    IDWriteRenderingParams *rendering_params; /* Can't use RefPtr because this struct is malloc'd.  */
    DWRITE_MEASURING_MODE measuring_mode;
    cairo_bool_t manual_show_glyphs_allowed;
    cairo_d2d_surface_t::TextRenderingState rendering_mode;
};
typedef struct _cairo_dwrite_scaled_font cairo_dwrite_scaled_font_t;

class DWriteFactory
{
public:
    static IDWriteFactory *
    Instance()
    {
        InitializeFactories();
        return mFactoryInstance;
    }

    static IDWriteFactory1 *
    Instance1()
    {
        InitializeFactories();
        return mFactoryInstance1;
    }

    static IDWriteFactory2 *
    Instance2()
    {
        InitializeFactories();
        return mFactoryInstance2;
    }

    static IDWriteFactory3 *
    Instance3()
    {
        InitializeFactories();
        return mFactoryInstance3;
    }

    static IDWriteFactory4 *
    Instance4()
    {
        InitializeFactories();
        return mFactoryInstance4;
    }

    static IDWriteFactory8 *
    Instance8()
    {
        InitializeFactories();
        return mFactoryInstance8;
    }

    static IDWriteFontCollection *
    SystemCollection()
    {
        /* The system font collection obtained from the shared factory
         * is a singleton object. This means that we can cache it
         * globally and use from any thread.
         */

        if (_cairo_atomic_init_once_enter (&mOnceSystemCollection)) {
            HRESULT hr = Instance()->GetSystemFontCollection(&mSystemCollection);
            assert(SUCCEEDED(hr));

            _cairo_atomic_init_once_leave (&mOnceSystemCollection);
        }
        return mSystemCollection;
    }

    static RefPtr<IDWriteFontFamily>
    FindSystemFontFamily(const WCHAR *aFamilyName)
    {
	UINT32 idx;
	BOOL found;

	SystemCollection()->FindFamilyName(aFamilyName, &idx, &found);
	if (!found) {
	    return NULL;
	}

	RefPtr<IDWriteFontFamily> family;
	SystemCollection()->GetFontFamily(idx, &family);
	return family;
    }

    /* The rendering parameters of the Direct2D surface: the default ones and two
     * built from them with the values cairo_dwrite_set_cleartype_params() set.
     * The returned reference is the caller's.
     */
    static RefPtr<IDWriteRenderingParams>
    RenderingParams(cairo_d2d_surface_t::TextRenderingState mode)
    {
        RefPtr<IDWriteRenderingParams> params;

        AcquireSRWLockExclusive (&mRenderingParamsLock);
        if (!mDefaultRenderingParams) {
            CreateRenderingParams();
        }
        if (mode == cairo_d2d_surface_t::TEXT_RENDERING_NO_CLEARTYPE) {
            params = mDefaultRenderingParams;
        } else if (mode == cairo_d2d_surface_t::TEXT_RENDERING_GDI_CLASSIC && mRenderingMode < 0) {
            params = mForceGDIClassicRenderingParams;
        } else {
            params = mCustomClearTypeRenderingParams;
        }
        ReleaseSRWLockExclusive (&mRenderingParamsLock);
        return params;
    }

    static void
    SetRenderingParams(FLOAT aGamma,
                       FLOAT aEnhancedContrast,
                       FLOAT aClearTypeLevel,
                       int aPixelGeometry,
                       int aRenderingMode)
    {
        AcquireSRWLockExclusive (&mRenderingParamsLock);
        mGamma = aGamma;
        mEnhancedContrast = aEnhancedContrast;
        mClearTypeLevel = aClearTypeLevel;
        mPixelGeometry = aPixelGeometry;
        mRenderingMode = aRenderingMode;
        /* discard the current parameters: the next RenderingParams() makes them anew */
        ReleaseRenderingParams();
        ReleaseSRWLockExclusive (&mRenderingParamsLock);
    }

    static int
    GetClearTypeRenderingMode()
    {
        return mRenderingMode;
    }

    /* cairo_debug_reset_static_data(): releases the factories, the system
     * collection and the rendering parameters at once; the next use creates
     * them anew. Nothing of cairo may be in use.
     */
    static void
    Reset()
    {
        static const cairo_atomic_once_t once_init = CAIRO_ATOMIC_ONCE_INIT;

        AcquireSRWLockExclusive (&mRenderingParamsLock);
        ReleaseRenderingParams();
        ReleaseSRWLockExclusive (&mRenderingParamsLock);

        if (_cairo_atomic_init_once_check (&mOnceSystemCollection)) {
            ReleaseInterface (mSystemCollection);
            mOnceSystemCollection = once_init;
        }

        if (_cairo_atomic_init_once_check (&mOnceFactories)) {
            ReleaseInterface (mFactoryInstance);
            ReleaseInterface (mFactoryInstance1);
            ReleaseInterface (mFactoryInstance2);
            ReleaseInterface (mFactoryInstance3);
            ReleaseInterface (mFactoryInstance4);
            ReleaseInterface (mFactoryInstance8);
            mOnceFactories = once_init;
        }
    }

    static void
    Finalize()
    {
        /* Loader-lock-safe */

        cairo_win32_async_com_release (mDefaultRenderingParams);
        cairo_win32_async_com_release (mCustomClearTypeRenderingParams);
        cairo_win32_async_com_release (mForceGDIClassicRenderingParams);

        if (_cairo_atomic_init_once_check (&mOnceSystemCollection)) {
            cairo_win32_async_com_release (mSystemCollection);
        }

        if (_cairo_atomic_init_once_check (&mOnceFactories)) {
            cairo_win32_async_com_release (mFactoryInstance);
            cairo_win32_async_com_release (mFactoryInstance1);
            cairo_win32_async_com_release (mFactoryInstance2);
            cairo_win32_async_com_release (mFactoryInstance3);
            cairo_win32_async_com_release (mFactoryInstance4);
            cairo_win32_async_com_release (mFactoryInstance8);
        }
    }

private:
    static void
    InitializeFactories()
    {
        /* The shared IDWriteFactory is a singleton object (every call to
         * DWriteCreateFactory returns the same object) and thus is safe
         * for concurrent access.
         */

        if (_cairo_atomic_init_once_enter (&mOnceFactories)) {
            typedef HRESULT
            (WINAPI *pDWriteCreateFactory_t) (DWRITE_FACTORY_TYPE factoryType,
                                              REFIID iid,
                                              IUnknown **factory);

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
            HMODULE dwrite = _cairo_win32_load_library_from_system32 (L"dwrite.dll");
            pDWriteCreateFactory_t pDWriteCreateFactory = (pDWriteCreateFactory_t)
                GetProcAddress (dwrite, "DWriteCreateFactory");
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

            /* DWrite is based on nano-COM, which is COM as binary interface
             * and call convention, but doesn't need the runtime library or
             * registered informations. There's no need to enter a COM
             * apartment here.
             */
            HRESULT hr = pDWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED,
                                               __uuidof (IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(&mFactoryInstance));
            assert(SUCCEEDED(hr));

            mFactoryInstance->QueryInterface(&mFactoryInstance1);
            mFactoryInstance->QueryInterface(&mFactoryInstance2);
            mFactoryInstance->QueryInterface(&mFactoryInstance3);
            mFactoryInstance->QueryInterface(&mFactoryInstance4);
            mFactoryInstance->QueryInterface(&mFactoryInstance8);

            _cairo_atomic_init_once_leave (&mOnceFactories);
        }
    }

    template<typename T>
    static void
    ReleaseInterface(T *&iface)
    {
        if (iface) {
            iface->Release();
            iface = NULL;
        }
    }

    /* under mRenderingParamsLock */
    static void CreateRenderingParams();

    static void
    ReleaseRenderingParams()
    {
        ReleaseInterface (mDefaultRenderingParams);
        ReleaseInterface (mCustomClearTypeRenderingParams);
        ReleaseInterface (mForceGDIClassicRenderingParams);
    }

private:
    static cairo_atomic_once_t mOnceFactories;
    static IDWriteFactory *mFactoryInstance;
    static IDWriteFactory1 *mFactoryInstance1;
    static IDWriteFactory2 *mFactoryInstance2;
    static IDWriteFactory3 *mFactoryInstance3;
    static IDWriteFactory4 *mFactoryInstance4;
    static IDWriteFactory8 *mFactoryInstance8;

    static cairo_atomic_once_t mOnceSystemCollection;
    static IDWriteFontCollection *mSystemCollection;

    static SRWLOCK mRenderingParamsLock;
    static IDWriteRenderingParams *mDefaultRenderingParams;
    static IDWriteRenderingParams *mCustomClearTypeRenderingParams;
    static IDWriteRenderingParams *mForceGDIClassicRenderingParams;
    static FLOAT mGamma; // [1.0-2.0]
    static FLOAT mEnhancedContrast; // [0.0-10.0]
    static FLOAT mClearTypeLevel; // [0.0-1.0]
    static int mPixelGeometry; // [0-2]
    static int mRenderingMode; // [0-5]
};

class AutoDWriteGlyphRun : public DWRITE_GLYPH_RUN
{
    static const int kNumAutoGlyphs = 256;

public:
    AutoDWriteGlyphRun() {
        glyphCount = 0;
    }

    ~AutoDWriteGlyphRun() {
        if (glyphCount > kNumAutoGlyphs) {
            delete[] glyphIndices;
            delete[] glyphAdvances;
            delete[] glyphOffsets;
        }
    }

    void allocate(int aNumGlyphs) {
        glyphCount = aNumGlyphs;
        if (aNumGlyphs <= kNumAutoGlyphs) {
            glyphIndices = &mAutoIndices[0];
            glyphAdvances = &mAutoAdvances[0];
            glyphOffsets = &mAutoOffsets[0];
        } else {
            glyphIndices = new UINT16[aNumGlyphs];
            glyphAdvances = new FLOAT[aNumGlyphs];
            glyphOffsets = new DWRITE_GLYPH_OFFSET[aNumGlyphs];
        }
    }

private:
    DWRITE_GLYPH_OFFSET mAutoOffsets[kNumAutoGlyphs];
    FLOAT               mAutoAdvances[kNumAutoGlyphs];
    UINT16              mAutoIndices[kNumAutoGlyphs];
};

/* #cairo_font_face_t implementation */
struct _cairo_dwrite_font_face {
    cairo_font_face_t base;
    IDWriteFontFace *dwriteface; /* Can't use RefPtr because this struct is malloc'd.  */
    cairo_bool_t have_color;
    IDWriteRenderingParams *rendering_params; /* Can't use RefPtr because this struct is malloc'd.  */
    DWRITE_MEASURING_MODE measuring_mode;
};
typedef struct _cairo_dwrite_font_face cairo_dwrite_font_face_t;

DWRITE_MATRIX _cairo_dwrite_matrix_from_matrix(const cairo_matrix_t* matrix);

// This will initialize a DWrite glyph run from cairo glyphs and a scaled_font.
void
_cairo_dwrite_glyph_run_from_glyphs(cairo_glyph_t* glyphs,
    int num_glyphs,
    cairo_dwrite_scaled_font_t* scaled_font,
    AutoDWriteGlyphRun* run,
    cairo_bool_t* transformed);
