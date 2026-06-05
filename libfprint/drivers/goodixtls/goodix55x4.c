// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-assembling.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"
#include "glibconfig.h"
#include "gusb/gusb-device.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define FP_COMPONENT "goodixtls55x4"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix55x4.h"
#include "goodix_proto.h"

#include <math.h>

/* 55a2 with the Windows config: raw image is 56 wide x 176 tall = 9856 px,
 * raw frame = 176*56/4*6 = 14784 bytes (matches the working Python capture_tl).
 * SCAN_WIDTH == WIDTH (no padding columns to crop on this sensor). */
#define GOODIX55X4_WIDTH 56
#define GOODIX55X4_HEIGHT 176
#define GOODIX55X4_SCAN_WIDTH 56
#define GOODIX55X4_FRAME_SIZE (GOODIX55X4_WIDTH * GOODIX55X4_HEIGHT)
// For every 4 pixels there are 6 bytes and there are 8 extra start bytes and 5
// extra end
#define GOODIX55X4_RAW_FRAME_SIZE                                              \
  (GOODIX55X4_HEIGHT * GOODIX55X4_SCAN_WIDTH) / 4 * 6
#define GOODIX55X4_CAP_FRAMES 1 // Number of frames we capture per swipe

/* ---- Swipe assembly (ported from ElvinStarry/libfprint goodix55a2.c) ----
 * The 56-px sensor short axis is far too small for single-frame minutiae
 * matching. We stream frames at ~24fps while the finger swipes along the long
 * axis, rotate each frame 90deg (sensor long axis -> stripe width), and stack
 * them into a wide landscape image with enough area for NBIS. */
#define GOODIX55X4_CROP 4 /* drop 4 bogus rows/cols on every edge */
#define GOODIX55X4_OUT_WIDTH (GOODIX55X4_WIDTH - 2 * GOODIX55X4_CROP)   /* 48 */
#define GOODIX55X4_OUT_HEIGHT (GOODIX55X4_HEIGHT - 2 * GOODIX55X4_CROP) /* 168 */
/* rotated stripe: sensor long axis (168) -> width, short axis (48) -> height */
#define GOODIX55X4_SWIPE_FRAME_W GOODIX55X4_OUT_HEIGHT /* 168 */
#define GOODIX55X4_SWIPE_FRAME_H GOODIX55X4_OUT_WIDTH  /* 48  */
/* Minimum distinct stripes for a usable image. A short swipe (e.g. 5 stripes /
 * ~240px) carries too few minutiae and scores ~0 even for the genuine finger;
 * such swipes are discarded and the user is asked to swipe again rather than
 * producing a false no-match. Good deliberate swipes give 15-28 stripes. */
#define GOODIX55X4_SWIPE_MIN_FRAMES 12
#define GOODIX55X4_SWIPE_MAX_FRAMES 60   /* hard cap on stored stripes */
#define GOODIX55X4_FDT_TARGET 8 /* frames per capture in FDT-gated (Mode B) mode */
#define GOODIX55X4_SWIPE_WAIT_FRAMES 250 /* max frames to wait for finger-on */
#define GOODIX55X4_SWIPE_STATIC_THRESHOLD 12 /* MAD below this = not moving */
#define GOODIX55X4_SWIPE_STATIC_FRAMES 4     /* this many static frames = end */
/* Only KEEP a streamed frame whose MAD vs the last kept frame is >= this, so
 * stored stripes cover distinct finger area (avoids the motion-smear from
 * blending dozens of near-duplicate slow-swipe frames). */
#define GOODIX55X4_SWIPE_KEEP_STEP 22
#define GOODIX55X4_DPI 500.0
#define GOODIX55X4_PPMM (GOODIX55X4_DPI / 25.4) /* ~19.685 */
/* Finger present when the raw decoded mean drops this far below the no-finger
 * baseline (~2450); a finger lowers the capacitive signal (~1748 with finger).
 * Baseline is measured from the first streamed frames at runtime. */
#define GOODIX55X4_FINGER_DROP 350

typedef unsigned short Goodix55X4Pix;

struct _FpiDeviceGoodixTls55X4 {
  FpiDeviceGoodixTls parent;

  guint8 *otp;

  GSList *frames;

  Goodix55X4Pix empty_img[GOODIX55X4_FRAME_SIZE];

  /* fdt_down command (0x0c,0x01 + 6 zones of {0x80, base>>1}) computed from the
   * sensor's own baseline reported in the fdt_mode reply. The hardcoded
   * fdt_switch_state_down_55X4 is for GF3268; this device (GF320x) has a
   * different baseline, so finger-down never triggered with the static values. */
  guint8 fdt_down_dyn[26];
  guint8 fdt_down_len;
  gboolean fdt_down_ready;

  /* How often we re-armed finger-detection because the captured frame was
   * noise/empty (no real ridges). Bounded to avoid hammering USB forever. */
  guint scan_retries;

  /* ---- swipe assembly state ---- */
  GSList *strips;            /* list of struct fpi_frame*, rotated stripes */
  guint strips_len;
  guint swipe_frame_idx;     /* total frames streamed this swipe */
  gboolean finger_seen;
  gboolean finger_moving;
  guint static_count;
  gint baseline_mean;        /* no-finger raw mean, measured at swipe start */
  guint8 prev_out[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];
  guint8 last_kept_out[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];
  gboolean have_kept; /* a stripe has been stored since the last reset */
};

G_DECLARE_FINAL_TYPE(FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4, FPI,
                         DEVICE_GOODIXTLS55X4, FpiDeviceGoodixTls);

G_DEFINE_TYPE(FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4,
              FPI_TYPE_DEVICE_GOODIXTLS);

static void goodix55X4_reset_state(FpiDeviceGoodixTls55X4 *self) {}

// ---- ACTIVE SECTION START ----

enum activate_states {
  ACTIVATE_USB_RESET,
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_NUM_STATES,
};

