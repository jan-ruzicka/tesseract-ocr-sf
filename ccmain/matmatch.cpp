/**********************************************************************
 * File:        matmatch.cpp  (Formerly matrix_match.c)
 * Description: matrix matching routines for Tessedit
 * Author:      Chris Newton
 * Created:     Wed Nov 24 15:57:41 GMT 1993
 *
 * (C) Copyright 1993, Hewlett-Packard Ltd.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 ** http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 *
 **********************************************************************/

#include "mfcpch.h"
#include          <stdlib.h>
#include          <math.h>
#include          <string.h>
#include          <ctype.h>
#ifdef __UNIX__
#include          <assert.h>
#endif
#include          "tessvars.h"
#include          "stderr.h"
#include          "img.h"
#include          "evnts.h"
#include          "showim.h"
#include          "hosthplb.h"
#include          "grphics.h"
#include          "evnts.h"
#include          "adaptions.h"
#include          "matmatch.h"
#include          "secname.h"

#define EXTERN

EXTERN BOOL_VAR (tessedit_display_mm, FALSE, "Display matrix matches");
EXTERN BOOL_VAR (tessedit_mm_debug, FALSE,
"Print debug information for matrix matcher");
EXTERN INT_VAR (tessedit_mm_prototype_min_size, 3,
"Smallest number of samples in a cluster for a prototype to be used");

// Colours for displaying the match
#define BB_COLOUR 0
#define BW_COLOUR 1
#define WB_COLOUR 3
#define UB_COLOUR 5
#define BU_COLOUR 7
#define UU_COLOUR 9
#define WU_COLOUR 11
#define UW_COLOUR 13
#define WW_COLOUR 15

#define BINIM_BLACK 0
#define BINIM_WHITE 1

float matrix_match(  // returns match score
                   IMAGE *image1,
                   IMAGE *image2) {
  ASSERT_HOST (image1->get_bpp () == 1 && image2->get_bpp () == 1);

  if (image1->get_xsize () >= image2->get_xsize ())
    return match1 (image1, image2);
  else
    return match1 (image2, image1);
}


