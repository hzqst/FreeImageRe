// ==========================================================
// FreeImage implementation
//
// Design and implementation by
// - Floris van den Berg (flvdberg@wxs.nl)
// - Herve Drolon (drolon@infonie.fr)
// - Detlev Vendt (detlev.vendt@brillit.de)
// - Petr Supina (psup@centrum.cz)
// - Carsten Klein (c.klein@datagis.com)
// - Mihail Naydenov (mnaydenov@users.sourceforge.net)
//
// This file is part of FreeImage 3
//
// COVERED CODE IS PROVIDED UNDER THIS LICENSE ON AN "AS IS" BASIS, WITHOUT WARRANTY
// OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, WITHOUT LIMITATION, WARRANTIES
// THAT THE COVERED CODE IS FREE OF DEFECTS, MERCHANTABLE, FIT FOR A PARTICULAR PURPOSE
// OR NON-INFRINGING. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE COVERED
// CODE IS WITH YOU. SHOULD ANY COVERED CODE PROVE DEFECTIVE IN ANY RESPECT, YOU (NOT
// THE INITIAL DEVELOPER OR ANY OTHER CONTRIBUTOR) ASSUME THE COST OF ANY NECESSARY
// SERVICING, REPAIR OR CORRECTION. THIS DISCLAIMER OF WARRANTY CONSTITUTES AN ESSENTIAL
// PART OF THIS LICENSE. NO USE OF ANY COVERED CODE IS AUTHORIZED HEREUNDER EXCEPT UNDER
// THIS DISCLAIMER.
//
// Use at your own risk!
// ==========================================================

#ifdef _MSC_VER 
#pragma warning (disable : 4786) // identifier was truncated to 'number' characters
#endif 

#include <stdlib.h>
#if defined(_WIN32) || defined(_WIN64) || defined(__MINGW32__)
#include <malloc.h>
#endif // _WIN32 || _WIN64 || __MINGW32__

#include "FreeImage.h"
#include "FreeImageIO.h"
#include "Utilities.h"
#include "MapIntrospector.h"

#include "../Metadata/FreeImageTag.h"

/**
Constants for the BITMAPINFOHEADER::biCompression field
BI_RGB:
The bitmap is in uncompressed red green blue (RGB) format that is not compressed and does not use color masks.
BI_BITFIELDS:
The bitmap is not compressed and the color table consists of three uint32_t color masks that specify the red, green, and blue components, 
respectively, of each pixel. This is valid when used with 16 and 32-bits per pixel bitmaps.
*/
#ifndef _WINGDI_
#define BI_RGB       0L
#define BI_BITFIELDS 3L
#endif // _WINGDI_

// ----------------------------------------------------------
//  Metadata definitions
// ----------------------------------------------------------

/** helper for map<key, value> where value is a pointer to a FreeImage tag */
typedef std::map<std::string, FITAG*> TAGMAP;

/** helper for map<FREE_IMAGE_MDMODEL, TAGMAP*> */
typedef std::map<int, TAGMAP*> METADATAMAP;

/** helper for metadata iterator */
FI_STRUCT (METADATAHEADER) { 
	long pos;		//! current position when iterating the map
	TAGMAP *tagmap;	//! pointer to the tag map
};

// ----------------------------------------------------------
//  FIBITMAP definition
// ----------------------------------------------------------

/**
FreeImage header structure
*/
FI_STRUCT (FREEIMAGEHEADER) {
	/** data type - bitmap, array of long, double, complex, etc */
	FREE_IMAGE_TYPE type;

	/** background color used for RGB transparency */
	FIRGBA8 bkgnd_color;

	/**@name transparency management */
	//@{
	/**
	why another table ? for easy transparency table retrieval !
	transparency could be stored in the palette, which is better
	overall, but it requires quite some changes and it will render
	FreeImage_GetTransparencyTable obsolete in its current form;
	*/
	uint8_t transparent_table[256];
	/** number of transparent colors */
	int  transparency_count;
	/** TRUE if the image is transparent */
	FIBOOL transparent;
	//@}

	/** space to hold ICC profile */
	FIICCPROFILE iccProfile;

	/** contains a list of metadata models attached to the bitmap */
	METADATAMAP *metadata;

	/** FALSE if the FIBITMAP only contains the header and no pixel data */
	FIBOOL has_pixels;

	/** optionally contains a thumbnail attached to the bitmap */
	FIBITMAP *thumbnail;

	/**@name external pixel buffer management */
	//@{
	/** pointer to user provided pixels, NULL otherwise */
	uint8_t *external_bits;
	/** user provided pitch, 0 otherwise */
	unsigned external_pitch;
	//@}

	//uint8_t filler[1];			 // fill to 32-bit alignment
};

// ----------------------------------------------------------
//  FREEIMAGERGBMASKS definition
// ----------------------------------------------------------

/**
RGB mask structure - mainly used for 16-bit RGB555 / RGB 565 FIBITMAP
*/
FI_STRUCT (FREEIMAGERGBMASKS) {
	unsigned red_mask;		//! bit layout of the red components
	unsigned green_mask;	//! bit layout of the green components
	unsigned blue_mask;		//! bit layout of the blue components
};

// ----------------------------------------------------------
//  Memory allocation on a specified alignment boundary
// ----------------------------------------------------------

#if (defined(_WIN32) || defined(_WIN64)) && !defined(__MINGW32__)

void* FreeImage_Aligned_Malloc(size_t amount, size_t alignment) {
	assert(alignment == FIBITMAP_ALIGNMENT);
	return _aligned_malloc(amount, alignment);
}

void FreeImage_Aligned_Free(void* mem) {
	_aligned_free(mem);
}

#elif defined (__MINGW32__)

void* FreeImage_Aligned_Malloc(size_t amount, size_t alignment) {
	assert(alignment == FIBITMAP_ALIGNMENT);
	return __mingw_aligned_malloc (amount, alignment);
}

void FreeImage_Aligned_Free(void* mem) {
	__mingw_aligned_free (mem);
}

#else

void* FreeImage_Aligned_Malloc(size_t amount, size_t alignment) {
	assert(alignment == FIBITMAP_ALIGNMENT);
	/*
	In some rare situations, the malloc routines can return misaligned memory. 
	The routine FreeImage_Aligned_Malloc allocates a bit more memory to do
	aligned writes.  Normally, it *should* allocate "alignment" extra memory and then writes
	one dword back the true pointer.  But if the memory manager returns a
	misaligned block that is less than a dword from the next alignment, 
	then the writing back one dword will corrupt memory.

	For example, suppose that alignment is 16 and malloc returns the address 0xFFFF.

	16 - 0xFFFF % 16 + 0xFFFF = 16 - 15 + 0xFFFF = 0x10000.

	Now, you subtract one dword from that and write and that will corrupt memory.

	That's why the code below allocates *two* alignments instead of one. 
	*/
	void* mem_real = malloc(amount + 2 * alignment);
	if (!mem_real) return nullptr;
	char* mem_align = (char*)((unsigned long)(2 * alignment - (unsigned long)mem_real % (unsigned long)alignment) + (unsigned long)mem_real);
	*((long*)mem_align - 1) = (long)mem_real;
	return mem_align;
}

void FreeImage_Aligned_Free(void* mem) {
	free((void*)*((long*)mem - 1));
}