static void check_none(FpDevice *dev, gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_firmware_version(FpDevice *dev, gchar *firmware,
                                   gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device firmware: \"%s\"", firmware);
  fp_dbg("%s\n", firmware);

  /* The 55a2 ships a different sub-model/version (e.g.
   * GF3208/GF3258_RTSEC_APP_10062) than the reference 55b4
   * (GF3268_RTSEC_APP_10041). Accept any GF32xx RTSEC app firmware of the
   * family instead of requiring an exact string match. */
  if (!g_str_has_prefix(firmware, "GF32") ||
      g_strstr_len(firmware, -1, "RTSEC_APP") == NULL) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device firmware: \"%s\"", firmware);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_reset(FpDevice *dev, gboolean success, guint16 number,
                        gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to reset device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device reset number: %d", number);

  if (number != GOODIX_55X4_RESET_NUMBER) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device reset number: %d", number);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_preset_psk_read(FpDevice *dev, gboolean success,
                                  guint32 flags, guint8 *psk, guint16 length,
                                  gpointer user_data, GError *error) {
  g_autofree gchar *psk_str = data_to_str(psk, length);

  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to read PSK from device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device PSK: 0x%s", psk_str);
  fp_dbg("Device PSK flags: 0x%08x", flags);

  if (flags != GOODIX_55X4_PSK_FLAGS) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK flags: 0x%08x", flags);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (length != sizeof(goodix_55x4_psk_0)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (memcmp(psk, goodix_55x4_psk_0, sizeof(goodix_55x4_psk_0))) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}
static void check_idle(FpDevice *dev, gpointer user_data, GError *err) {

  if (err) {
    fpi_ssm_mark_failed(user_data, err);
    return;
  }
  fpi_ssm_next_state(user_data);
}
static void check_sleep_realtek(FpDevice *dev, gboolean success, gpointer user_data, GError *err) {

  if (err) {
    fpi_ssm_mark_failed(user_data, err);
    return;
  }
  if (!success) {
    fpi_ssm_mark_failed(user_data,
                        g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "failed to put into sleep mode (realtek)"));
    return;
  } else {
    fp_dbg("Device is now on sleep\n");
  }
  fpi_ssm_next_state(user_data);
}
static void check_config_upload(FpDevice *dev, gboolean success,
                                gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
  } else if (!success) {
    fpi_ssm_mark_failed(user_data,
                        g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "failed to upload mcu config"));
  } else {
    fpi_ssm_next_state(user_data);
  }
}
static void check_powerdown_scan_freq(FpDevice *dev, gboolean success,
                                      gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
  } else if (!success) {
    fpi_ssm_mark_failed(user_data,
                        g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "failed to set powerdown freq"));
  } else {
    // goodix_send_mcu_get_pov_image(dev, check_mcu_pov_image, user_data);
  }
}

static void check_mcu_pov_image(FpDevice *dev, gboolean success,
                                gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
  } else if (!success) {
    fpi_ssm_mark_failed(user_data,
                        g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "failed to set powerdown freq"));
  } else {
    fpi_ssm_next_state(user_data);
  }
}

static void activate_reset_settle(FpDevice *dev, gpointer ssm) {
  fpi_ssm_next_state(ssm);
}

static void activate_run_state(FpiSsm *ssm, FpDevice *dev) {

  switch (fpi_ssm_get_cur_state(ssm)) {
  case ACTIVATE_USB_RESET: {
    /* OPTIONAL (opt-in via GOODIX_USB_RESET): a USB port reset at the start of
     * each activation, to mitigate the sensor's finger-detection (FDT) becoming
     * unreliable after many capture cycles. Release the interface, reset,
     * re-claim, then wait ~2s for the firmware to come back up. NOTE: this only
     * helps MILD degradation; once FDT is deeply wedged (after a very large
     * number of resets) only a full power-cycle/reboot recovers it. Off by
     * default — it adds ~2s latency per scan and is unnecessary on a fresh
     * device. */
    if (g_getenv("GOODIX_USB_RESET")) {
      GUsbDevice *usb = fpi_device_get_usb_device(dev);
      GError *uerr = NULL;
      g_usb_device_release_interface(usb, 0, 0, NULL);
      if (!g_usb_device_reset(usb, &uerr)) {
        fp_warn("USB reset failed: %s", uerr ? uerr->message : "?");
        g_clear_error(&uerr);
      }
      if (!g_usb_device_claim_interface(usb, 0, 0, &uerr)) {
        fpi_ssm_mark_failed(ssm, uerr);
        return;
      }
      fpi_device_add_timeout(dev, 2000, activate_reset_settle, ssm, NULL);
      return; /* settle, then advance from the timeout callback */
    }
    fpi_ssm_next_state(ssm);
    break;
  }
  case ACTIVATE_READ_AND_NOP:
    fp_dbg("Read and NO OP\n");
    // Nop seems to clear the previous command buffer. But we are
    // unable to do so.
    goodix_start_read_loop(dev);
    goodix_send_nop(dev, check_none, ssm);
    break;

  case ACTIVATE_ENABLE_CHIP:
    fp_dbg("Enable Chip\n");
    goodix_send_enable_chip(dev, TRUE, check_none, ssm);
    break;

  case ACTIVATE_NOP:
    fp_dbg("NO OP\n");
    goodix_send_nop(dev, check_none, ssm);
    break;

  case ACTIVATE_CHECK_FW_VER:
    fp_dbg("Checking FW\n");
    goodix_send_firmware_version(dev, check_firmware_version, ssm);
    break;

  case ACTIVATE_CHECK_PSK:
    fp_dbg("Checking PSK\n");
    goodix_send_preset_psk_read(dev, GOODIX_55X4_PSK_FLAGS, 32,
                                check_preset_psk_read, ssm);
    break;

  case ACTIVATE_RESET:
    fp_dbg("Reset Device\n");
    goodix_send_reset(dev, TRUE, 20, check_reset, ssm);
    break;

  case ACTIVATE_SET_MCU_IDLE:
    fp_dbg("Device IDLE\n");
    goodix_send_mcu_switch_to_idle_mode(dev, 20, check_idle, ssm);
    break;

    // case ACTIVATE_SET_ODP:
    //     goodix_send_read_otp(dev, read_otp_callback, ssm);
    //     break;

  case ACTIVATE_SET_MCU_CONFIG:
    fp_dbg("Uploading Device Config\n");
    goodix_send_upload_config_mcu(dev, goodix_55x4_config,
                                  sizeof(goodix_55x4_config), NULL,
                                  check_config_upload, ssm);
    break;

    // case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
    //     fp_dbg("Powerdown Scan Freq\n");
    //     /*goodix_send_set_powerdown_scan_frequency(
    //         dev, 100, check_powerdown_scan_freq, ssm);*/
    //     //goodix_send_drv_state(dev, check_powerdown_scan_freq, ssm);
    //     fpi_ssm_next_state(ssm);
    //     break;
  }
}

static void tls_activation_complete(FpDevice *dev, gpointer user_data,
                                    GError *error) {
  if (error) {
    fp_err("failed to complete tls activation: %s", error->message);
    return;
  }
  FpImageDevice *image_dev = FP_IMAGE_DEVICE(dev);

  fpi_image_device_activate_complete(image_dev, error);
}

static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  G_DEBUG_HERE();
  if (!error)
    goodix_tls(dev, tls_activation_complete, NULL);
  else {
    fp_err("failed during activation: %s (code: %d)", error->message,
           error->code);
    fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
  }
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