float match1(  /* returns match score */
             IMAGE *image_w,
             IMAGE *image_n) {
  INT32 x_offset;
  INT32 y_offset;
  INT32 x_size = image_w->get_xsize ();
  INT32 y_size;
  INT32 x_size2 = image_n->get_xsize ();
  INT32 y_size2;
  IMAGE match_image;
  IMAGELINE imline_w;
  IMAGELINE imline_n;
  IMAGELINE match_imline;
  INT32 x;
  INT32 y;
  float sum = 0.0;

  x_offset = (image_w->get_xsize () - image_n->get_xsize ()) / 2;

  ASSERT_HOST (x_offset >= 0);
  match_imline.init (x_size);

  sum = 0;

  if (image_w->get_ysize () < image_n->get_ysize ()) {
    y_size = image_n->get_ysize ();
    y_size2 = image_w->get_ysize ();
    y_offset = (y_size - y_size2) / 2;

    if (tessedit_display_mm && !tessedit_mm_use_prototypes)
      tprintf ("I1 (%d, %d), I2 (%d, %d), MI (%d, %d)\n", x_size,
        image_w->get_ysize (), x_size2, image_n->get_ysize (),
        x_size, y_size);

    match_image.create (x_size, y_size, 4);

    for (y = 0; y < y_offset; y++) {
      image_n->fast_get_line (0, y, x_size2, &imline_n);
      for (x = 0; x < x_size2; x++) {
        if (imline_n.pixels[x] == BINIM_BLACK) {
          sum += -1;
          match_imline.pixels[x] = UB_COLOUR;
        }
        else {
          match_imline.pixels[x] = UW_COLOUR;
        }
      }
      match_image.fast_put_line (x_offset, y, x_size2, &match_imline);
    }

    for (y = y_offset + y_size2; y < y_size; y++) {
      image_n->fast_get_line (0, y, x_size2, &imline_n);
      for (x = 0; x < x_size2; x++) {
        if (imline_n.pixels[x] == BINIM_BLACK) {
          sum += -1.0;
          match_imline.pixels[x] = UB_COLOUR;
        }
        else {
          match_imline.pixels[x] = UW_COLOUR;
        }
      }
      match_image.fast_put_line (x_offset, y, x_size2, &match_imline);
    }

    for (y = y_offset; y < y_offset + y_size2; y++) {
      image_w->fast_get_line (0, y - y_offset, x_size, &imline_w);
      image_n->fast_get_line (0, y, x_size2, &imline_n);
      for (x = 0; x < x_offset; x++) {
        if (imline_w.pixels[x] == BINIM_BLACK) {
          sum += -1.0;
          match_imline.pixels[x] = BU_COLOUR;
        }
        else {
          match_imline.pixels[x] = WU_COLOUR;
        }
      }

      for (x = x_offset + x_size2; x < x_size; x++) {
        if (imline_w.pixels[x] == BINIM_BLACK) {
          sum += -1.0;
          match_imline.pixels[x] = BU_COLOUR;
        }
        else {
          match_imline.pixels[x] = WU_COLOUR;
        }
      }

      for (x = x_offset; x < x_offset + x_size2; x++) {
        if (imline_n.pixels[x - x_offset] == imline_w.pixels[x]) {
          sum += 1.0;
          if (imline_w.pixels[x] == BINIM_BLACK)
            match_imline.pixels[x] = BB_COLOUR;
          else
            match_imline.pixels[x] = WW_COLOUR;
        }
        else {
          sum += -1.0;
          if (imline_w.pixels[x] == BINIM_BLACK)
            match_imline.pixels[x] = BW_COLOUR;
          else
            match_imline.pixels[x] = WB_COLOUR;
        }
      }

      match_image.fast_put_line (0, y, x_size, &match_imline);
    }
  }
  else {
    y_size = image_w->get_ysize ();
    y_size2 = image_n->get_ysize ();
    y_offset = (y_size - y_size2) / 2;

    if (tessedit_display_mm && !tessedit_mm_use_prototypes)
      tprintf ("I1 (%d, %d), I2 (%d, %d), MI (%d, %d)\n", x_size,
        image_w->get_ysize (), x_size2, image_n->get_ysize (),
        x_size, y_size);

    match_image.create (x_size, y_size, 4);

    for (y = 0; y < y_offset; y++) {
      image_w->fast_get_line (0, y, x_size, &imline_w);
      for (x = 0; x < x_size; x++) {
        if (imline_w.pixels[x] == BINIM_BLACK) {
          sum += -1;
          match_imline.pixels[x] = BU_COLOUR;
        }
        else {
          match_imline.pixels[x] = WU_COLOUR;
        }
      }
      match_image.fast_put_line (0, y, x_size, &match_imline);
    }

    for (y = y_offset + y_size2; y < y_size; y++) {
      image_w->fast_get_line (0, y, x_size, &imline_w);
      for (x = 0; x < x_size; x++) {
        if (imline_w.pixels[x] == BINIM_BLACK) {
          sum += -1;
          match_imline.pixels[x] = BU_COLOUR;
        }
        else {
          match_imline.pixels[x] = WU_COLOUR;
        }
      }
      match_image.fast_put_line (0, y, x_size, &match_imline);
    }

    for (y = y_offset; y < y_offset + y_size2; y++) {
      image_w->fast_get_line (0, y, x_size, &imline_w);
      image_n->fast_get_line (0, y - y_offset, x_size2, &imline_n);
      for (x = 0; x < x_offset; x++) {
        if (imline_w.pixels[x] == BINIM_BLACK) {
          sum += -1.0;
          match_imline.pixels[x] = BU_COLOUR;
        }
        else {
          match_imline.pixels[x] = WU_COLOUR;
        }
      }

      for (x = x_offset + x_size2; x < x_size; x++) {
        if (imline_w.pixels[x] == BINIM_BLACK) {
          sum += -1.0;
          match_imline.pixels[x] = BU_COLOUR;
        }
        else {
          match_imline.pixels[x] = WU_COLOUR;
        }
      }

      for (x = x_offset; x < x_offset + x_size2; x++) {
        if (imline_n.pixels[x - x_offset] == imline_w.pixels[x]) {
          sum += 1.0;
          if (imline_w.pixels[x] == BINIM_BLACK)
            match_imline.pixels[x] = BB_COLOUR;
          else
            match_imline.pixels[x] = WW_COLOUR;
        }
        else {
          sum += -1.0;
          if (imline_w.pixels[x] == BINIM_BLACK)
            match_imline.pixels[x] = BW_COLOUR;
          else
            match_imline.pixels[x] = WB_COLOUR;
        }
      }

      match_image.fast_put_line (0, y, x_size, &match_imline);
    }
  }

#ifndef GRAPHICS_DISABLED
  if (tessedit_display_mm && !tessedit_mm_use_prototypes) {
    tprintf ("Match score %f\n", 1.0 - sum / (x_size * y_size));
    display_images(image_w, image_n, &match_image); 
  }
#endif

  if (tessedit_mm_debug)
    tprintf ("Match score %f\n", 1.0 - sum / (x_size * y_size));

  return (1.0 - sum / (x_size * y_size));
}


/*************************************************************************
 * display_images()
 *
 * Show a pair of images, plus the match image
 *
 *************************************************************************/