#endif // _WIN32 || _WIN64

// ----------------------------------------------------------
//  FIBITMAP memory management
// ----------------------------------------------------------

/**
Calculate the size of a FreeImage image. 
Align the palette and the pixels on a FIBITMAP_ALIGNMENT bytes alignment boundary.
This function includes a protection against malicious images, based on a KISS integer overflow detection mechanism. 

@param header_only If TRUE, calculate a 'header only' FIBITMAP size, otherwise calculate a full FIBITMAP size
@param width Image width
@param height Image height
@param bpp Number of bits-per-pixel
@param need_masks We only store the masks (and allocate memory for them) for 16-bit images of type FIT_BITMAP
@return Returns a size in uint8_t units
@see FreeImage_AllocateBitmap
*/
static size_t 
FreeImage_GetInternalImageSize(FIBOOL header_only, unsigned width, unsigned height, unsigned bpp, FIBOOL need_masks) {
	size_t dib_size = sizeof(FREEIMAGEHEADER);
	dib_size += (dib_size % FIBITMAP_ALIGNMENT ? FIBITMAP_ALIGNMENT - dib_size % FIBITMAP_ALIGNMENT : 0);
	dib_size += FIBITMAP_ALIGNMENT - sizeof(FIBITMAPINFOHEADER) % FIBITMAP_ALIGNMENT;
	dib_size += sizeof(FIBITMAPINFOHEADER);
	// palette is aligned on a 16 bytes boundary
	dib_size += sizeof(FIRGBA8) * CalculateUsedPaletteEntries(bpp);
	// we both add palette size and masks size if need_masks is true, since CalculateUsedPaletteEntries
	// always returns 0 if need_masks is true (which is only true for 16 bit images).
	dib_size += need_masks ? sizeof(uint32_t) * 3 : 0;
	dib_size += (dib_size % FIBITMAP_ALIGNMENT ? FIBITMAP_ALIGNMENT - dib_size % FIBITMAP_ALIGNMENT : 0);

	if (!header_only) {
		const size_t header_size = dib_size;

		// pixels are aligned on a 16 bytes boundary
		dib_size += (size_t)CalculatePitch(CalculateLine(width, bpp)) * (size_t)height;

		// check for possible malloc overflow using a KISS integer overflow detection mechanism
		{
			const double dPitch = floor( ((double)bpp * width + 31.0) / 32.0 ) * 4.0;
			const double dImageSize = (double)header_size + dPitch * height;
			if (dImageSize != (double)dib_size) {
				// here, we are sure to encounter a malloc overflow: try to avoid it ...
				return 0;
			}

			/*
			The following constant take into account the additionnal memory used by 
			aligned malloc functions as well as debug malloc functions. 
			It is supposed here that using a (8 * FIBITMAP_ALIGNMENT) risk margin will be enough
			for the target compiler. 
			*/
			const double FIBITMAP_MAX_MEMORY = (double)((size_t)-1) - 8 * FIBITMAP_ALIGNMENT;

			if (dImageSize > FIBITMAP_MAX_MEMORY) {
				// avoid possible overflow inside C allocation functions
				return 0;
			}
		}
	}

	return dib_size;
}

/**
Helper for 16-bit FIT_BITMAP
Returns a pointer to the bitmap's red-, green- and blue masks.
@param dib The bitmap to obtain masks from.
@return Returns a pointer to the bitmap's red-, green- and blue masks
or NULL, if no masks are present (e.g. for 24 bit images).
*/
static FREEIMAGERGBMASKS *
FreeImage_GetRGBMasks(FIBITMAP *dib) {
	return FreeImage_HasRGBMasks(dib) ? (FREEIMAGERGBMASKS *)(((uint8_t *)FreeImage_GetInfoHeader(dib)) + sizeof(FIBITMAPINFOHEADER)) : nullptr;
}