enum SCAN_STAGES {
  /* Like the Windows driver: confirm the MCU considers TLS connected before
   * scanning. If isTlsConnected is 0 here, get_image will answer with 0xd0
   * (REQUEST_TLS_CONNECTION) - which is exactly the failure we see. */
  SCAN_STAGE_QUERY_MCU,
  /* 55a2 (GF3208/GF3258 fw 10062) returns 0xd0 / times out if asked for an
   * image before any FDT mode switch, so switch to FDT mode BEFORE reading
   * the empty calibration frame. */
  SCAN_STAGE_SWITCH_TO_FDT_MODE,
  SCAN_STAGE_CALIBRATE,
  SCAN_STAGE_SWITCH_TO_FDT_DOWN,
  /* Light the orange "scanning" LED right before we start streaming so the user
   * knows WHEN to place/swipe the finger (the fw streams immediately after). */
  SCAN_STAGE_SET_LED,
  SCAN_STAGE_GET_IMG,
  SCAN_STAGE_SWITCH_TO_FDT_MODE2,
  SCAN_STAGE_SWITCH_TO_FDT_UP_NO_REPLY,
  SCAN_STAGE_SWITCH_TO_FDT_UP,
//  SCAN_STAGE_SWITCH_TO_SLEEP_MODE,
//  SCAN_STAGE_SWITCH_TO_SLEEP_MODE_REALTEK,
  SCAN_STAGE_SWITCH_TO_FDT_DONE,
  SCAN_STAGE_NUM,
};

enum SLEEP_STAGES {
  SLEEP_STAGE_SWITCH_TO_SLEEP_MODE,
  SLEEP_STAGE_SWITCH_TO_SLEEP_MODE_REALTEK,
  SLEEP_STAGE_DEACTIVATE,
  SLEEP_STAGE_NUM,
};


static void check_none_cmd(FpDevice *dev, guint8 *data, guint16 len,
                           gpointer ssm, GError *err) {
  if (err) {
    fp_dbg("CHECK NONE FAILED\n");
    fpi_ssm_mark_failed(ssm, err);
    return;
  }
  fp_dbg("CHECK NONE SUCCESS\n");
  fpi_ssm_next_state(ssm);
}

/* Callback for the fdt_mode switch: the reply carries the sensor's per-zone
 * baseline (4-byte header + 6x little-endian 16-bit values). The Windows driver
 * derives each finger-detect threshold byte as (baseline >> 1), framed on the
 * wire as {0x80, byte}. We build the fdt_down command from the live baseline so
 * finger detection works on this sensor instead of the hardcoded GF3268 values. */
static void fdt_mode_base_cb(FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);
  /* reply = 4-byte header + N little-endian 16-bit zone baselines. Build an
   * fdt_down command with the SAME zone count as fdt_mode (each zone framed as
   * {0x80, base>>1}); a shorter command does not arm finger detection. */
  int nzones = (len > 4) ? (len - 4) / 2 : 0;
  if (nzones > 12)
    nzones = 12;
  if (nzones > 0) {
    self->fdt_down_dyn[0] = 0x0c;
    self->fdt_down_dyn[1] = 0x01;
    GString *dbg = g_string_new(NULL);
    for (int z = 0; z < nzones; z++) {
      guint16 base = data[4 + 2 * z] | (data[5 + 2 * z] << 8);
      guint8 thr = (base >> 1) & 0xff;
      self->fdt_down_dyn[2 + 2 * z] = 0x80;
      self->fdt_down_dyn[3 + 2 * z] = thr;
      g_string_append_printf(dbg, "z%d=0x%04x->0x%02x ", z, base, thr);
    }
    self->fdt_down_len = 2 + 2 * nzones;
    self->fdt_down_ready = TRUE;
    fp_dbg("dynamic fdt_down (%d zones): %s", nzones, dbg->str);
    g_string_free(dbg, TRUE);
  } else {
    fp_warn("fdt_mode reply too short (%d), keeping static fdt_down", len);
  }
  fpi_ssm_next_state(ssm);
}

static unsigned char get_pix(struct fpi_frame_asmbl_ctx *ctx,
                             struct fpi_frame *frame, unsigned int x,
                             unsigned int y) {
  return frame->data[x + y * GOODIX55X4_WIDTH];
}

// Bitdepth is 12, but we have to fit it in a byte
static unsigned char squash(int v) { return v / 16; }

static void decode_frame(Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE],
                         const guint8 *raw_frame) {

  Goodix55X4Pix uncropped[GOODIX55X4_SCAN_WIDTH * GOODIX55X4_HEIGHT];
  Goodix55X4Pix *pix = uncropped;
  for (int i = 0; i < GOODIX55X4_RAW_FRAME_SIZE; i += 6) {
    const guint8 *chunk = raw_frame + i;
    *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
    *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
    *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
    *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
  }

  for (int y = 0; y != GOODIX55X4_HEIGHT; ++y) {
    for (int x = 0; x != GOODIX55X4_WIDTH; ++x) {
      const int idx = x + y * GOODIX55X4_SCAN_WIDTH;
      frame[x + y * GOODIX55X4_WIDTH] = uncropped[idx];
    }
  }
}
static int goodix_cmp_short(const void *a, const void *b) {
  return (int)(*(short *)a - *(short *)b);
}

static void rotate_frame(Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE]) {
  Goodix55X4Pix buff[GOODIX55X4_FRAME_SIZE];

  for (int y = 0; y != GOODIX55X4_HEIGHT; ++y) {
    for (int x = 0; x != GOODIX55X4_WIDTH; ++x) {
      buff[x * GOODIX55X4_WIDTH + y] = frame[x + y * GOODIX55X4_WIDTH];
    }
  }
  memcpy(frame, buff, GOODIX55X4_FRAME_SIZE);
}
static void squash_frame(Goodix55X4Pix *frame, guint8 *squashed) {
  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    squashed[i] = squash(frame[i]);
  }
}
/**
 * @brief Squashes the 12 bit pixels of a raw frame into the 4 bit pixels used
 * by libfprint.
 * @details Borrowed from the elan driver. We reduce frames to
 * within the max and min.
 *
 * @param frame
 * @param squashed
 */
static void squash_frame_linear(Goodix55X4Pix *frame, guint8 *squashed) {
  Goodix55X4Pix min = 0xffff;
  Goodix55X4Pix max = 0;

  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    const Goodix55X4Pix pix = frame[i];
    if (pix < min) {
      min = pix;
    }
    if (pix > max) {
      max = pix;
    }
  }

  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    const Goodix55X4Pix pix = frame[i];
    if (pix - min == 0 || max - min == 0) {
      squashed[i] = 0;
    } else {
      squashed[i] = (pix - min) * 0xff / (max - min);
    }
  }
}

/**
 * @brief Subtracts the background from the frame
 *
 * @param frame
 * @param background
 */
