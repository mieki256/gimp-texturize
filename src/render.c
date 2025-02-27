#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <gtk/gtk.h>
#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>
#include "plugin-intl.h"

#include "main.h"
#include "render.h"
#include "texturize.h"

/*  Public functions  */

gint32
render (gint32        image_ID,
	GimpDrawable       *drawable,
	PlugInVals         *vals,
	PlugInImageVals    *image_vals,
	PlugInDrawableVals *drawable_vals)
{

/////////////////////                               ////////////////////
/////////////////////      Variable declaration     ////////////////////
/////////////////////                               ////////////////////

  gint32 new_image_id = 0;
  gint32 new_layer_id = 0;
  GimpDrawable *    new_drawable;
  GimpImageBaseType image_type = GIMP_RGB;
  GimpImageType     drawable_type = GIMP_RGB_IMAGE;
  gint32            drawable_id = drawable->drawable_id;

  GimpPixelRgn rgn_in, rgn_out;
  gint width_i, height_i, width_p, height_p;
  gint channels; // 3 for RVB, 1 for grayscale

  gint k, x_i, y_i; // Many counters

  guchar * patch; // To store the original image
  guchar * image; // Buffer to store the current image in a 3d array

  // These are for storing the pixels we have discarded along the cuts.
  guchar * coupe_h_here;  // pixel (x,y) of the patch to which belongs the pixel
                          // on the left (we will thus not use the first
                          // column of this array).

  guchar * coupe_h_west;  // Pixel to the left of the patch to which belongs the
                          // pixel (x,y) (same for the first column).

  guchar * coupe_v_here;  // pixel (x,y) of the patch to which belongs the pixel
                          // to the top (we will thus not use the first
                          // line of this array).

  guchar * coupe_v_north; // Pixel to the top of the patch to which belongs the
                          // pixel (x,y) (same for the first line).

  guchar ** filled; // To keep track of which pixels have been filled.
  // 0 iff the pixel isn't filled
  // 1 if the pixel is filled and wihout any cuts
  // 3 if there is an upwards cut
  // 5 if there is a cut towards the left
  // 7 if both
  // I.e. the weak bit is "filled?", the previous bit is "upwards_cut?".

  int cur_posn[2];          // The position of the pixel to be filled.
  int patch_posn[2];        // Where we'll paste the patch to fill this pixel.
  int x_off_min, y_off_min; // Max and min values of the offset, i.e. the vector
  int x_off_max, y_off_max; // substracted from cur_posn to get patch_posn.

  float progress; // Progress bar displayed during computation.
  gimp_progress_init ("Texturizing image...");

///////////////////////                           //////////////////////
///////////////////////      Image dimensions     //////////////////////
///////////////////////                           //////////////////////

  width_i  = image_vals->width_i;
  height_i = image_vals->height_i;
  width_p  = image_vals->width_p;
  height_p = image_vals->height_p;
  channels = gimp_drawable_bpp (drawable->drawable_id);

  //g_warning ("Tileable : %i\n", vals->make_tileable);

  /* Figure out the type of the new image according to the original image */
  switch (gimp_drawable_type (drawable_id)) {
  case GIMP_RGB_IMAGE:
  case GIMP_RGBA_IMAGE:
    image_type    = GIMP_RGB;
    drawable_type = GIMP_RGB_IMAGE;
    break;
  case GIMP_GRAY_IMAGE:
  case GIMP_GRAYA_IMAGE:
    image_type    = GIMP_GRAY;
    drawable_type = GIMP_GRAY_IMAGE;
    break;
  case GIMP_INDEXED_IMAGE:
  case GIMP_INDEXEDA_IMAGE:
    g_message (_("Sorry, the Texturize plugin only supports RGB and grayscale images. "
		 "Please convert your image to RGB mode first."));
    return -1;
  }

  if (gimp_drawable_has_alpha (drawable_id)) {
    g_message (_("Sorry, the Texturize plugin doesn't support images"
		 " with an alpha (ie transparency) channel yet."
		 " Please flatten your image first."));
    return -1;
  }

////////////////////////////                  ///////////////////////////
////////////////////////////   Recouvrement   ///////////////////////////
////////////////////////////                  ///////////////////////////

  /* WARNING: our conventions here aren't necessarily intuitive. Given the way
     that we detect the next pixel to fill, offsets are always negative values
     (we paste the patch a little above and to the left). However, {x,y}_off_*
     are positive values, and x_off_max < x_off_min. */

  // Heuristic values, to refine when we get more experience.
  x_off_min = MIN (vals->overlap, width_p - 1);
  y_off_min = MIN (vals->overlap, height_p - 1);
  x_off_max = CLAMP (20, x_off_min/3, width_p -1); /* We know that x_off_min/5 < width_p -1 */
  y_off_max = CLAMP (20, y_off_min/3, height_p - 1); /* We know that y_off_min/5 < height_p-1 */

//////////////////                                     /////////////////
//////////////////      New image, initializations     /////////////////
//////////////////                                     /////////////////

  // Create a new image with only one layer.
  new_image_id = gimp_image_new (width_i,height_i,image_type);
  new_layer_id = gimp_layer_new (new_image_id, "Texture",
				 width_i, height_i,
				 drawable_type, 100, GIMP_NORMAL_MODE);
  gimp_image_add_layer (new_image_id, new_layer_id, 0);
  new_drawable = gimp_drawable_get (new_layer_id);

  // Initialize in and out regions.
  gimp_pixel_rgn_init (&rgn_out, new_drawable, 0, 0, width_i, height_i, TRUE, TRUE);
  gimp_pixel_rgn_init (&rgn_in, drawable, 0, 0, width_p, height_p, FALSE, FALSE);

  // Allocate some memory for everyone.
  patch = g_new (guchar,width_p * height_p * channels);
  image = g_new (guchar,width_i * height_i * channels);
  filled = init_guchar_tab_2d (width_i, height_i);

  coupe_h_here  = g_new (guchar, width_i * height_i * channels);
  coupe_h_west  = g_new (guchar, width_i * height_i * channels);
  coupe_v_here  = g_new (guchar, width_i * height_i * channels);
  coupe_v_north = g_new (guchar, width_i * height_i * channels);

  // For security, initialize everything to 0.
  for (k = 0; k < width_i * height_i * channels; k++)
    coupe_h_here[k] = coupe_h_west[k] = coupe_v_here[k] = coupe_v_north[k] = 0;

//////////////////                                    /////////////////
//////////////////    Cleaning up of the new image    /////////////////
//////////////////                                    /////////////////


  // Retrieve the initial image into the patch.
  gimp_pixel_rgn_get_rect (&rgn_in, patch, 0, 0, width_p, height_p);

  // Then paste a first patch at position (0,0) of the out image.
  gimp_pixel_rgn_set_rect (&rgn_out, patch, 0, 0, width_p, height_p);

  // And declare we have already filled in the corresponding pixels.
  for (x_i = 0; x_i < width_p; x_i++) {
    for (y_i = 0; y_i < height_p; y_i++) filled[x_i][y_i] = 1;
  }

  // Retrieve all of the current image into image.
  gimp_pixel_rgn_get_rect (&rgn_out, image, 0, 0, width_i, height_i);


/////////////////////////                      ////////////////////////
/////////////////////////     The big loop     ////////////////////////
/////////////////////////                      ////////////////////////


  // The current position : (0,0)
  cur_posn[0] = 0; cur_posn[1] = 0;

  while (count_filled_pixels (filled,width_i,height_i) < (width_i * height_i)) {
    /* Update the current position: it's the next pixel to fill. */
    if (pixel_to_fill (filled, width_i, height_i, cur_posn) == NULL) {
      g_message (_("There was a problem when filling the new image."));
      exit(-1);
    };

    offset_optimal (patch_posn,
		    image, patch,
		    width_p, height_p, width_i, height_i,
		    cur_posn[0] - x_off_min,
		    cur_posn[1] - y_off_min,
		    cur_posn[0] - x_off_max,
		    cur_posn[1] - y_off_max,
		    channels,
		    filled,
		    vals->make_tileable);

    decoupe_graphe (patch_posn,
		    width_i, height_i, width_p, height_p,
		    channels,
		    filled,
		    image,
		    patch,
		    coupe_h_here, coupe_h_west, coupe_v_here, coupe_v_north,
		    vals->make_tileable,
		    FALSE);

    // Display progress to the user.
    progress = ((float) count_filled_pixels (filled, width_i, height_i)) / ((float)(width_i * height_i));
    gimp_progress_update(progress);
  }


//////////////////////                             /////////////////////
//////////////////////        Last clean up        /////////////////////
//////////////////////                             /////////////////////


/*
  // To see where cuts are.
  guchar * image_coupes;
  image_coupes = g_new(guchar, width_i*height_i*channels);
  for (k=0;k<width_i*height_i*channels;k++) image_coupes[k] = 255;

  for(x_i=1; x_i<width_i; x_i++){
    for(y_i=1; y_i<height_i; y_i++){
      guchar r = filled[x_i][y_i];
      if (HAS_CUT_NORTH(r) || HAS_CUT_WEST(r)){
        for (k=0; k<channels; k++)
          image_coupes[(y_i*width_i +x_i)*channels +k] = 0;
      }
//      if (HAS_CUT_WEST(r))
//        image_coupes[(y_i*width_i +x_i)*channels +channels-1] = 255;
    }
  }
*/


  gimp_pixel_rgn_set_rect (&rgn_out, image, 0, 0, width_i, height_i);

  gimp_drawable_flush (new_drawable);
  gimp_drawable_merge_shadow (new_drawable->drawable_id, TRUE);
  gimp_drawable_update (new_drawable->drawable_id, 0, 0, width_i, height_i);
  gimp_drawable_detach (new_drawable);
  gimp_displays_flush ();

  g_free (patch);
  g_free (coupe_h_here);
  g_free (coupe_h_west);
  g_free (coupe_v_here);
  g_free (coupe_v_north);

  /* Finally return the ID of the new image, for the main function to display
     it */
  return new_image_id;
}
