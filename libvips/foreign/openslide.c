/* Read a virtual microscope slide using OpenSlide.
 *
 * Benjamin Gilbert
 *
 * Copyright (c) 2011 Carnegie Mellon University
 *
 * 26/11/11
 *	- initial version
 * 27/11/11
 *	- fix black background in transparent areas
 *	- no need to set *stop on fill_region() error return
 *	- add OpenSlide properties to image metadata
 *	- consolidate setup into one function
 *	- support reading arbitrary layers
 *	- use VIPS_ARRAY()
 *	- add helper to copy a line of pixels
 *	- support reading associated images
 * 7/12/11
 *	- redirect OpenSlide error logging to vips_error()
 * 8/12/11
 *	- add more exposition to documentation
 * 9/12/11
 * 	- unpack to a tile cache
 * 11/12/11
 * 	- move argb->rgba into conversion
 * 	- turn into a set of read fns ready to be called from a class
 */

/*

    This file is part of VIPS.

    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

/*
#define VIPS_DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>

#include <vips/vips.h>
#include <vips/debug.h>

#include <openslide.h>

#include "openslide.h"

/* We run our own tile cache. The OpenSlide one can't always keep enough for a
 * complete lines of pixels.
 */
#define TILE_WIDTH (256)
#define TILE_HEIGHT (256)

typedef struct {
	openslide_t *osr;
	const char *associated;

	/* Only valid if associated == NULL.
	 */
	int32_t layer;
	double downsample;
} ReadSlide;

int
vips__openslide_isslide( const char *filename )
{
	openslide_t *osr;
	const char *vendor;
	int ok;

	ok = 1;
	osr = openslide_open( filename );
	if( osr != NULL ) {
		/* If this is a generic tiled TIFF image, decline to support
		 * it, since im_tiff2vips can do better.
		 */
		vendor = openslide_get_property_value( osr,
			OPENSLIDE_PROPERTY_NAME_VENDOR );
		if( vendor == NULL ||
			strcmp( vendor, "generic-tiff" ) == 0 )
			ok = 0;
		openslide_close( osr );
	} 
	else 
		ok = 0;

	VIPS_DEBUG_MSG( "vips__openslide_isslide: %s - %d\n", filename, ok );

	return( ok );
}

static void
readslide_destroy_cb( VipsImage *image, ReadSlide *rslide )
{
	VIPS_FREEF( openslide_close, rslide->osr );
}

static int
check_associated_image( openslide_t *osr, const char *name )
{
	const char * const *associated;

	for( associated = openslide_get_associated_image_names( osr );
		*associated != NULL; associated++ )
		if( strcmp( *associated, name ) == 0 )
			return( 0 );

	vips_error( "im_openslide2vips", 
		"%s", _( "invalid associated image name" ) );

	return( -1 );
}

static ReadSlide *
readslide_new( const char *filename, VipsImage *out )
{
	ReadSlide *rslide;
	char name[FILENAME_MAX];
	char mode[FILENAME_MAX];
	char *endp;
	int64_t w, h;
	const char *background;
	const char * const *properties;
	char *associated;

	rslide = VIPS_NEW( out, ReadSlide );
	memset( rslide, 0, sizeof( *rslide ) );
	g_signal_connect( out, "close", G_CALLBACK( readslide_destroy_cb ),
		rslide );

	vips_filename_split( filename, name, mode );
	rslide->osr = openslide_open( name );
	if( rslide->osr == NULL ) {
		vips_error( "im_openslide2vips", 
			"%s", _( "failure opening slide" ) );
		return( NULL );
	}

	/* Parse optional mode.
	 */
	rslide->layer = strtol( mode, &endp, 10 );
	if( *mode != 0 && *endp == 0 ) {
		/* Mode specifies slide layer.
		 */
		if( rslide->layer < 0 || rslide->layer >=
			openslide_get_layer_count( rslide->osr ) ) {
			vips_error( "im_openslide2vips",
				"%s", _( "invalid slide layer" ) );
			return( NULL );
		}
	} 
	else if( *mode != 0 ) {
		/* Mode specifies associated image.
		 */
		if ( check_associated_image( rslide->osr, mode ) )
			return( NULL );
		rslide->associated = vips_strdup( VIPS_OBJECT( out ), mode );
	}

	if( rslide->associated ) {
		openslide_get_associated_image_dimensions( rslide->osr,
			rslide->associated, &w, &h );
		vips_image_set_string( out, "slide-associated-image",
			rslide->associated );
		vips_demand_hint( out, VIPS_DEMAND_STYLE_THINSTRIP, NULL );
	} 
	else {
		openslide_get_layer_dimensions( rslide->osr, rslide->layer,
			&w, &h );
		rslide->downsample = openslide_get_layer_downsample(
			rslide->osr, rslide->layer );
		vips_image_set_int( out, "slide-layer", rslide->layer );
		vips_demand_hint( out, VIPS_DEMAND_STYLE_SMALLTILE, NULL );
	}

	/* This tag is used by argb2rgba() to paint fully-transparent pixels.
	 */
	background = openslide_get_property_value( rslide->osr,
		OPENSLIDE_PROPERTY_NAME_BACKGROUND_COLOR );
	if( background != NULL )
		im_meta_set_int( out, 
			"background-rgb", strtoul( background, NULL, 16 ) );
	else
		im_meta_set_int( out, "background-rgb", 0xffffff );

	if( w < 0 || h < 0 || rslide->downsample < 0 ) {
		vips_error( "im_openslide2vips", _( "getting dimensions: %s" ),
			openslide_get_error( rslide->osr ) );
		return( NULL );
	}
	if( w > INT_MAX || 
		h > INT_MAX ) {
		vips_error( "im_openslide2vips",
			"%s", _( "image dimensions overflow int" ) );
		return( NULL );
	}

	vips_image_init_fields( out, (int) w, (int) h, 4, VIPS_FORMAT_UCHAR,
		VIPS_CODING_NONE, VIPS_INTERPRETATION_RGB, 1.0, 1.0 );

	for( properties = openslide_get_property_names( rslide->osr );
		*properties != NULL; properties++ )
		vips_image_set_string( out, *properties,
			openslide_get_property_value( rslide->osr,
			*properties ) );

	associated = g_strjoinv( ", ", (char **)
		openslide_get_associated_image_names( rslide->osr ) );
	vips_image_set_string( out, "slide-associated-images", associated );
	g_free( associated );

	return( rslide );
}