static gboolean
postprocess_frame(Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE],
                  Goodix55X4Pix background[GOODIX55X4_FRAME_SIZE]) {
  int sum = 0;
  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    Goodix55X4Pix *og_px = frame + i;
    Goodix55X4Pix bg_px = background[i];
    if (bg_px > *og_px) {
      *og_px = bg_px - *og_px;
    } else {
      *og_px -= bg_px;
    }

    sum += *og_px;
  }
  if (sum == 0) {
    fp_warn("frame darker than background, finger on scanner during "
            "calibration?");
  }
  return sum != 0;
}

typedef struct _frame_processing_info {
  FpiDeviceGoodixTls55X4 *dev;
  GSList **frames;

} frame_processing_info;

static void process_frame(Goodix55X4Pix *raw_frame,
                          frame_processing_info *info) {
  struct fpi_frame *frame =
      g_malloc(GOODIX55X4_FRAME_SIZE + sizeof(struct fpi_frame));
  postprocess_frame(raw_frame, info->dev->empty_img);
  squash_frame_linear(raw_frame, frame->data);

  *(info->frames) = g_slist_append(*(info->frames), frame);
}

static void save_frame(FpiDeviceGoodixTls55X4 *self, guint8 *raw) {
  Goodix55X4Pix *frame = malloc(GOODIX55X4_FRAME_SIZE * sizeof(Goodix55X4Pix));
  decode_frame(frame, raw);
  self->frames = g_slist_append(self->frames, frame);
}

/* Reject empty/noise frames. The sensor's finger-detect (FDT) sometimes fires
 * without a real finger (or before it settles); the resulting frame is pure
 * sensor noise, which squash_frame_linear then stretches to full contrast. A
 * real fingerprint is strongly periodic (the ridges), so its normalized
 * autocorrelation at the ridge period is high; noise is aperiodic and stays
 * near zero. Measured separation on real captures: ridges >=0.7, noise <=0.21.
 * scale-invariant (divided by variance), so it works on the squashed image. */
#define GOODIX55X4_VALIDITY_THRESHOLD 0.40
/* Re-arm FDT at most this many times per scan before giving up on this swipe. */
#define GOODIX55X4_MAX_SCAN_RETRIES 60

static double frame_ridge_validity(const guint8 *img) {
  const int W = GOODIX55X4_WIDTH, H = GOODIX55X4_HEIGHT;
  double mean = 0.0;
  for (int i = 0; i < W * H; ++i)
    mean += img[i];
  mean /= (double)(W * H);

  double var = 0.0;
  for (int i = 0; i < W * H; ++i) {
    const double d = img[i] - mean;
    var += d * d;
  }
  var /= (double)(W * H);
  if (var < 1e-6)
    return 0.0;

  double best = 0.0;
  /* vertical autocorrelation: shift by d rows */
  for (int d = 4; d < 16; ++d) {
    double acc = 0.0;
    int cnt = 0;
    for (int y = 0; y + d < H; ++y)
      for (int x = 0; x < W; ++x) {
        acc += (img[x + y * W] - mean) * (img[x + (y + d) * W] - mean);
        ++cnt;
      }
    const double ac = fabs((acc / cnt) / var);
    if (ac > best)
      best = ac;
  }
  /* horizontal autocorrelation: shift by d columns */
  for (int d = 4; d < 16; ++d) {
    double acc = 0.0;
    int cnt = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x + d < W; ++x) {
        acc += (img[x + y * W] - mean) * (img[(x + d) + y * W] - mean);
        ++cnt;
      }
    const double ac = fabs((acc / cnt) / var);
    if (ac > best)
      best = ac;
  }
  return best;
}

/* ---- swipe imaging helpers (ported from ElvinStarry/libfprint goodix55a2) ---- */

/* raw decoded mean over the full 56x176 frame (12-bit values) */
static gint swipe_raw_mean(const Goodix55X4Pix *frame) {
  guint64 sum = 0;
  for (int i = 0; i < GOODIX55X4_FRAME_SIZE; ++i)
    sum += frame[i];
  return (gint)(sum / GOODIX55X4_FRAME_SIZE);
}

/* crop the 4-px borders and squash 12-bit -> 8-bit into an OUT_W x OUT_H buf */
static void swipe_build_out(const Goodix55X4Pix *frame, guint8 *out) {
  for (int oy = 0; oy < GOODIX55X4_OUT_HEIGHT; ++oy)
    for (int ox = 0; ox < GOODIX55X4_OUT_WIDTH; ++ox) {
      const int sx = ox + GOODIX55X4_CROP;
      const int sy = oy + GOODIX55X4_CROP;
      guint v = frame[sx + sy * GOODIX55X4_WIDTH] >> 4;
      out[ox + oy * GOODIX55X4_OUT_WIDTH] = v > 255 ? 255 : (guint8)v;
    }
}

/* 4-channel fixed-pattern-noise correction + p10/p90 contrast stretch.
 * The sensor reads out 4 interleaved channels cycling across rows; equalise
 * each channel's mean to the global mean, then stretch the central histogram
 * so NBIS gets strong ridge/valley contrast. Operates in place on OUT_WxOUT_H. */
static void swipe_fpn_stretch(guint8 *img) {
  const int w = GOODIX55X4_OUT_WIDTH, h = GOODIX55X4_OUT_HEIGHT;
  const gsize n = (gsize)w * h;

  double global_sum = 0;
  for (gsize i = 0; i < n; ++i)
    global_sum += img[i];
  const double global_mean = global_sum / n;

  for (int ch = 0; ch < 4; ++ch) {
    double ch_sum = 0;
    gsize ch_n = 0;
    for (int y = ch; y < h; y += 4)
      for (int x = 0; x < w; ++x) {
        ch_sum += img[y * w + x];
        ch_n++;
      }
    if (!ch_n)
      continue;
    const double offset = global_mean - (ch_sum / ch_n);
    for (int y = ch; y < h; y += 4)
      for (int x = 0; x < w; ++x) {
        double v = (double)img[y * w + x] + offset;
        img[y * w + x] = (guint8)CLAMP(v, 0.0, 255.0);
      }
  }

  guint32 hist[256] = {0};
  for (gsize i = 0; i < n; ++i)
    hist[img[i]]++;
  gsize lo_t = (n * 10) / 100, hi_t = (n * 90) / 100, cum = 0;
  guint8 lo = 0, hi = 255;
  for (int v = 0; v < 256; ++v) {
    cum += hist[v];
    if (cum >= lo_t) { lo = (guint8)v; break; }
  }
  cum = 0;
  for (int v = 0; v < 256; ++v) {
    cum += hist[v];
    if (cum >= hi_t) { hi = (guint8)v; break; }
  }
  if (hi > lo)
    for (gsize i = 0; i < n; ++i) {
      int v = (((int)img[i] - lo) * 255) / (hi - lo);
      img[i] = (guint8)CLAMP(v, 0, 255);
    }
}

/* mean absolute difference between two OUT buffers (inter-frame motion) */
static guint swipe_out_diff(const guint8 *a, const guint8 *b) {
  guint64 sum = 0;
  const gsize n = (gsize)GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT;
  for (gsize i = 0; i < n; ++i)
    sum += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
  return (guint)(sum / n);
}