/**
Internal FIBITMAP allocation.

This function accepts (ext_bits, ext_pitch) arguments. If these are provided the FIBITMAP 
will be allocated as "header only", but bits and pitch will be stored within the FREEIMAGEHEADER 
and the resulting FIBITMAP will have pixels, i.e. HasPixels() will return TRUE.
- GetBits() and GetPitch return the correct values - either offsets or the stored values (user-provided bits and pitch).
- Clone() creates a new FIBITMAP with copy of the user pixel data.
- Unload's implementation does not need to change - it just release a "header only" dib.
Note that when using external data, the data does not need to have the same alignment as the default 4-byte alignment. 
This enables the possibility to access buffers with, for instance, stricter alignment,
like the ones used in low-level APIs like OpenCL or intrinsics.

@param header_only If TRUE, allocate a 'header only' FIBITMAP, otherwise allocate a full FIBITMAP
@param ext_bits Pointer to external user's pixel buffer if using wrapped buffer, NULL otherwise
@param ext_pitch Pointer to external user's pixel buffer pitch if using wrapped buffer, 0 otherwise
@param type Image type
@param width Image width
@param height Image height
@param bpp Number of bits per pixel
@param red_mask Image red mask 
@param green_mask Image green mask
@param blue_mask Image blue mask
@return Returns the allocated FIBITMAP if successful, returns NULL otherwise
*/
static FIBITMAP * 
FreeImage_AllocateBitmap(FIBOOL header_only, uint8_t *ext_bits, unsigned ext_pitch, FREE_IMAGE_TYPE type, int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {

	// check input variables
	width = abs(width);
	height = abs(height);
	if (!((width > 0) && (height > 0))) {
		return nullptr;
	}
	if (ext_bits) {
		if (ext_pitch == 0) {
			return nullptr;
		}
		assert(!header_only);
	}

	// we only store the masks (and allocate memory for them) for 16-bit images of type FIT_BITMAP
	FIBOOL need_masks = FALSE;

	// check pixel bit depth
	switch (type) {
		case FIT_BITMAP:
			switch (bpp) {
				case 1:
				case 2:
				case 4:
				case 8:
					break;
				case 16:
					need_masks = TRUE;
					break;
				case 24:
				case 32:
					break;
				default:
					bpp = 8;
					break;
			}
			break;
		case FIT_UINT16:
			bpp = 8 * sizeof(unsigned short);
			break;
		case FIT_INT16:
			bpp = 8 * sizeof(short);
			break;
		case FIT_UINT32:
			bpp = 8 * sizeof(uint32_t);
			break;
		case FIT_INT32:
			bpp = 8 * sizeof(int32_t);
			break;
		case FIT_FLOAT:
			bpp = 8 * sizeof(float);
			break;
		case FIT_DOUBLE:
			bpp = 8 * sizeof(double);
			break;
		case FIT_COMPLEXF:
			bpp = 8 * sizeof(FICOMPLEXF);
			break;
		case FIT_COMPLEX:
			bpp = 8 * sizeof(FICOMPLEX);
			break;
		case FIT_RGB16:
			bpp = 8 * sizeof(FIRGB16);
			break;
		case FIT_RGBA16:
			bpp = 8 * sizeof(FIRGBA16);
			break;
		case FIT_RGB32:
			bpp = 8 * sizeof(FIRGB32);
			break;
		case FIT_RGBA32:
			bpp = 8 * sizeof(FIRGBA32);
			break;
		case FIT_RGBF:
			bpp = 8 * sizeof(FIRGBF);
			break;
		case FIT_RGBAF:
			bpp = 8 * sizeof(FIRGBAF);
			break;
		default:
			return nullptr;
	}

	do {
		auto *bitmap = static_cast<FIBITMAP *>(malloc(sizeof(FIBITMAP)));

		if (!bitmap) {
			break;
		}
		std::unique_ptr<void, decltype(&free)> safeBitmap(bitmap, &free);

		// calculate the size of a FreeImage image
		// align the palette and the pixels on a FIBITMAP_ALIGNMENT bytes alignment boundary
		// palette is aligned on a 16 bytes boundary
		// pixels are aligned on a 16 bytes boundary

		// when using a user provided pixel buffer, force a 'header only' allocation

		size_t dib_size = FreeImage_GetInternalImageSize(header_only || ext_bits, width, height, bpp, need_masks);

		if (dib_size == 0) {
			// memory allocation will fail (probably a malloc overflow)
			break;
		}

		bitmap->data = static_cast<uint8_t *>(FreeImage_Aligned_Malloc(dib_size * sizeof(uint8_t), FIBITMAP_ALIGNMENT));

		if (!bitmap->data) {
			break;
		}
		std::unique_ptr<void, decltype(&FreeImage_Aligned_Free)> safeData(bitmap->data, &FreeImage_Aligned_Free);

		memset(bitmap->data, 0, dib_size);

		// write out the FREEIMAGEHEADER

		auto *fih = (FREEIMAGEHEADER *)bitmap->data;

		fih->type = type;

		fih->transparent = FALSE;
		fih->transparency_count = 0;
		memset(fih->transparent_table, 0xff, 256);

		fih->has_pixels = header_only ? FALSE : TRUE;

		// initialize FIICCPROFILE link

		FIICCPROFILE *iccProfile = FreeImage_GetICCProfile(bitmap);
		iccProfile->size = 0;
		iccProfile->data = 0;
		iccProfile->flags = 0;

		// initialize metadata models list

		fih->metadata = new(std::nothrow) METADATAMAP;

		if (!fih->metadata) {
			break;
		}

		// initialize attached thumbnail

		fih->thumbnail = nullptr;

		// store a pointer to user provided pixel buffer (if any)

		fih->external_bits = ext_bits;
		fih->external_pitch = ext_pitch;

		// write out the BITMAPINFOHEADER

		auto *bih   = FreeImage_GetInfoHeader(bitmap);
		bih->biSize             = sizeof(FIBITMAPINFOHEADER);
		bih->biWidth            = width;
		bih->biHeight           = height;
		bih->biPlanes           = 1;
		bih->biCompression      = need_masks ? BI_BITFIELDS : BI_RGB;
		bih->biBitCount         = (uint16_t)bpp;
		bih->biClrUsed          = CalculateUsedPaletteEntries(bpp);
		bih->biClrImportant     = bih->biClrUsed;
		bih->biXPelsPerMeter	= 2835;	// 72 dpi
		bih->biYPelsPerMeter	= 2835;	// 72 dpi

		if (bpp == 8) {
			// build a default greyscale palette (very useful for image processing)
			auto *pal = FreeImage_GetPalette(bitmap);
			for (int i = 0; i < 256; i++) {
				pal[i].red	= (uint8_t)i;
				pal[i].green = (uint8_t)i;
				pal[i].blue	= (uint8_t)i;
			}
			}

		// just setting the masks (only if needed) just like the palette.
		if (need_masks) {
			auto *masks = FreeImage_GetRGBMasks(bitmap);
			masks->red_mask = red_mask;
			masks->green_mask = green_mask;
			masks->blue_mask = blue_mask;
		}

		safeData.release();
		safeBitmap.release();
		return bitmap;
	}
	while (false);

	return nullptr;
}

FIBITMAP * DLL_CALLCONV
FreeImage_AllocateHeaderForBits(uint8_t *ext_bits, unsigned ext_pitch, FREE_IMAGE_TYPE type, int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
	return FreeImage_AllocateBitmap(FALSE, ext_bits, ext_pitch, type, width, height, bpp, red_mask, green_mask, blue_mask);
}

FIBITMAP * DLL_CALLCONV
FreeImage_AllocateHeaderT(FIBOOL header_only, FREE_IMAGE_TYPE type, int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
	return FreeImage_AllocateBitmap(header_only, nullptr, 0, type, width, height, bpp, red_mask, green_mask, blue_mask);
}

FIBITMAP * DLL_CALLCONV
FreeImage_AllocateHeader(FIBOOL header_only, int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
	return FreeImage_AllocateBitmap(header_only, nullptr, 0, FIT_BITMAP, width, height, bpp, red_mask, green_mask, blue_mask);
}

FIBITMAP * DLL_CALLCONV
FreeImage_Allocate(int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
	return FreeImage_AllocateBitmap(FALSE, nullptr, 0, FIT_BITMAP, width, height, bpp, red_mask, green_mask, blue_mask);
}

FIBITMAP * DLL_CALLCONV
FreeImage_AllocateT(FREE_IMAGE_TYPE type, int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
	return FreeImage_AllocateBitmap(FALSE, nullptr, 0, type, width, height, bpp, red_mask, green_mask, blue_mask);
}

void DLL_CALLCONV
FreeImage_Unload(FIBITMAP *dib) {
	if (dib) {	
		if (dib->data) {
			// delete possible icc profile ...
			if (FreeImage_GetICCProfile(dib)->data) {
				free(FreeImage_GetICCProfile(dib)->data);
			}

			// delete metadata models
			auto *metadata = ((FREEIMAGEHEADER *)dib->data)->metadata;

			for (auto &i : *metadata) {
				if (auto *tagmap = i.second) {
					for (auto &j : *tagmap) {
						FreeImage_DeleteTag(j.second);
					}

					delete tagmap;
				}
			}

			delete metadata;

			// delete embedded thumbnail
			FreeImage_Unload(FreeImage_GetThumbnail(dib));

			// delete bitmap ...
			FreeImage_Aligned_Free(dib->data);
		}

		free(dib);		// ... and the wrapper
	}
}

// ----------------------------------------------------------

FIBITMAP * DLL_CALLCONV
FreeImage_Clone(FIBITMAP *dib) {
	if (!dib) {
		return nullptr;
	}

	FREE_IMAGE_TYPE type = FreeImage_GetImageType(dib);
	unsigned width	= FreeImage_GetWidth(dib);
	unsigned height	= FreeImage_GetHeight(dib);
	unsigned bpp	= FreeImage_GetBPP(dib);

	// if the FIBITMAP is a wrapper to a user provided pixel buffer, get a pointer to this buffer
	const uint8_t *ext_bits = ((FREEIMAGEHEADER *)dib->data)->external_bits;
	
	// check for pixel availability ...
	FIBOOL header_only = FreeImage_HasPixels(dib) ? FALSE : TRUE;

	// check whether this image has masks defined ...
	FIBOOL need_masks = (bpp == 16 && type == FIT_BITMAP) ? TRUE : FALSE;

	// allocate a new dib
	FIBITMAP *new_dib = FreeImage_AllocateHeaderT(header_only, type, width, height, bpp,
			FreeImage_GetRedMask(dib), FreeImage_GetGreenMask(dib), FreeImage_GetBlueMask(dib));

	if (new_dib) {
		// save ICC profile links
		FIICCPROFILE *src_iccProfile = FreeImage_GetICCProfile(dib);
		FIICCPROFILE *dst_iccProfile = FreeImage_GetICCProfile(new_dib);

		// save metadata links
		auto *src_metadata = ((FREEIMAGEHEADER *)dib->data)->metadata;
		auto *dst_metadata = ((FREEIMAGEHEADER *)new_dib->data)->metadata;

		// calculate the size of the dst image
		// align the palette and the pixels on a FIBITMAP_ALIGNMENT bytes alignment boundary
		// palette is aligned on a 16 bytes boundary
		// pixels are aligned on a 16 bytes boundary
		
		// when using a user provided pixel buffer, force a 'header only' calculation		

		size_t dib_size = FreeImage_GetInternalImageSize(header_only || ext_bits, width, height, bpp, need_masks);

		// copy the bitmap + internal pointers (remember to restore new_dib internal pointers later)
		memcpy(new_dib->data, dib->data, dib_size);

		// reset ICC profile link for new_dib
		memset(dst_iccProfile, 0, sizeof(FIICCPROFILE));

		// restore metadata link for new_dib
		((FREEIMAGEHEADER *)new_dib->data)->metadata = dst_metadata;

		// reset thumbnail link for new_dib
		((FREEIMAGEHEADER *)new_dib->data)->thumbnail = nullptr;

		// reset external wrapped buffer link for new_dib
		((FREEIMAGEHEADER *)new_dib->data)->external_bits = nullptr;
		((FREEIMAGEHEADER *)new_dib->data)->external_pitch = 0;

		// copy possible ICC profile
		FreeImage_CreateICCProfile(new_dib, src_iccProfile->data, src_iccProfile->size);
		dst_iccProfile->flags = src_iccProfile->flags;

		// copy metadata models
		for (auto &i : *src_metadata) {
			int model = i.first;

			if (auto *src_tagmap = i.second) {
				// create a metadata model
				if (auto *dst_tagmap = new(std::nothrow) TAGMAP()) {
					// fill the model
					for (auto &j : *src_tagmap) {
						std::string dst_key = j.first;
						auto *dst_tag = FreeImage_CloneTag(j.second);

						// assign key and tag value
						(*dst_tagmap)[dst_key] = dst_tag;
					}

					// assign model and tagmap
					(*dst_metadata)[model] = dst_tagmap;
				}
			}
		}

		// copy the thumbnail
		FreeImage_SetThumbnail(new_dib, FreeImage_GetThumbnail(dib));

		// copy user provided pixel buffer (if any)
		if (ext_bits) {
			const unsigned pitch = FreeImage_GetPitch(dib);
			const unsigned linesize = FreeImage_GetLine(dib);
			for (unsigned y = 0; y < height; y++) {
				memcpy(FreeImage_GetScanLine(new_dib, y), ext_bits, linesize);
				ext_bits += pitch;
			}
		}

		return new_dib;
	}

	return nullptr;
}

// ----------------------------------------------------------

uint8_t * DLL_CALLCONV
FreeImage_GetBits(FIBITMAP *dib) {
	if (!FreeImage_HasPixels(dib)) {
		return nullptr;
	}

	if (((FREEIMAGEHEADER *)dib->data)->external_bits) {
		return ((FREEIMAGEHEADER *)dib->data)->external_bits;
	}

	// returns the pixels aligned on a FIBITMAP_ALIGNMENT bytes alignment boundary
	size_t lp = (size_t)FreeImage_GetInfoHeader(dib);
	lp += sizeof(FIBITMAPINFOHEADER) + sizeof(FIRGBA8) * FreeImage_GetColorsUsed(dib);
	lp += FreeImage_HasRGBMasks(dib) ? sizeof(uint32_t) * 3 : 0;
	lp += (lp % FIBITMAP_ALIGNMENT ? FIBITMAP_ALIGNMENT - lp % FIBITMAP_ALIGNMENT : 0);
	return (uint8_t *)lp;
}

// ----------------------------------------------------------
//  DIB information functions
// ----------------------------------------------------------

FIBITMAP* DLL_CALLCONV
FreeImage_GetThumbnail(FIBITMAP *dib) {
	return dib ? ((FREEIMAGEHEADER *)dib->data)->thumbnail : nullptr;
}

FIBOOL DLL_CALLCONV
FreeImage_SetThumbnail(FIBITMAP *dib, FIBITMAP *thumbnail) {
	if (!dib) {
		return FALSE;
	}
	FIBITMAP *currentThumbnail = FreeImage_GetThumbnail(dib);
	if (currentThumbnail == thumbnail) {
		return TRUE;
	}
	FreeImage_Unload(currentThumbnail);

	((FREEIMAGEHEADER *)dib->data)->thumbnail = FreeImage_HasPixels(thumbnail) ? FreeImage_Clone(thumbnail) : nullptr;

	return TRUE;
}

// ----------------------------------------------------------

FREE_IMAGE_COLOR_TYPE
FreeImage_GetColorType(FIBITMAP* dib) {
	return FreeImage_GetColorType2(dib, TRUE);
}

FREE_IMAGE_COLOR_TYPE
FreeImage_GetColorType2(FIBITMAP* dib, FIBOOL scan_alpha) {
	FIRGBA8 *rgb;

	const FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(dib);
	const FIICCPROFILE* icc_profile = FreeImage_GetICCProfile(dib);

	// special bitmap type
	if (image_type != FIT_BITMAP) {
		switch (image_type) {
			case FIT_UINT16:
			{
				// 16-bit greyscale TIF can be either FIC_MINISBLACK (the most common case) or FIC_MINISWHITE
				// you can check this using EXIF_MAIN metadata
				FITAG *photometricTag{};
				if (FreeImage_GetMetadata(FIMD_EXIF_MAIN, dib, "PhotometricInterpretation", &photometricTag)) {
					const short *value = (short*)FreeImage_GetTagValue(photometricTag);
					// PHOTOMETRIC_MINISWHITE = 0 => min value is white
					// PHOTOMETRIC_MINISBLACK = 1 => min value is black
					return (*value == 0) ? FIC_MINISWHITE : FIC_MINISBLACK;
				}
				return FIC_MINISBLACK;
			}
			break;

			case FIT_RGB32:
			case FIT_RGB16:
			case FIT_RGBF:
				if (icc_profile && ((icc_profile->flags & FIICC_COLOR_IS_YUV) == FIICC_COLOR_IS_YUV)) {
					return FIC_YUV;
				}
				return FIC_RGB;

			case FIT_RGBA32:
			case FIT_RGBA16:
			case FIT_RGBAF:
				if (icc_profile) {
					if ((icc_profile->flags & FIICC_COLOR_IS_CMYK) == FIICC_COLOR_IS_CMYK) {
						return FIC_CMYK;
					}
					if ((icc_profile->flags & FIICC_COLOR_IS_YUV) == FIICC_COLOR_IS_YUV) {
						return FIC_YUV;
					}
				}
				return FIC_RGBALPHA;
		}

		return FIC_MINISBLACK;
	}

	// standard image type
	switch (FreeImage_GetBPP(dib)) {
		case 1:
		{
			rgb = FreeImage_GetPalette(dib);

			if ((rgb->red == 0) && (rgb->green == 0) && (rgb->blue == 0)) {
				rgb++;

				if ((rgb->red == 255) && (rgb->green == 255) && (rgb->blue == 255)) {
					return FIC_MINISBLACK;
				}
			}

			if ((rgb->red == 255) && (rgb->green == 255) && (rgb->blue == 255)) {
				rgb++;

				if ((rgb->red == 0) && (rgb->green == 0) && (rgb->blue == 0)) {
					return FIC_MINISWHITE;
				}
			}

			return FIC_PALETTE;
		}

		case 4:
		case 8:	// Check if the DIB has a color or a greyscale palette
		{
			int ncolors = FreeImage_GetColorsUsed(dib);
		    int minisblack = 1;
			rgb = FreeImage_GetPalette(dib);

			for (int i = 0; i < ncolors; i++) {
				if ((rgb->red != rgb->green) || (rgb->red != rgb->blue)) {
					return FIC_PALETTE;
				}

				// The DIB has a color palette if the greyscale isn't a linear ramp
				// Take care of reversed grey images
				if (rgb->red != i) {
					if ((ncolors-i-1) != rgb->red) {
						return FIC_PALETTE;
					} else {
						minisblack = 0;
					}
				}

				rgb++;
			}

			return minisblack ? FIC_MINISBLACK : FIC_MINISWHITE;
		}

		case 16:
		case 24:
			if (icc_profile && ((icc_profile->flags & FIICC_COLOR_IS_YUV) == FIICC_COLOR_IS_YUV)) {
				return FIC_YUV;
			}
			return FIC_RGB;

		case 32:
		{
			if (icc_profile) {
				if ((icc_profile->flags & FIICC_COLOR_IS_CMYK) == FIICC_COLOR_IS_CMYK) {
					return FIC_CMYK;
				}
				if ((icc_profile->flags & FIICC_COLOR_IS_YUV) == FIICC_COLOR_IS_YUV) {
					return FIC_YUV;
				}
			}

			if (scan_alpha && FreeImage_HasPixels(dib)) {
				// check for fully opaque alpha layer
				for (unsigned y = 0; y < FreeImage_GetHeight(dib); y++) {
					rgb = (FIRGBA8 *)FreeImage_GetScanLine(dib, y);

					for (unsigned x = 0; x < FreeImage_GetWidth(dib); x++) {
						if (rgb[x].alpha != 0xFF) {
							return FIC_RGBALPHA;
						}
					}
				}
				return FIC_RGB;
			}

			return FIC_RGBALPHA;
		}
				
		default :
			return FIC_MINISBLACK;
	}
}

// ----------------------------------------------------------

FREE_IMAGE_TYPE DLL_CALLCONV 
FreeImage_GetImageType(FIBITMAP *dib) {
	return dib ? ((FREEIMAGEHEADER *)dib->data)->type : FIT_UNKNOWN;
}

// ----------------------------------------------------------

FIBOOL DLL_CALLCONV 
FreeImage_HasPixels(FIBITMAP *dib) {
	return dib ? ((FREEIMAGEHEADER *)dib->data)->has_pixels : FALSE;
}

// ----------------------------------------------------------

FIBOOL DLL_CALLCONV
FreeImage_HasRGBMasks(FIBITMAP *dib) {
	return (dib && FreeImage_GetInfoHeader(dib)->biCompression == BI_BITFIELDS) ? TRUE : FALSE;
}

unsigned DLL_CALLCONV
FreeImage_GetRedMask(FIBITMAP *dib) {
	const FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(dib);
	switch (image_type) {
		case FIT_BITMAP:
			// check for 16-bit RGB (565 or 555)
			if (auto *masks = FreeImage_GetRGBMasks(dib)) {
				return masks->red_mask;
			}
			return FreeImage_GetBPP(dib) >= 24 ? FI_RGBA_RED_MASK : 0;
		default:
			return 0;
	}
}

unsigned DLL_CALLCONV
FreeImage_GetGreenMask(FIBITMAP *dib) {
	const FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(dib);
	switch (image_type) {
		case FIT_BITMAP:
			// check for 16-bit RGB (565 or 555)
			if (auto *masks = FreeImage_GetRGBMasks(dib)) {
				return masks->green_mask;
			}
			return FreeImage_GetBPP(dib) >= 24 ? FI_RGBA_GREEN_MASK : 0;
		default:
			return 0;
	}
}

unsigned DLL_CALLCONV
FreeImage_GetBlueMask(FIBITMAP *dib) {
	const FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(dib);
	switch (image_type) {
		case FIT_BITMAP:
			// check for 16-bit RGB (565 or 555)
			if (auto *masks = FreeImage_GetRGBMasks(dib)) {
				return masks->blue_mask;
			}
			return FreeImage_GetBPP(dib) >= 24 ? FI_RGBA_BLUE_MASK : 0;
		default:
			return 0;
	}
}

// ----------------------------------------------------------

FIBOOL DLL_CALLCONV
FreeImage_HasBackgroundColor(FIBITMAP *dib) {
	if (dib) {
		FIRGBA8 *bkgnd_color = &((FREEIMAGEHEADER *)dib->data)->bkgnd_color;
		return (bkgnd_color->alpha != 0) ? TRUE : FALSE;
	}
	return FALSE;
}

FIBOOL DLL_CALLCONV
FreeImage_GetBackgroundColor(FIBITMAP *dib, FIRGBA8 *bkcolor) {
	if (dib && bkcolor) {
		if (FreeImage_HasBackgroundColor(dib)) {
			// get the background color
			FIRGBA8 *bkgnd_color = &((FREEIMAGEHEADER *)dib->data)->bkgnd_color;
			memcpy(bkcolor, bkgnd_color, sizeof(FIRGBA8));

			bkcolor->alpha = 0;

			// get the background index
			if (FreeImage_GetBPP(dib) == 8) {
				auto *pal = FreeImage_GetPalette(dib);
				for (unsigned i = 0; i < FreeImage_GetColorsUsed(dib); i++) {
					if (bkgnd_color->red == pal[i].red) {
						if (bkgnd_color->green == pal[i].green) {
							if (bkgnd_color->blue == pal[i].blue) {
								bkcolor->alpha = (uint8_t)i;
								break;
							}
						}
					}
				}
			}
			return TRUE;
		}
	}

	return FALSE;
}

FIBOOL DLL_CALLCONV 
FreeImage_SetBackgroundColor(FIBITMAP *dib, FIRGBA8 *bkcolor) {
	if (dib) {
		FIRGBA8 *bkgnd_color = &((FREEIMAGEHEADER *)dib->data)->bkgnd_color;
		if (bkcolor) {
			// set the background color
			memcpy(bkgnd_color, bkcolor, sizeof(FIRGBA8));
			// enable the file background color
			bkgnd_color->alpha = 1;
		} else {
			// clear and disable the file background color
			memset(bkgnd_color, 0, sizeof(FIRGBA8));
		}
		return TRUE;
	}

	return FALSE;
}

// ----------------------------------------------------------

FIBOOL DLL_CALLCONV
FreeImage_IsTransparent(FIBITMAP *dib) {
	if (dib) {
		const FREE_IMAGE_TYPE image_type = FreeImage_GetImageType(dib);
		switch (image_type) {
			case FIT_BITMAP:
				if (FreeImage_GetBPP(dib) == 32) {
					if (FreeImage_GetColorType(dib) == FIC_RGBALPHA) {
						return TRUE;
					}
				} else {
					return ((FREEIMAGEHEADER *)dib->data)->transparent ? TRUE : FALSE;
				}
				break;
			case FIT_RGBA16:
			case FIT_RGBAF:
				return (((FreeImage_GetICCProfile(dib)->flags) & FIICC_COLOR_IS_CMYK) == FIICC_COLOR_IS_CMYK) ? FALSE : TRUE;
			default:
				break;
		}
	}
	return FALSE;
}

uint8_t * DLL_CALLCONV
FreeImage_GetTransparencyTable(FIBITMAP *dib) {
	return dib ? ((FREEIMAGEHEADER *)dib->data)->transparent_table : nullptr;
}

void DLL_CALLCONV
FreeImage_SetTransparent(FIBITMAP *dib, FIBOOL enabled) {
	if (dib) {
		if ((FreeImage_GetBPP(dib) <= 8) || (FreeImage_GetBPP(dib) == 32)) {
			((FREEIMAGEHEADER *)dib->data)->transparent = enabled;
		} else {
			((FREEIMAGEHEADER *)dib->data)->transparent = FALSE;
		}
	}
}

unsigned DLL_CALLCONV
FreeImage_GetTransparencyCount(FIBITMAP *dib) {
	return dib ? ((FREEIMAGEHEADER *)dib->data)->transparency_count : 0;
}

void DLL_CALLCONV
FreeImage_SetTransparencyTable(FIBITMAP *dib, uint8_t *table, int count) {
	if (dib) {
		count = MAX(0, MIN(count, 256));
		if (FreeImage_GetBPP(dib) <= 8) {
			((FREEIMAGEHEADER *)dib->data)->transparent = (count > 0) ? TRUE : FALSE;
			((FREEIMAGEHEADER *)dib->data)->transparency_count = count;

			if (table) {
				memcpy(((FREEIMAGEHEADER *)dib->data)->transparent_table, table, count);
			} else {
				memset(((FREEIMAGEHEADER *)dib->data)->transparent_table, 0xff, count);
			}
		} 
	}
}

/** @brief Sets the index of the palette entry to be used as transparent color
 for the image specified. Does nothing on high color images. 
 
 This method sets the index of the palette entry to be used as single transparent
 color for the image specified. This works on palletised images only and does
 nothing for high color images.
 
 Although it is possible for palletised images to have more than one transparent
 color, this method sets the palette entry specified as the single transparent
 color for the image. All other colors will be set to be non-transparent by this
 method.
 
 As with FreeImage_SetTransparencyTable(), this method also sets the image's
 transparency property to TRUE (as it is set and obtained by
 FreeImage_SetTransparent() and FreeImage_IsTransparent() respectively) for
 palletised images.
 
 @param dib Input image, whose transparent color is to be set.
 @param index The index of the palette entry to be set as transparent color.
 */
void DLL_CALLCONV
FreeImage_SetTransparentIndex(FIBITMAP *dib, int index) {
	if (dib) {
		int count = FreeImage_GetColorsUsed(dib);
		if (count) {
			auto *new_tt = static_cast<uint8_t *>(malloc(count * sizeof(uint8_t)));
			memset(new_tt, 0xFF, count);
			if ((index >= 0) && (index < count)) {
				new_tt[index] = 0x00;
			}
			FreeImage_SetTransparencyTable(dib, new_tt, count);
			free(new_tt);
		}
	}
}

/** @brief Returns the palette entry used as transparent color for the image
 specified. Works for palletised images only and returns -1 for high color
 images or if the image has no color set to be transparent. 
 
 Although it is possible for palletised images to have more than one transparent
 color, this function always returns the index of the first palette entry, set
 to be transparent. 
 
 @param dib Input image, whose transparent color is to be returned.
 @return Returns the index of the palette entry used as transparent color for
 the image specified or -1 if there is no transparent color found (e.g. the image
 is a high color image).
 */
int DLL_CALLCONV
FreeImage_GetTransparentIndex(FIBITMAP *dib) {
	const int count = FreeImage_GetTransparencyCount(dib);
	const uint8_t *tt = FreeImage_GetTransparencyTable(dib);
	for (int i = 0; i < count; i++) {
		if (tt[i] == 0) {
			return i;
		}
	}
	return -1;
}

// ----------------------------------------------------------

FIICCPROFILE * DLL_CALLCONV
FreeImage_GetICCProfile(FIBITMAP *dib) {
	FIICCPROFILE *profile = (dib ? (FIICCPROFILE *)&((FREEIMAGEHEADER *)dib->data)->iccProfile : nullptr);
	return profile;
}

FIICCPROFILE * DLL_CALLCONV
FreeImage_CreateICCProfile(FIBITMAP *dib, void *data, long size) {
	// clear the profile but preserve profile->flags
	FreeImage_DestroyICCProfile(dib);
	// create the new profile
	FIICCPROFILE *profile = FreeImage_GetICCProfile(dib);
	if (size && profile) {
		assert(data);
		profile->data = malloc(size);
		if (profile->data) {
			memcpy(profile->data, data, profile->size = size);
		}
	}
	return profile;
}

void DLL_CALLCONV
FreeImage_DestroyICCProfile(FIBITMAP *dib) {
	FIICCPROFILE *profile = FreeImage_GetICCProfile(dib);
	if (profile) {
		if (profile->data) {
			free (profile->data);
		}
		// clear the profile but preserve profile->flags
		profile->data = nullptr;
		profile->size = 0;
	}

	// destroy also Exif-Main ICC profile
	FreeImage_SetMetadata(FIMD_EXIF_MAIN, dib, "InterColorProfile", nullptr);
}

// ----------------------------------------------------------

unsigned DLL_CALLCONV
FreeImage_GetWidth(FIBITMAP *dib) {
	return dib ? FreeImage_GetInfoHeader(dib)->biWidth : 0;
}

unsigned DLL_CALLCONV
FreeImage_GetHeight(FIBITMAP *dib) {
	return dib ? FreeImage_GetInfoHeader(dib)->biHeight : 0;
}

unsigned DLL_CALLCONV
FreeImage_GetBPP(FIBITMAP *dib) {
	return dib ? FreeImage_GetInfoHeader(dib)->biBitCount : 0;
}

unsigned DLL_CALLCONV
FreeImage_GetLine(FIBITMAP *dib) {
	return dib ? ((FreeImage_GetWidth(dib) * FreeImage_GetBPP(dib)) + 7) / 8 : 0;
}

unsigned DLL_CALLCONV
FreeImage_GetPitch(FIBITMAP *dib) {
	if (dib) {
		FREEIMAGEHEADER *fih = (FREEIMAGEHEADER *)dib->data;
		return fih->external_bits ? fih->external_pitch : (FreeImage_GetLine(dib) + 3 & ~3);
	}
	return 0;
}

unsigned DLL_CALLCONV
FreeImage_GetColorsUsed(FIBITMAP *dib) {
	return dib ? FreeImage_GetInfoHeader(dib)->biClrUsed : 0;
}

unsigned DLL_CALLCONV
FreeImage_GetDIBSize(FIBITMAP *dib) {
	return dib ? sizeof(FIBITMAPINFOHEADER) + (FreeImage_GetColorsUsed(dib) * sizeof(FIRGBA8)) + (FreeImage_GetPitch(dib) * FreeImage_GetHeight(dib)) : 0;
}

FIRGBA8 * DLL_CALLCONV
FreeImage_GetPalette(FIBITMAP *dib) {
	return (dib && FreeImage_GetBPP(dib) < 16) ? (FIRGBA8 *)(((uint8_t *)FreeImage_GetInfoHeader(dib)) + sizeof(FIBITMAPINFOHEADER)) : nullptr;
}

unsigned DLL_CALLCONV
FreeImage_GetDotsPerMeterX(FIBITMAP *dib) {
	return dib ? FreeImage_GetInfoHeader(dib)->biXPelsPerMeter : 0;
}

unsigned DLL_CALLCONV
FreeImage_GetDotsPerMeterY(FIBITMAP *dib) {
	return dib ? FreeImage_GetInfoHeader(dib)->biYPelsPerMeter : 0;
}

void DLL_CALLCONV
FreeImage_SetDotsPerMeterX(FIBITMAP *dib, unsigned res) {
	if (dib) {
		FreeImage_GetInfoHeader(dib)->biXPelsPerMeter = res;
	}
}

void DLL_CALLCONV
FreeImage_SetDotsPerMeterY(FIBITMAP *dib, unsigned res) {
	if (dib) {
		FreeImage_GetInfoHeader(dib)->biYPelsPerMeter = res;
	}
}

FIBITMAPINFOHEADER * DLL_CALLCONV
FreeImage_GetInfoHeader(FIBITMAP *dib) {
	if (!dib) {
		return nullptr;
	}
	size_t lp = (size_t)dib->data + sizeof(FREEIMAGEHEADER);
	lp += (lp % FIBITMAP_ALIGNMENT ? FIBITMAP_ALIGNMENT - lp % FIBITMAP_ALIGNMENT : 0);
	lp += FIBITMAP_ALIGNMENT - sizeof(FIBITMAPINFOHEADER) % FIBITMAP_ALIGNMENT;
	return (FIBITMAPINFOHEADER *)lp;
}

FIBITMAPINFO * DLL_CALLCONV
FreeImage_GetInfo(FIBITMAP *dib) {
	return (FIBITMAPINFO *)FreeImage_GetInfoHeader(dib);
}

unsigned DLL_CALLCONV
FreeImage_GetChannelsNumber(FIBITMAP* dib) {
	if (!dib) {
		return 0;
	}
	switch (FreeImage_GetImageType(dib)) {
	case FIT_BITMAP:
		switch (FreeImage_GetBPP(dib)) {
		case 32:
		case 64:
		case 128:
			return 4;
		case 24:
		case 48:
		case 96:
			return 3;
		default:
			return 1;
		}

	case FIT_RGB32:
	case FIT_RGB16:
	case FIT_RGBF:
		return 3;

	case FIT_RGBA32:
	case FIT_RGBA16:
	case FIT_RGBAF:
		return 4;

	case FIT_COMPLEXF:
	case FIT_COMPLEX:
		return 2;

	default:
		return 1;
	}
}

// ----------------------------------------------------------
//  Metadata routines
// ----------------------------------------------------------

FIMETADATA * DLL_CALLCONV 
FreeImage_FindFirstMetadata(FREE_IMAGE_MDMODEL model, FIBITMAP *dib, FITAG **tag) {
	if (!dib) {
		return nullptr;
	}

	// get the metadata model
	auto *metadata = ((FREEIMAGEHEADER *)dib->data)->metadata;
	TAGMAP *tagmap{};
	if (auto it{ metadata->find(model) }; it != metadata->end()) {
		tagmap = it->second;
	}
	if (tagmap) {
		// allocate a handle
		if (auto *handle = (FIMETADATA *)malloc(sizeof(FIMETADATA))) {
			// calculate the size of a METADATAHEADER
			const size_t header_size = sizeof(METADATAHEADER);

			if (handle->data = (uint8_t *)malloc(header_size)) {
				memset(handle->data, 0, header_size);

				// write out the METADATAHEADER
				METADATAHEADER *mdh = (METADATAHEADER *)handle->data;

				mdh->pos = 1;
				mdh->tagmap = tagmap;

				// get the first element
				TAGMAP::iterator i = tagmap->begin();
				*tag = i->second;

				return handle;
			}

			free(handle);
		}
	}

	return nullptr;
}

FIBOOL DLL_CALLCONV 
FreeImage_FindNextMetadata(FIMETADATA *mdhandle, FITAG **tag) {
	if (!mdhandle) {
		return FALSE;
	}

	METADATAHEADER *mdh = (METADATAHEADER *)mdhandle->data;
	TAGMAP *tagmap = mdh->tagmap;

	int current_pos = mdh->pos;
	int mapsize     = (int)tagmap->size();

	if (current_pos < mapsize) {
		// get the tag element at position pos

		auto i{ std::next(tagmap->begin(), current_pos) };
		*tag = i->second;
		mdh->pos++;
		
		return TRUE;
	}

	return FALSE;
}

void DLL_CALLCONV 
FreeImage_FindCloseMetadata(FIMETADATA *mdhandle) {
	if (mdhandle) {	// delete the handle
		free(mdhandle->data);
		free(mdhandle);		// ... and the wrapper
	}
}


// ----------------------------------------------------------

FIBOOL DLL_CALLCONV
FreeImage_CloneMetadata(FIBITMAP *dst, FIBITMAP *src) {
	if (!src || !dst) {
		return FALSE;
	}

	// get metadata links
	auto *src_metadata = ((FREEIMAGEHEADER *)src->data)->metadata;
	auto *dst_metadata = ((FREEIMAGEHEADER *)dst->data)->metadata;

	// copy metadata models, *except* the FIMD_ANIMATION model
	for (auto &i : *src_metadata) {
		int model = i.first;
		if (model == (int)FIMD_ANIMATION) {
			continue;
		}

		if (auto *src_tagmap = i.second) {
			if (dst_metadata->find(model) != dst_metadata->end()) {
				// destroy dst model
				FreeImage_SetMetadata((FREE_IMAGE_MDMODEL)model, dst, nullptr, nullptr);
			}

			// create a metadata model
			if (auto *dst_tagmap = new(std::nothrow) TAGMAP()) {
				// fill the model
				for (auto &j : *src_tagmap) {
					std::string dst_key = j.first;
					auto *dst_tag = FreeImage_CloneTag(j.second);

					// assign key and tag value
					(*dst_tagmap)[dst_key] = dst_tag;
				}

				// assign model and tagmap
				(*dst_metadata)[model] = dst_tagmap;
			}
		}
	}

	// clone resolution 
	FreeImage_SetDotsPerMeterX(dst, FreeImage_GetDotsPerMeterX(src)); 
	FreeImage_SetDotsPerMeterY(dst, FreeImage_GetDotsPerMeterY(src)); 

	return TRUE;
}

// ----------------------------------------------------------

FIBOOL DLL_CALLCONV 
FreeImage_SetMetadata(FREE_IMAGE_MDMODEL model, FIBITMAP *dib, const char *key, FITAG *tag) {
	if (!dib) {
		return FALSE;
	}

	TAGMAP *tagmap{};

	// get the metadata model
	auto *metadata = ((FREEIMAGEHEADER *)dib->data)->metadata;
	auto model_iterator = metadata->find(model);
	if (model_iterator != metadata->end()) {
		tagmap = model_iterator->second;
	}

	if (key) {

		if (!tag && !tagmap) {
			// remove a tag from an unknown tagmap, nothing to do
			return TRUE;
		}

		if (!tagmap) {
			// this model, doesn't exist: create it 
			tagmap = new(std::nothrow) TAGMAP();
			(*metadata)[model] = tagmap;
		}
		
		if (tag) {
			// first check the tag
			if (!FreeImage_GetTagKey(tag)) {
				FreeImage_SetTagKey(tag, key);
			} else if (strcmp(key, FreeImage_GetTagKey(tag)) != 0) {
				// set the tag key
				FreeImage_SetTagKey(tag, key);
			}
			if (FreeImage_GetTagCount(tag) * FreeImage_TagDataWidth(FreeImage_GetTagType(tag)) != FreeImage_GetTagLength(tag)) {
				FreeImage_OutputMessageProc(FIF_UNKNOWN, "Invalid data count for tag '%s'", key);
				return FALSE;
			}

			// fill the tag ID if possible and if it's needed
			const TagLib& tag_lib = TagLib::instance();
			switch (model) {
				case FIMD_IPTC:
				{
					int id = tag_lib.getTagID(TagLib::IPTC, key);
					/*
					if (id == -1) {
						FreeImage_OutputMessageProc(FIF_UNKNOWN, "IPTC: Invalid key '%s'", key);
					}
					*/
					FreeImage_SetTagID(tag, (uint16_t)id);
				}
				break;

				default:
					break;
			}

			// delete existing tag
			if (auto *old_tag = (*tagmap)[key]) {
				FreeImage_DeleteTag(old_tag);
			}

			// create a new tag
			(*tagmap)[key] = FreeImage_CloneTag(tag);
		}
		else {
			// delete existing tag
			if (auto i{ tagmap->find(key) }; i != tagmap->end()) {
				FreeImage_DeleteTag(i->second);
				tagmap->erase(i);
			}
		}
	}
	else {
		// destroy the metadata model
		if (tagmap) {
			for (auto &e : *tagmap) {
				FreeImage_DeleteTag(e.second);
			}

			delete tagmap;
			metadata->erase(model_iterator);
		}
	}

	return TRUE;
}

FIBOOL DLL_CALLCONV 
FreeImage_GetMetadata(FREE_IMAGE_MDMODEL model, FIBITMAP *dib, const char *key, FITAG **tag) {
	if (!dib || !key || !tag) {
		return FALSE;
	}

	*tag = nullptr;

	// get the metadata model
	auto *metadata = ((FREEIMAGEHEADER *)dib->data)->metadata;
	if (!metadata->empty()) {
		if (auto model_iterator{ metadata->find(model) }; model_iterator != metadata->end() ) {
			// this model exists : try to get the requested tag
			auto *tagmap = model_iterator->second;
			if (auto tag_iterator = tagmap->find(key); tag_iterator != tagmap->end() ) {
				// get the requested tag
				*tag = tag_iterator->second;
			} 
		}
	}

	return (*tag) ? TRUE : FALSE;
}

/**
Build and set a FITAG whose type is FIDT_ASCII. 
@param model Metadata model to be filled
@param dib Image to be filled
@param key Tag key
@param value Tag value as a ASCII string
@return Returns TRUE if successful, returns FALSE otherwise
*/
FIBOOL DLL_CALLCONV 
FreeImage_SetMetadataKeyValue(FREE_IMAGE_MDMODEL model, FIBITMAP *dib, const char *key, const char *value) {
	if (!dib || !key || !value) {
		return FALSE;
	}
	// create a tag
	if (auto *tag = FreeImage_CreateTag()) {
		// fill the tag
		auto tag_length = (uint32_t)(strlen(value) + 1);
		bool bSuccess = FreeImage_SetTagKey(tag, key);
		bSuccess = bSuccess && FreeImage_SetTagLength(tag, tag_length);
		bSuccess = bSuccess && FreeImage_SetTagCount(tag, tag_length);
		bSuccess = bSuccess && FreeImage_SetTagType(tag, FIDT_ASCII);
		bSuccess = bSuccess && FreeImage_SetTagValue(tag, value);
		// set the tag
		bSuccess = bSuccess && FreeImage_SetMetadata(model, dib, FreeImage_GetTagKey(tag), tag);
		// delete the tag
		FreeImage_DeleteTag(tag);

		return bSuccess ? TRUE : FALSE;
	}

	return FALSE;
}

// ----------------------------------------------------------

unsigned DLL_CALLCONV 
FreeImage_GetMetadataCount(FREE_IMAGE_MDMODEL model, FIBITMAP *dib) {
	if (!dib) {
		return FALSE;
	}

	// get the metadata model
	auto *metadata = ((FREEIMAGEHEADER *)dib->data)->metadata;
	if (auto it{ metadata->find(model) }; it != metadata->end()) {
		if (auto *tagmap = it->second) {
			// get the tag count
			return (unsigned)tagmap->size();
		}
	}

	// this model, doesn't exist: return
	return 0;
}

// ----------------------------------------------------------

unsigned DLL_CALLCONV
FreeImage_GetMemorySize(FIBITMAP *dib) {
	if (!dib) {
		return 0;
	}
	auto *header = (FREEIMAGEHEADER *)dib->data;
	auto *bih = FreeImage_GetInfoHeader(dib);

	FIBOOL header_only = !header->has_pixels || header->external_bits;
	FIBOOL need_masks = bih->biCompression == BI_BITFIELDS;
	unsigned width = bih->biWidth;
	unsigned height = bih->biHeight;
	unsigned bpp = bih->biBitCount;
	
	// start off with the size of the FIBITMAP structure
	size_t size = sizeof(FIBITMAP);
	
	// add sizes of FREEIMAGEHEADER, BITMAPINFOHEADER, palette and DIB data
	size += FreeImage_GetInternalImageSize(header_only, width, height, bpp, need_masks);

	// add ICC profile size
	size += header->iccProfile.size;

	// add thumbnail image size
	if (header->thumbnail) {
		// we assume a thumbnail not having a thumbnail as well, 
		// so this recursive call should not create an infinite loop
		size += FreeImage_GetMemorySize(header->thumbnail);
	}

	// add metadata size
	auto *md = header->metadata;
	if (!md) {
		return (unsigned)size;
	}

	// add size of METADATAMAP
	size += sizeof(METADATAMAP);

	const size_t models = md->size();
	if (models == 0) {
		return (unsigned)size;
	}

	unsigned tags = 0;

	for (auto &i : *md) {
		if (auto *tm = i.second) {
			for (auto &j : *tm) {
				++tags;
				const std::string & key = j.first;
				size += key.capacity();
				size += FreeImage_GetTagMemorySize(j.second);
			}
		}
	}

	// add size of all TAGMAP instances
	size += models * sizeof(TAGMAP);
	// add size of tree nodes in METADATAMAP
	size += MapIntrospector<METADATAMAP>::GetNodesMemorySize(models);
	// add size of tree nodes in TAGMAP
	size += MapIntrospector<TAGMAP>::GetNodesMemorySize(tags);

	return (unsigned)size;
}