#ifndef GRAPHICS_DISABLED
void display_images(IMAGE *image_w, IMAGE *image_n, IMAGE *match_image) { 
  WINDOW w_im_window;
  WINDOW n_im_window;
  WINDOW match_window;
  GRAPHICS_EVENT event;          //output event
  INT16 i;

                                 // xmin xmax ymin ymax
  w_im_window = create_window ("Image 1", SCROLLINGWIN, 20, 100, 10 * image_w->get_xsize (), 10 * image_w->get_ysize (), 0, image_w->get_xsize (), 0, image_w->get_ysize (),
    TRUE, FALSE, FALSE, TRUE);   // down event & key only

  clear_view_surface(w_im_window); 
  show_sub_image (image_w,
    0, 0,
    image_w->get_xsize (), image_w->get_ysize (),
    w_im_window, 0, 0);

  line_color_index(w_im_window, RED); 
  for (i = 1; i < image_w->get_xsize (); i++) {
    move2d (w_im_window, i, 0);
    draw2d (w_im_window, i, image_w->get_ysize ());
  }
  for (i = 1; i < image_w->get_ysize (); i++) {
    move2d (w_im_window, 0, i);
    draw2d (w_im_window, image_w->get_xsize (), i);
  }

                                 // xmin xmax ymin ymax
  n_im_window = create_window ("Image 2", SCROLLINGWIN, 240, 100, 10 * image_n->get_xsize (), 10 * image_n->get_ysize (), 0, image_n->get_xsize (), 0, image_n->get_ysize (),
    TRUE, FALSE, FALSE, TRUE);   // down event & key only

  clear_view_surface(n_im_window); 
  show_sub_image (image_n,
    0, 0,
    image_n->get_xsize (), image_n->get_ysize (),
    n_im_window, 0, 0);

  line_color_index(n_im_window, RED); 
  for (i = 1; i < image_n->get_xsize (); i++) {
    move2d (n_im_window, i, 0);
    draw2d (n_im_window, i, image_n->get_ysize ());
  }
  for (i = 1; i < image_n->get_ysize (); i++) {
    move2d (n_im_window, 0, i);
    draw2d (n_im_window, image_n->get_xsize (), i);
  }
  overlap_picture_ops(TRUE); 

                                 // xmin xmax ymin ymax
  match_window = create_window ("Match Result", SCROLLINGWIN, 460, 100, 10 * match_image->get_xsize (), 10 * match_image->get_ysize (), 0, match_image->get_xsize (), 0, match_image->get_ysize (),
    TRUE, FALSE, FALSE, TRUE);   // down event & key only

  clear_view_surface(match_window); 
  show_sub_image (match_image,
    0, 0,
    match_image->get_xsize (), match_image->get_ysize (),
    match_window, 0, 0);

  line_color_index(match_window, RED); 
  for (i = 1; i < match_image->get_xsize (); i++) {
    move2d (match_window, i, 0);
    draw2d (match_window, i, match_image->get_ysize ());
  }
  for (i = 1; i < match_image->get_ysize (); i++) {
    move2d (match_window, 0, i);
    draw2d (match_window, match_image->get_xsize (), i);
  }
  overlap_picture_ops(TRUE); 

  await_event(match_window, TRUE, ANY_EVENT, &event); 
  destroy_window(w_im_window); 
  destroy_window(n_im_window); 
  destroy_window(match_window); 
}


/*************************************************************************
 * display_image()
 *
 * Show a single image
 *
 *************************************************************************/

WINDOW display_image(IMAGE *image,
                     const char *title,
                     INT32 x,
                     INT32 y,
                     BOOL8 wait) {
  WINDOW im_window;
  INT16 i;
  GRAPHICS_EVENT event;          //output event

                                 // xmin xmax ymin ymax
  im_window = create_window (title, SCROLLINGWIN, x, y, 10 * image->get_xsize (), 10 * image->get_ysize (), 0, image->get_xsize (), 0, image->get_ysize (),
    TRUE, FALSE, FALSE, TRUE);   // down event & key only

  clear_view_surface(im_window); 
  show_sub_image (image,
    0, 0,
    image->get_xsize (), image->get_ysize (), im_window, 0, 0);

  line_color_index(im_window, RED); 
  for (i = 1; i < image->get_xsize (); i++) {
    move2d (im_window, i, 0);
    draw2d (im_window, i, image->get_ysize ());
  }
  for (i = 1; i < image->get_ysize (); i++) {
    move2d (im_window, 0, i);
    draw2d (im_window, image->get_xsize (), i);
  }
  overlap_picture_ops(TRUE); 

  if (wait)
    await_event(im_window, TRUE, ANY_EVENT, &event); 

  return im_window;
}
#endif