static unsigned char swipe_get_pixel(struct fpi_frame_asmbl_ctx *ctx,
                                     struct fpi_frame *frame, unsigned int x,
                                     unsigned int y) {
  return frame->data[x + y * ctx->frame_width];
}

static struct fpi_frame_asmbl_ctx swipe_asmbl_ctx = {
    .frame_width = GOODIX55X4_SWIPE_FRAME_W,
    .frame_height = GOODIX55X4_SWIPE_FRAME_H,
    .image_width = GOODIX55X4_SWIPE_FRAME_W,
    .get_pixel = swipe_get_pixel,
};

/* build a rotated, per-stripe p5/p95-stretched fpi_frame from an OUT buffer */
static struct fpi_frame *swipe_make_stripe(const guint8 *out) {
  const gsize frame_bytes =
      (gsize)GOODIX55X4_SWIPE_FRAME_W * GOODIX55X4_SWIPE_FRAME_H;
  struct fpi_frame *stripe = g_malloc(sizeof(struct fpi_frame) + frame_bytes);
  stripe->delta_x = 0;
  stripe->delta_y = 0;

  const gsize px = (gsize)GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT;
  guint hist[256] = {0};
  for (gsize p = 0; p < px; ++p)
    hist[out[p]]++;
  guint lo = 0, hi = 255, cum = 0;
  const guint t_lo = (guint)(px * 5 / 100), t_hi = (guint)(px * 95 / 100);
  for (guint b = 0; b < 256; ++b) {
    cum += hist[b];
    if (cum >= t_lo && lo == 0) lo = b;
    if (cum >= t_hi) { hi = b; break; }
  }
  if (hi <= lo)
    hi = lo + 1;

  for (guint x = 0; x < GOODIX55X4_SWIPE_FRAME_W; ++x)
    for (guint y = 0; y < GOODIX55X4_SWIPE_FRAME_H; ++y) {
      guint8 raw = out[x * GOODIX55X4_OUT_WIDTH + y];
      int v = (int)(raw - lo) * 255 / (int)(hi - lo);
      stripe->data[x + y * GOODIX55X4_SWIPE_FRAME_W] = (guint8)CLAMP(v, 0, 255);
    }
  return stripe;
}

static void swipe_reset(FpiDeviceGoodixTls55X4 *self) {
  if (self->strips) {
    g_slist_free_full(self->strips, g_free);
    self->strips = NULL;
  }
  self->strips_len = 0;
  self->swipe_frame_idx = 0;
  self->finger_seen = FALSE;
  self->finger_moving = FALSE;
  self->static_count = 0;
  self->baseline_mean = 0;
  self->have_kept = FALSE;
}

static void scan_on_read_img(FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err);

/* Refresh the orange capture LED, then read the next frame. The fw only keeps
 * the LED lit while SetLed is sent repeatedly during active capture (a single
 * SetLed has no visible effect), so we re-send it each frame while waiting for
 * the finger — that is exactly when the user needs the "swipe now" cue. */
static void led_then_read_img(FpDevice *dev, gpointer ssm, GError *err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }
  goodix_tls_read_image(dev, scan_on_read_img, ssm);
}

