/**
 * Copyright (c) 2006-2017 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#include "Image.h"
#include "Graphics.h"

// C++
#include <algorithm>

namespace love
{
namespace graphics
{

love::Type Image::type("Image", &Texture::type);

int Image::imageCount = 0;

Image::Image(const Slices &data, const Settings &settings, bool validatedata)
	: Texture(data.getTextureType())
	, settings(settings)
	, data(data)
	, mipmapsType(settings.mipmaps ? MIPMAPS_GENERATED : MIPMAPS_NONE)
	, sRGB(isGammaCorrect() && !settings.linear)
	, usingDefaultTexture(false)
	, textureMemorySize(0)
{
	if (validatedata && data.validate() == MIPMAPS_DATA)
		mipmapsType = MIPMAPS_DATA;

	++imageCount;
}

Image::Image(TextureType textype, PixelFormat format, int width, int height, int slices, const Settings &settings)
	: Image(Slices(textype), settings, false)
{
	if (isPixelFormatCompressed(format))
		throw love::Exception("This constructor is only supported for non-compressed pixel formats.");

	if (textype == TEXTURE_2D_ARRAY)
		layers = slices;
	else if (textype == TEXTURE_VOLUME)
		depth = slices;

	init(format, width, height, settings);
}

Image::Image(const Slices &slices, const Settings &settings)
	: Image(slices, settings, true)
{
	if (texType == TEXTURE_2D_ARRAY)
		this->layers = data.getSliceCount();
	else if (texType == TEXTURE_VOLUME)
		this->depth = data.getSliceCount();

	love::image::ImageDataBase *slice = data.get(0, 0);
	init(slice->getFormat(), slice->getWidth(), slice->getHeight(), settings);
}

Image::~Image()
{
	--imageCount;
}

void Image::init(PixelFormat fmt, int w, int h, const Settings &settings)
{
	pixelWidth = w;
	pixelHeight = h;

	width  = (int) (pixelWidth / settings.pixeldensity + 0.5);
	height = (int) (pixelHeight / settings.pixeldensity + 0.5);

	mipmapCount = mipmapsType == MIPMAPS_NONE ? 1 : getMipmapCount(w, h);
	format = fmt;

	if (isCompressed() && mipmapsType == MIPMAPS_GENERATED)
		mipmapsType = MIPMAPS_NONE;

	if (getMipmapCount() > 1)
		filter.mipmap = defaultMipmapFilter;
	
	initQuad();
}

void Image::uploadImageData(love::image::ImageDataBase *d, int level, int slice)
{
	love::image::ImageData *id = dynamic_cast<love::image::ImageData *>(d);

	love::thread::EmptyLock lock;
	if (id != nullptr)
		lock.setLock(id->getMutex());

	Rect rect = {0, 0, d->getWidth(), d->getHeight()};
	uploadByteData(d->getFormat(), d->getData(), d->getSize(), rect, level, slice);
}

void Image::replacePixels(love::image::ImageDataBase *d, int slice, int mipmap, bool reloadmipmaps)
{
	// No effect if the texture hasn't been created yet.
	if (getHandle() == 0 || usingDefaultTexture)
		return;

	if (d->getFormat() != getPixelFormat())
		throw love::Exception("Pixel formats must match.");

	if (mipmap < 0 || (mipmapsType != MIPMAPS_DATA && mipmap > 0) || mipmap >= getMipmapCount())
		throw love::Exception("Invalid image mipmap index.");

	if (slice < 0 || (texType == TEXTURE_CUBE && slice >= 6)
		|| (texType == TEXTURE_VOLUME && slice >= std::max(getDepth() >> mipmap, 1))
		|| (texType == TEXTURE_2D_ARRAY && slice >= getLayerCount()))
	{
		throw love::Exception("Invalid image slice index.");
	}

	love::image::ImageDataBase *oldd = data.get(slice, mipmap);

	if (oldd == nullptr)
		throw love::Exception("Image does not store ImageData!");

	int w = d->getWidth();
	int h = d->getHeight();

	if (w != oldd->getWidth() || h != oldd->getHeight())
		throw love::Exception("Dimensions must match the texture's dimensions for the specified mipmap level.");

	Graphics::flushStreamDrawsGlobal();

	d->retain();
	oldd->release();

	data.set(slice, mipmap, d);

	uploadImageData(d, mipmap, slice);

	if (reloadmipmaps && mipmap == 0 && getMipmapCount() > 1)
		generateMipmaps();
}

void Image::replacePixels(const void *data, size_t size, const Rect &rect, int slice, int mipmap, bool reloadmipmaps)
{
	Graphics::flushStreamDrawsGlobal();

	uploadByteData(format, data, size, rect, mipmap, slice);

	if (reloadmipmaps && mipmap == 0 && getMipmapCount() > 1)
		generateMipmaps();
}

bool Image::isCompressed() const
{
	return isPixelFormatCompressed(format);
}

bool Image::isFormatLinear() const
{
	return isGammaCorrect() && !sRGB;
}

Image::MipmapsType Image::getMipmapsType() const
{
	return mipmapsType;
}

Image::Slices::Slices(TextureType textype)
	: textureType(textype)
{
}

void Image::Slices::clear()
{
	data.clear();
}

void Image::Slices::set(int slice, int mipmap, love::image::ImageDataBase *d)
{
	if (textureType == TEXTURE_VOLUME)
	{
		if (mipmap >= (int) data.size())
			data.resize(mipmap + 1);

		if (slice >= (int) data[mipmap].size())
			data[mipmap].resize(slice + 1);

		data[mipmap][slice].set(d);
	}
	else
	{
		if (slice >= (int) data.size())
			data.resize(slice + 1);

		if (mipmap >= (int) data[slice].size())
			data[slice].resize(mipmap + 1);

		data[slice][mipmap].set(d);
	}
}

love::image::ImageDataBase *Image::Slices::get(int slice, int mipmap) const
{
	if (slice < 0 || slice >= getSliceCount(mipmap))
		return nullptr;

	if (mipmap < 0 || mipmap >= getMipmapCount(slice))
		return nullptr;

	if (textureType == TEXTURE_VOLUME)
		return data[mipmap][slice].get();
	else
		return data[slice][mipmap].get();
}

void Image::Slices::add(love::image::CompressedImageData *cdata, int startslice, int startmip, bool addallslices, bool addallmips)
{
	int slicecount = addallslices ? cdata->getSliceCount() : 1;
	int mipcount = addallmips ? cdata->getMipmapCount() : 1;

	for (int mip = 0; mip < mipcount; mip++)
	{
		for (int slice = 0; slice < slicecount; slice++)
			set(startslice + slice, startmip + mip, cdata->getSlice(slice, mip));
	}
}

int Image::Slices::getSliceCount(int mip) const
{
	if (textureType == TEXTURE_VOLUME)
	{
		if (mip < 0 || mip >= (int) data.size())
			return 0;

		return (int) data[mip].size();
	}
	else
		return (int) data.size();
}

int Image::Slices::getMipmapCount(int slice) const
{
	if (textureType == TEXTURE_VOLUME)
		return (int) data.size();
	else
	{
		if (slice < 0 || slice >= (int) data.size())
			return 0;

		return data[slice].size();
	}
}

Image::MipmapsType Image::Slices::validate() const
{
	int slicecount = getSliceCount();
	int mipcount = getMipmapCount(0);

	if (slicecount == 0 || mipcount == 0)
		throw love::Exception("At least one ImageData or CompressedImageData is required!");

	if (textureType == TEXTURE_CUBE && slicecount != 6)
		throw love::Exception("Cube textures must have exactly 6 sides.");

	image::ImageDataBase *firstdata = get(0, 0);

	int w = firstdata->getWidth();
	int h = firstdata->getHeight();
	PixelFormat format = firstdata->getFormat();

	int expectedmips = Texture::getMipmapCount(w, h);

	if (mipcount != expectedmips && mipcount != 1)
		throw love::Exception("Image does not have all required mipmap levels (expected %d, got %d)", expectedmips, mipcount);

	if (textureType == TEXTURE_CUBE && w != h)
		throw love::Exception("Cube images must have equal widths and heights for each cube face.");

	int mipw = w;
	int miph = h;
	int mipslices = slicecount;

	for (int mip = 0; mip < mipcount; mip++)
	{
		if (textureType == TEXTURE_VOLUME)
		{
			slicecount = getSliceCount(mip);

			if (slicecount != mipslices)
				throw love::Exception("Invalid number of image data layers in mipmap level %d (expected %d, got %d)", mip+1, mipslices, slicecount);
		}

		for (int slice = 0; slice < slicecount; slice++)
		{
			auto slicedata = get(slice, mip);

			if (slicedata == nullptr)
				throw love::Exception("Missing image data (slice %d, mipmap level %d)", slice+1, mip+1);

			int realw = slicedata->getWidth();
			int realh = slicedata->getHeight();

			if (getMipmapCount(slice) != mipcount)
				throw love::Exception("All Image layers must have the same mipmap count.");

			if (mipw != realw)
				throw love::Exception("Width of image data (slice %d, mipmap level %d) is incorrect (expected %d, got %d)", slice+1, mip+1, mipw, realw);

			if (miph != realh)
				throw love::Exception("Height of image data (slice %d, mipmap level %d) is incorrect (expected %d, got %d)", slice+1, mip+1, miph, realh);

			if (format != slicedata->getFormat())
				throw love::Exception("All Image slices and mipmaps must have the same pixel format.");
		}

		mipw = std::max(mipw / 2, 1);
		miph = std::max(miph / 2, 1);

		if (textureType == TEXTURE_VOLUME)
			mipslices = std::max(mipslices / 2, 1);
	}
	
	if (mipcount > 1)
		return MIPMAPS_DATA;
	else
		return MIPMAPS_NONE;
}

} // graphics
} // love