int
vips__openslide_read_header( const char *filename, VipsImage *out, 
	int layer, char *associated )
{
	ReadSlide *rslide;

	if( !(rslide = readslide_new( filename, out, layer, associated )) )
		return( -1 );

	return( 0 );
}

static int
fill_region( VipsRegion *out, void *seq, void *_rslide, void *unused,
	gboolean *stop )
{
	ReadSlide *rslide = _rslide;
	VipsRect *r = &out->valid;

	const char *error;
	int x, y;

	VIPS_DEBUG_MSG( "fill_region: %dx%d @ %dx%d\n",
		r->width, r->height, r->left, r->top );

	/* Fill in tile-sized chunks. Some versions of OpenSlide can fail for
	 * very large dimensions.
	 */
	for( y = 0; y < r->height; y += TILE_HEIGHT ) 
		for( x = 0; x < r->width; x += TILE_WIDTH ) {
			int w = VIPS_MIN( TILE_WIDTH, r->width - x );
			int h = VIPS_MIN( TILE_HEIGHT, r->height - y );

			openslide_read_region( rslide->osr, 
				(uint32_t *) VIPS_REGION_ADDR( out, 
					r->left + x, r->top + y ),
				(r->left + x) * rslide->downsample, 
				(r->top + y) * rslide->downsample, 
				rslide->layer,
				w, h ); 
		}

	error = openslide_get_error( rslide->osr );
	if( error ) {
		vips_error( "im_openslide2vips", 
			_( "reading region: %s" ), error );

		return( -1 );
	}

	return( 0 );
}

int
vips__openslide_read_file( const char *filename, VipsImage *out, int layer )
{
	ReadSlide *rslide;
	VipsImage *raw;

	VIPS_DEBUG_MSG( "vips__openslide_read_file: %s %d\n", 
		filename, layer );

	/* Tile cache: keep enough for two complete rows of tiles.
	 * This lets us do (smallish) area ops, like im_conv(), while
	 * still only hitting each tile once.
	 */
	if( !(raw = im_open_local( out, "cache", "p" )) )
		return( -1 );

	if( !(rslide = readslide_new( filename, raw, layer, NULL )) )
		return( -1 );

	if( vips_image_pio_output( raw ) ||
		vips_image_generate( raw, 
			NULL, fill_region, NULL, rslide, NULL ) )
		return( -1 );

	/* Copy to out, adding a cache. Enough tiles for a complete row, plus
	 * 50%.
	 */
	if( im_tile_cache( raw, out, 
		TILE_WIDTH, TILE_HEIGHT,
		1.5 * (1 + raw->Xsize / TILE_WIDTH) ) ) 
		return( -1 );

	return( 0 );
}

int
vips__openslide_read_associated( const char *filename, VipsImage *out, 
	const char *associated )
{
	ReadSlide *rslide;
	VipsImage *raw;
	uint32_t *buf;
	int64_t w, h;
	int y;
	const char *error;

	VIPS_DEBUG_MSG( "vips__openslide_read_associated: %s %s\n", 
		filename, associated );

	/* Tile cache: keep enough for two complete rows of tiles.
	 * This lets us do (smallish) area ops, like im_conv(), while
	 * still only hitting each tile once.
	 */
	if( !(raw = im_open_local( out, "cache", "p" )) )
		return( -1 );

	if( !(rslide = readslide_new( filename, raw, 0, associated )) )
		return( -1 );

	openslide_get_associated_image_dimensions( rslide->osr,
		rslide->associated, &w, &h );
	if( w == -1 || 
		h == -1 ) {
		vips_error( "im_openslide2vips", 
			_( "getting dimensions: %s" ),
			openslide_get_error( rslide->osr ) );

		return( -1 );
	}

	buf = VIPS_ARRAY( out, w * h, uint32_t );
	openslide_read_associated_image( rslide->osr, rslide->associated, buf );
	error = openslide_get_error( rslide->osr );
	if( error ) {
		vips_error( "im_openslide2vips",
			_( "reading associated image: %s" ), error );
		return( -1 );
	}

	if( vips_image_wio_output( out ) )
		return( -1 );
	for( y = 0; y < h; y++ ) 
		if( vips_image_write_line( out, y, (PEL *) (buf + y * w) ) )
			return( -1 );

	return( 0 );
}