static void scan_on_read_img(FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }

  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);
  FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);

  /* Decode this streamed frame and build the cropped/FPN/stretched OUT buffer. */
  Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE];
  decode_frame(frame, data);
  const gint mean = swipe_raw_mean(frame);

  /* Per-pixel background subtraction with the no-finger calibration frame. This
   * cancels the sensor's fixed pattern (the vertical grid the 4-channel FPN
   * alone leaves behind) so NBIS detects real ridge minutiae, not grid noise —
   * exactly what made the single-frame path clean. Mean (above) is taken from
   * the RAW frame first, since finger detection needs the raw signal level. */
  if (swipe_raw_mean(self->empty_img) > 500)
    postprocess_frame(frame, self->empty_img);

  guint8 out[GOODIX55X4_OUT_WIDTH * GOODIX55X4_OUT_HEIGHT];
  swipe_build_out(frame, out);
  swipe_fpn_stretch(out);

  self->swipe_frame_idx++;
  if (self->baseline_mean == 0) {
    /* No-finger baseline from the calibration (empty) frame captured before
     * FDT fired — NOT from this frame, which (post-FDT) already has a finger. */
    gint bm = swipe_raw_mean(self->empty_img);
    self->baseline_mean = bm > 500 ? bm : mean;
  }

  /* ---- Mode B: FDT-gated accumulation (opt-in via env GOODIX_SWIPE_FDT) ----
   * Re-arm FDT_DOWN between a fixed number of frames (user holds & slowly moves
   * / re-taps), stack edge-to-edge. Kept as a fallback; the DEFAULT is the
   * streaming swipe path below, which (with background subtraction + distinct-
   * frame subsampling + edge-to-edge stacking) reliably yields a tall, minutiae-
   * rich image that NBIS/bozorth matches (genuine ~62, impostor <=14). */
  if (g_getenv("GOODIX_SWIPE_FDT")) {
    if (!self->finger_seen) {
      self->finger_seen = TRUE;
      fpi_image_device_report_finger_status(img_dev, TRUE);
    }
    self->strips = g_slist_prepend(self->strips, swipe_make_stripe(out));
    self->strips_len++;
    fp_dbg("SWIPE-FDT frame %u/%d mean=%d\n", self->strips_len,
            GOODIX55X4_FDT_TARGET, mean);
    if (self->strips_len < GOODIX55X4_FDT_TARGET) {
      fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DOWN); /* next frame */
      return;
    }
    self->strips = g_slist_reverse(self->strips);
    guint idx = 0;
    for (GSList *l = self->strips; l; l = l->next, ++idx) {
      struct fpi_frame *f = l->data;
      f->delta_x = 0;
      f->delta_y = (idx == 0) ? 0 : GOODIX55X4_SWIPE_FRAME_H; /* edge-to-edge */
    }
    FpImage *bimg = fpi_assemble_frames(&swipe_asmbl_ctx, self->strips);
    bimg->ppmm = GOODIX55X4_PPMM;
    bimg->flags |= FPI_IMAGE_COLORS_INVERTED;
    fp_dbg("SWIPE-FDT assembled %u frames -> %dx%d\n", self->strips_len,
            fp_image_get_width(bimg), fp_image_get_height(bimg));
    const char *sd = g_getenv("GOODIX_SAVE_DIR");
    if (sd) {
      static guint dseq = 0;
      char *p = g_strdup_printf("%s/fdt_%05u.pgm", sd, dseq++);
      save_image_to_pgm(bimg, p);
      fp_dbg("SAVED %s\n", p);
      g_free(p);
    }
    swipe_reset(self);
    fpi_image_device_image_captured(img_dev, bimg);
    fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DONE);
    return;
  }

  /* A finger lowers the capacitive signal well below the no-finger baseline. */
  const gboolean finger_present =
      (mean < self->baseline_mean - GOODIX55X4_FINGER_DROP);

  /* ---- Phase 1: stream and wait for the finger to land ---- */
  if (!self->finger_seen) {
    if (finger_present) {
      self->finger_seen = TRUE;
      fpi_image_device_report_finger_status(img_dev, TRUE);
      memcpy(self->prev_out, out, sizeof(out));
      goodix_tls_read_image(dev, scan_on_read_img, ssm); /* skip touch frame */
      return;
    }
    /* Track the no-finger baseline, but adapt ONLY toward HIGHER means. A finger
     * only ever lowers the capacitive signal, so a symmetric EMA would chase a
     * slow finger placement downward and the (base - mean) delta would never
     * reach FINGER_DROP — exactly the bug that made slow swipes stream forever.
     * Tracking just the upper (no-finger) envelope keeps the threshold stable. */
    if (mean >= self->baseline_mean)
      self->baseline_mean = (self->baseline_mean * 7 + mean) / 8;
    if (self->swipe_frame_idx > 700) { /* ~30s safety cap (bounds thermal load) */
      swipe_reset(self);
      fpi_ssm_mark_failed(ssm, fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
      return;
    }
    /* Keep the LED lit per-frame while waiting (the visible "swipe now" cue). */
    goodix_send_set_led(dev, 0x00, led_then_read_img, ssm);
    return;
  }

  /* ---- Phase 2: finger is down, accumulate moving stripes ---- */
  gboolean finish = FALSE;

  if (!finger_present) {
    finish = TRUE; /* finger lifted -> end of swipe */
  } else {
    const guint diff = swipe_out_diff(out, self->prev_out);
    memcpy(self->prev_out, out, sizeof(out));

    if (diff > GOODIX55X4_SWIPE_STATIC_THRESHOLD) {
      if (!self->finger_moving && self->strips) {
        /* discard pre-movement strips captured while finger was settling */
        g_slist_free_full(self->strips, g_free);
        self->strips = NULL;
        self->strips_len = 0;
      }
      self->finger_moving = TRUE;
      self->static_count = 0;
    } else {
      self->static_count++;
      if (self->finger_moving &&
          self->static_count >= GOODIX55X4_SWIPE_STATIC_FRAMES) {
        /* drop the trailing static strips, the swipe has stopped */
        for (guint s = 0; s < self->static_count && self->strips; ++s) {
          g_free(self->strips->data);
          self->strips = g_slist_delete_link(self->strips, self->strips);
          self->strips_len--;
        }
        finish = TRUE;
      } else {
        /* still settling or briefly static: keep streaming, don't store */
        goodix_tls_read_image(dev, scan_on_read_img, ssm);
        return;
      }
    }

    if (!finish) {
      if (!self->finger_moving) {
        goodix_tls_read_image(dev, scan_on_read_img, ssm);
        return;
      }
      /* Subsample: keep this frame only if it is sufficiently DIFFERENT from the
       * last kept one, so stored stripes cover distinct area instead of blending
       * into motion-smear (the cause of the low minutiae yield). */
      const guint kdiff = self->have_kept
                              ? swipe_out_diff(out, self->last_kept_out)
                              : G_MAXUINT;
      if (kdiff >= GOODIX55X4_SWIPE_KEEP_STEP) {
        self->strips = g_slist_prepend(self->strips, swipe_make_stripe(out));
        self->strips_len++;
        memcpy(self->last_kept_out, out, sizeof(out));
        self->have_kept = TRUE;
      }
      if (self->strips_len < GOODIX55X4_SWIPE_MAX_FRAMES) {
        goodix_tls_read_image(dev, scan_on_read_img, ssm);
        return;
      }
      finish = TRUE; /* hit the cap */
    }
  }

  /* ---- swipe ended ---- */
  if (self->strips_len < GOODIX55X4_SWIPE_MIN_FRAMES) {
    /* too short to be usable: discard and keep streaming for another swipe */
    swipe_reset(self);
    if (++self->scan_retries > GOODIX55X4_MAX_SCAN_RETRIES) {
      fpi_ssm_mark_failed(ssm, fpi_device_error_new(FP_DEVICE_ERROR_GENERAL));
      return;
    }
    fp_info("swipe too short, waiting for another (retry %u)",
            self->scan_retries);
    fpi_image_device_report_finger_status(img_dev, FALSE);
    goodix_tls_read_image(dev, scan_on_read_img, ssm);
    return;
  }
  self->scan_retries = 0;

  self->strips = g_slist_reverse(self->strips);
  /* Stack the (subsampled, distinct) stripes EDGE-TO-EDGE. Cross-correlation
   * registration is unreliable once frames are spaced further than the small
   * overlap window — it collapses the image. Edge-to-edge stacking of distinct
   * frames is what the offline validation used and gives a tall, minutiae-rich
   * image (genuine bozorth ~62, impostor <=14). */
  guint idx = 0;
  for (GSList *l = self->strips; l; l = l->next, ++idx) {
    struct fpi_frame *f = l->data;
    f->delta_x = 0;
    f->delta_y = (idx == 0) ? 0 : GOODIX55X4_SWIPE_FRAME_H;
  }
  FpImage *img = fpi_assemble_frames(&swipe_asmbl_ctx, self->strips);
  img->ppmm = GOODIX55X4_PPMM;
  img->flags |= FPI_IMAGE_COLORS_INVERTED;
  fp_dbg("swipe assembled %u stripes -> %dx%d", self->strips_len,
         fp_image_get_width(img), fp_image_get_height(img));

  const char *save_dir = g_getenv("GOODIX_SAVE_DIR");
  if (save_dir) {
    static guint dump_seq = 0;
    char *p = g_strdup_printf("%s/swipe_%05u.pgm", save_dir, dump_seq++);
    save_image_to_pgm(img, p);
    fp_dbg("SAVED %s\n", p);
    g_free(p);
  }

  swipe_reset(self);
  fpi_image_device_image_captured(img_dev, img);
  fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DONE);
}

gboolean save_image_to_pgm2(guchar *data, const char *path) {
  FILE *fd = fopen(path, "w");
  size_t write_size = 7656;
  int r;

  if (!fd) {
    g_warning("could not open '%s' for writing: %d", path, errno);
    return FALSE;
  }

  r = fprintf(fd, "P2\n%d %d\n255\n", GOODIX55X4_WIDTH, GOODIX55X4_HEIGHT);
  if (r < 0) {
    fclose(fd);
    g_critical("pgm header write failed, error %d", r);
    return FALSE;
  }

  for (int i = 0; i < write_size; i += 1) {
    r = fprintf(fd, "%d\n", data[i]);
  }
  fclose(fd);
  g_debug("written to '%s'", path);

  return TRUE;
}

gboolean save_image_to_pgm(FpImage *img, const char *path) {
  FILE *fd = fopen(path, "w");
  size_t write_size;
  const guchar *data = fp_image_get_data(img, &write_size);
  int r;

  if (!fd) {
    g_warning("could not open '%s' for writing: %d", path, errno);
    return FALSE;
  }

  r = fprintf(fd, "P5 %d %d 255\n", fp_image_get_width(img),
              fp_image_get_height(img));
  if (r < 0) {
    fclose(fd);
    g_critical("pgm header write failed, error %d", r);
    return FALSE;
  }

  r = fwrite(data, 1, write_size, fd);
  if (r < write_size) {
    fclose(fd);
    g_critical("short write (%d)", r);
    return FALSE;
  }

  fclose(fd);
  g_debug("written to '%s'", path);

  return TRUE;
}

const guint8 fdt_switch_state_mode_55X4[] = {
    0x0d, 0x01, 0x80, 0x12, 0x80, 0xaf, 0x80, 0x9a, 0x80,
    0x87, 0x80, 0x12, 0x80, 0xa8, 0x80, 0x95, 0x80, 0x81,
    0x80, 0x12, 0x80, 0xa7, 0x80, 0x98, 0x80, 0x84};

const guint8 fdt_switch_state_mode2_55X4[] = {
    0x0d, 0x01, 0x80, 0xb3, 0x80, 0xc6, 0x80, 0xbc, 0x80,
    0xa8, 0x80, 0xb9, 0x80, 0xca, 0x80, 0xc2, 0x80, 0xab,
    0x80, 0xb7, 0x80, 0xc6, 0x80, 0xbc, 0x80, 0xa6};

const guint8 fdt_switch_state_down_55X4[] = {
    0x0c, 0x01, 0x80, 0xb1, 0x80, 0xc6, 0x80, 0xbc, 0x80,
    0xa6, 0x80, 0xb9, 0x80, 0xca, 0x80, 0xc2, 0x80, 0xab,
    0x80, 0xb7, 0x80, 0xc7, 0x80, 0xbc, 0x80, 0xa7};

const guint8 fdt_switch_state_up_55X4[] = {
    0x0e, 0x01, 0x80, 0x92, 0x80, 0x9d, 0x80, 0x93, 0x80,
    0x92, 0x80, 0x97, 0x80, 0x9e, 0x80, 0xa0, 0x80, 0x8e,
    0x80, 0xab, 0x80, 0xa5, 0x80, 0xb0, 0x80, 0x12};

enum scan_empty_img_state {
  /* Replicate the Python capture3 flow that DID return a calibration frame on
   * the 55a2: fdt_up + nav_0 before reading the (empty) image. Reading the
   * image straight after fdt_mode made the device answer with 0xd0. */
  SCAN_EMPTY_FDT_UP,
  SCAN_EMPTY_NAV,
  SCAN_EMPTY_GET_IMG,

  SCAN_EMPTY_NUM,
};

static void on_scan_empty_img(FpDevice *dev, guint8 *data, guint16 length,
                              gpointer ssm, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(ssm, error);
    return;
  }
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);
  decode_frame(self->empty_img, data);
  // FpImage *bgk = fp_image_new(GOODIX55X4_WIDTH, GOODIX55X4_HEIGHT);
  // squash_frame(self->empty_img, bgk->data);
  // save_image_to_pgm(bgk, "./background.pgm");
  fpi_ssm_next_state(ssm);
}
static void scan_empty_run(FpiSsm *ssm, FpDevice *dev) {

  switch (fpi_ssm_get_cur_state(ssm)) {

  case SCAN_EMPTY_FDT_UP:
    goodix_send_mcu_switch_to_fdt_up(dev, (guint8 *)fdt_switch_state_up_55X4,
                                     sizeof(fdt_switch_state_up_55X4), NULL,
                                     check_none_cmd, ssm);
    break;
  case SCAN_EMPTY_NAV:
    goodix_send_nav_0(dev, check_none_cmd, ssm);
    break;
  case SCAN_EMPTY_GET_IMG:
    goodix_tls_read_image(dev, on_scan_empty_img, ssm);
    break;
  }
}

static void scan_empty_img(FpDevice *dev, FpiSsm *ssm) {
  fpi_ssm_start_subsm(ssm, fpi_ssm_new(dev, scan_empty_run, SCAN_EMPTY_NUM));
}

static void scan_get_img(FpDevice *dev, FpiSsm *ssm) {
  goodix_tls_read_image(dev, scan_on_read_img, ssm);
}

static void scan_query_mcu_cb(FpDevice *dev, guint8 *mcu_state, guint16 len,
                              gpointer ssm, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(ssm, error);
    return;
  }
  GString *hx = g_string_new(NULL);
  for (guint16 i = 0; i < len; i++)
    g_string_append_printf(hx, "%02x", mcu_state[i]);
  /* Windows parse: byte[2] bit... shows isTlsConnected. Log raw so we can see
   * whether TLS actually took (compare to Windows 01023000003a0000...). */
  gboolean tls_connected = (len > 2) && (mcu_state[2] & 0x10);
  fp_dbg("MCU STATE: %s  -> isTlsConnected guess=%d\n", hx->str,
          tls_connected);
  g_string_free(hx, TRUE);
  fpi_ssm_next_state(ssm);
}

static void scan_run_state(FpiSsm *ssm, FpDevice *dev) {
  FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);

  switch (fpi_ssm_get_cur_state(ssm)) {
  case SCAN_STAGE_QUERY_MCU:
    fp_dbg("QUERY MCU STATE (confirm TLS connected, like Windows)\n");
    goodix_send_query_mcu_state(dev, scan_query_mcu_cb, ssm);
    break;
  case SCAN_STAGE_CALIBRATE:
    scan_empty_img(dev, ssm);
    break;
  case SCAN_STAGE_SWITCH_TO_FDT_MODE:
    fp_dbg("SWITCH TO FDT MODE\n");
    /* capture the per-zone baseline from the reply to build fdt_down */
    goodix_send_mcu_switch_to_fdt_mode(
        dev, (guint8 *)fdt_switch_state_mode_55X4,
        sizeof(fdt_switch_state_mode_55X4), NULL, fdt_mode_base_cb, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_DOWN: {
    fp_dbg("SWITCH TO FDT DOWN\n");
    FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);
    if (self->fdt_down_ready)
      goodix_send_mcu_switch_to_fdt_down(dev, self->fdt_down_dyn,
                                         self->fdt_down_len, NULL,
                                         check_none_cmd, ssm);
    else
      goodix_send_mcu_switch_to_fdt_down(
          dev, (guint8 *)fdt_switch_state_down_55X4,
          sizeof(fdt_switch_state_down_55X4), NULL, check_none_cmd, ssm);
    break;
  }
  case SCAN_STAGE_SET_LED:
    /* Turn on the orange capture LED so the user sees when to swipe. payload
     * 0x00 is what the Windows driver sends during active capture. */
    goodix_send_set_led(dev, 0x00, check_none, ssm);
    break;
  case SCAN_STAGE_GET_IMG:
    /* Start streaming frames for the swipe. Finger status is reported from
     * scan_on_read_img once the finger is actually detected in the image. */
    fp_dbg("SWITCH TO GET IMAGE (swipe streaming)\n");
    scan_get_img(dev, ssm);
    break;
  case SCAN_STAGE_SWITCH_TO_FDT_MODE2:
    fp_dbg("SWITCH TO FDT MODE 2\n");
    goodix_send_mcu_switch_to_fdt_mode(
        dev, (guint8 *)fdt_switch_state_mode2_55X4,
        sizeof(fdt_switch_state_mode2_55X4), NULL, check_none_cmd, ssm);
    break;
  case SCAN_STAGE_SWITCH_TO_FDT_UP_NO_REPLY:
    fp_dbg("SWITCH TO FDT UP NO REPLY\n");
    goodix_send_mcu_switch_to_fdt_up_no_reply(
        dev, (guint8 *)fdt_switch_state_up_55X4,
        sizeof(fdt_switch_state_up_55X4), NULL, check_none_cmd, ssm);
    break;
  case SCAN_STAGE_SWITCH_TO_FDT_UP:
    fp_dbg("SWITCH TO FDT UP\n");
    goodix_send_mcu_switch_to_fdt_up(dev, (guint8 *)fdt_switch_state_up_55X4,
                                     sizeof(fdt_switch_state_up_55X4), NULL,
                                     check_none_cmd, ssm);
    break;
  case SCAN_STAGE_SWITCH_TO_FDT_DONE:
    fpi_image_device_report_finger_status(img_dev, FALSE);
    break;
  }
}

static void sleep_run_state(FpiSsm *ssm, FpDevice *dev) {
  FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);

  switch (fpi_ssm_get_cur_state(ssm)) {
  case SLEEP_STAGE_SWITCH_TO_SLEEP_MODE:
    goodix_reset_state(dev);
    fp_dbg("SWITCH TO SLEEP MODE\n");
    goodix_send_mcu_switch_to_sleep_mode(dev, 20, check_idle, ssm);

    break;
  case SLEEP_STAGE_SWITCH_TO_SLEEP_MODE_REALTEK:
    fp_dbg("SWITCH TO SLEEP MODE REALTEK\n");
    goodix_send_mcu_switch_to_sleep_mode_realtek(dev, 0x6c, check_sleep_realtek, ssm);
    break;
  case SLEEP_STAGE_DEACTIVATE:
    fp_dbg("Deactivated\n");
    goodix_reset_state(dev);
//    goodix_cancel_receive(dev);
    GError *error = NULL;
    goodix_shutdown_tls(dev, &error);
    goodix55X4_reset_state(FPI_DEVICE_GOODIXTLS55X4(img_dev));
    fpi_image_device_deactivate_complete(img_dev, error);
    break;
  }  
}

static void write_sensor_complete(FpDevice *dev, gpointer user_data,
                                  GError *error) {
  if (error) {
    fp_err("failed to scan: %s (code: %d)", error->message, error->code);
    return;
  }
  scan_get_img(dev, user_data);
}

static void scan_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  if (error) {
    fp_err("failed to scan: %s (code: %d)", error->message, error->code);
    return;
  }
  fp_dbg("finished scan!");
  fp_dbg("finished scan");
}

static void sleep_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  if (error) {
    fp_err("failed to sleep: %s (code: %d)", error->message, error->code);
    return;
  }
  fp_dbg("finished sleep!");
  fp_dbg("finished sleep");
}

static void scan_start(FpiDeviceGoodixTls55X4 *dev) {
  dev->scan_retries = 0;
  swipe_reset(dev);
  fpi_ssm_start(fpi_ssm_new(FP_DEVICE(dev), scan_run_state, SCAN_STAGE_NUM),
                scan_complete);
}

static void sleep_start(FpDevice *dev, gpointer user_data) {
  fpi_ssm_start(fpi_ssm_new(dev, sleep_run_state, SLEEP_STAGE_NUM),
                sleep_complete);
}

// ---- SCAN SECTION END ----

// ---- DEV SECTION START ----

static void dev_init(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_init(dev, &error)) {
    fpi_image_device_open_complete(img_dev, error);
    return;
  }

  fpi_image_device_open_complete(img_dev, NULL);
}

static void dev_deinit(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_deinit(dev, &error)) {
    fpi_image_device_close_complete(img_dev, error);
    return;
  }

  fpi_image_device_close_complete(img_dev, NULL);
}

static void dev_activate(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);

  fpi_ssm_start(fpi_ssm_new(dev, activate_run_state, ACTIVATE_NUM_STATES),
                activate_complete);
}

static void dev_change_state(FpImageDevice *img_dev,
                             FpiImageDeviceState state) {
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(img_dev);
  G_DEBUG_HERE();

  switch (state) { 
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON: {
      scan_start(self);
      break;
    }
  }
}


static void dev_deactivate(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  fpi_device_add_timeout(dev, 250, sleep_start, NULL, NULL);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls55x4_init(FpiDeviceGoodixTls55X4 *self) {
  self->frames = g_slist_alloc();
}

static void
fpi_device_goodixtls55x4_class_init(FpiDeviceGoodixTls55X4Class *class) {
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  gx_class->interface = GOODIX_55X4_INTERFACE;
  gx_class->ep_in = GOODIX_55X4_EP_IN;
  gx_class->ep_out = GOODIX_55X4_EP_OUT;

  dev_class->id = "goodixtls55x4";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 55X4";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 6;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;

  /* This sensor is read-only streamed (same as the Windows driver, which polls
   * get_image continuously) and does not meaningfully heat from bulk reads. The
   * default simulated thermal model (180s active -> "hot") spuriously disables a
   * multi-stage swipe enroll, where we must stream while waiting for each swipe.
   * Raise the modeled hot time well above a full enroll session. */
  dev_class->temp_hot_seconds = 30 * 60;
  dev_class->temp_cold_seconds = 9 * 60;

  /* Swipe assembly produces a tall, minutiae-rich image. Live separation on this
   * hardware: genuine bozorth 30-62, impostor <=14. 24 sits in the gap with
   * margin both ways (fewer false rejects on short swipes, clear of impostors). */
  img_dev_class->bz3_threshold = 24;
  img_dev_class->algorithm = FPI_DEVICE_ALGO_NBIS;
  img_dev_class->img_width = GOODIX55X4_SWIPE_FRAME_W; /* 168 */
  img_dev_class->img_height = 0;                       /* variable */

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
}
