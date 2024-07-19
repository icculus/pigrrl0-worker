// sudo apt-get install libi2c-dev wiringPi
// gcc -Wall -O3 -s -std=gnu99 -o pigrrl0-worker pigrrl0-worker.c -lwiringPi -lm

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <wiringPiI2C.h>

#include "hardcoded-images.h"

// stole these defines from https://github.com/adafruit/Adafruit_ADS1X15/blob/master/Adafruit_ADS1X15.h

#define ADS1X15_REG_CONFIG_CQUE_NONE                                           \
  (0x0003) ///< Disable the comparator and put ALERT/RDY in high state (default)

#define ADS1X15_REG_CONFIG_CLAT_NONLAT                                         \
  (0x0000) ///< Non-latching comparator (default)

#define ADS1X15_REG_CONFIG_CPOL_ACTVLOW                                        \
  (0x0000) ///< ALERT/RDY pin is low when active (default)

#define ADS1X15_REG_CONFIG_CMODE_TRAD                                          \
  (0x0000) ///< Traditional comparator with hysteresis (default)

#define ADS1X15_REG_CONFIG_MODE_SINGLE                                         \
  (0x0100) ///< Power-down single-shot mode (default)

#define ADS1X15_REG_CONFIG_MUX_SINGLE_0 (0x4000) ///< Single-ended AIN0
#define ADS1X15_REG_CONFIG_MUX_SINGLE_1 (0x5000) ///< Single-ended AIN1
#define ADS1X15_REG_CONFIG_MUX_SINGLE_2 (0x6000) ///< Single-ended AIN2
#define ADS1X15_REG_CONFIG_MUX_SINGLE_3 (0x7000) ///< Single-ended AIN3

#define ADS1X15_REG_CONFIG_PGA_4_096V (0x0200) ///< +/-4.096V range = Gain 1

#define ADS1X15_REG_CONFIG_DR_1600SPS (0x0080) ///< 1600 samples per second (default)

#define ADS1X15_REG_CONFIG_OS_SINGLE                                           \
  (0x8000) ///< Write: Set to start a single-conversion

#define ADS1X15_REG_POINTER_CONFIG (0x01)    ///< Configuration
#define ADS1X15_REG_POINTER_CONVERT (0x00)   ///< Conversion

#define ADS1X15_CONVERSIONDELAY 8

static inline uint16_t swap16(const uint16_t val)
{
    const uint8_t *u8 = (const uint8_t *) ((const char *) &val);
    return (((uint16_t) u8[0]) << 8) | ((uint16_t) u8[1]);
}

// a lot of this function came from https://forums.adafruit.com/viewtopic.php?t=48780
static uint16_t readADC_SingleEnded(int fd, uint8_t channel)
{
    uint16_t config = ADS1X15_REG_CONFIG_CQUE_NONE    | // Disable the comparator (default val)
                      ADS1X15_REG_CONFIG_CLAT_NONLAT  | // Non-latching (default val)
                      ADS1X15_REG_CONFIG_CPOL_ACTVLOW | // Alert/Rdy active low   (default val)
                      ADS1X15_REG_CONFIG_CMODE_TRAD   | // Traditional comparator (default val)
                      ADS1X15_REG_CONFIG_DR_1600SPS   | // 1600 samples per second (default)
                      ADS1X15_REG_CONFIG_MODE_SINGLE;   // Single-shot mode (default)

    config |= ADS1X15_REG_CONFIG_PGA_4_096V;

    // Set single-ended input channel
    switch (channel)
    {
        case (0):
            config |= ADS1X15_REG_CONFIG_MUX_SINGLE_0;
            break;
        case (1):
            config |= ADS1X15_REG_CONFIG_MUX_SINGLE_1;
            break;
        case (2):
            config |= ADS1X15_REG_CONFIG_MUX_SINGLE_2;
            break;
        case (3):
            config |= ADS1X15_REG_CONFIG_MUX_SINGLE_3;
            break;
    }

    // Set 'start single-conversion' bit
    config |= ADS1X15_REG_CONFIG_OS_SINGLE;

    // send the request, wait for the processing time, read the result.
    wiringPiI2CWriteReg16(fd, ADS1X15_REG_POINTER_CONFIG, swap16(config));
    usleep(ADS1X15_CONVERSIONDELAY * 1000);
    return swap16(wiringPiI2CReadReg16(fd, ADS1X15_REG_POINTER_CONVERT));
}

static uint64_t startup_ticks = 0;
static uint64_t get_ticks(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t ticks = (((uint64_t) now.tv_sec) * 1000) + (((uint64_t) now.tv_nsec) / 1000000);
    return ticks - startup_ticks;
}

static void init_ticks(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    startup_ticks = (((uint64_t) now.tv_sec) * 1000) + (((uint64_t) now.tv_nsec) / 1000000);
}


// overlay stuff is from https://github.com/The-Next-Guy/picotroller/blob/main/overlay.cpp
//  and is intended to interact with https://github.com/The-Next-Guy/fbcp-nexus/
#define SCREEN_WIDTH 320   // Example width, replace with actual
#define SCREEN_HEIGHT 240   // Example height, replace with actual

const key_t SHM_KEY_UPDATE = 1022; // Shared memory key for color buffer
const key_t SHM_KEY_COLOR = 1023; // Shared memory key for color buffer
const key_t SHM_KEY_TRANSPARENCY = SHM_KEY_COLOR + 20; // Shared memory key for transparency buffer

typedef struct ColorBuffer {
    uint16_t buffer[SCREEN_WIDTH * SCREEN_HEIGHT]; // 16-bit color depth per pixel
} ColorBuffer;

typedef struct TransparencyBuffer {
    uint8_t buffer[SCREEN_WIDTH * SCREEN_HEIGHT]; // 8-bit transparency per pixel
} TransparencyBuffer;

typedef struct Updater {
    bool update;
} Updater;

static int shmid_updater = -1;
static int shmid_color = -1;
static int shmid_transparency = -1;
static Updater* updater = NULL;
static ColorBuffer* color_buffer = NULL;
static TransparencyBuffer* transparency_buffer = NULL;

static int init_overlay(void)
{
    shmid_updater = shmget(SHM_KEY_UPDATE, sizeof(Updater), 0666 | IPC_CREAT);
    if (shmid_updater == -1) {
        perror("shmget (update)");
        return -1;
    }

    shmid_color = shmget(SHM_KEY_COLOR, sizeof(ColorBuffer), 0666 | IPC_CREAT);
    if (shmid_color == -1) {
        perror("shmget (color)");
        return -1;
    }

    shmid_transparency = shmget(SHM_KEY_TRANSPARENCY, sizeof(TransparencyBuffer), 0666 | IPC_CREAT);
    if (shmid_transparency == -1) {
        perror("shmget (transparency)");
        return -1;
    }

    // Attach to the shared memory segments
    updater = (Updater*)shmat(shmid_updater, NULL, 0);
    if (updater == (void*)-1) {
        perror("shmat (updater)");
        return -1;
    }

    color_buffer = (ColorBuffer*)shmat(shmid_color, NULL, 0);
    if (color_buffer == (void*)-1) {
        perror("shmat (color)");
        return -1;
    }

    transparency_buffer = (TransparencyBuffer*)shmat(shmid_transparency, NULL, 0);
    if (transparency_buffer == (void*)-1) {
        perror("shmat (transparency)");
        return -1;
    }

    memset(color_buffer, '\0', sizeof (*color_buffer));
    memset(transparency_buffer, '\0', sizeof (*transparency_buffer));
    updater->update = true;

    return 0;
}


static void clear_transparency(int x, int y, int w, int h)
{
    if ((x >= SCREEN_WIDTH) || (y >= SCREEN_HEIGHT)) {
        return;
    }

    if ((x + w) >= SCREEN_WIDTH) {
        w = (SCREEN_WIDTH - x) - 1;
    }

    if ((y + h) >= SCREEN_HEIGHT) {
        h = (SCREEN_HEIGHT - y) - 1;
    }

    if ((w <= 0) || (h <= 0)) {
        return;
    }

    uint8_t *dst = transparency_buffer->buffer + (y * SCREEN_WIDTH) + x;
    for (int i = 0; i < h; i++) {
        memset(dst, '\0', w);
        dst += SCREEN_WIDTH;
    }
}

static void draw_hardcoded_image(const HardcodedImage *img, int x, int y, bool set_transparency)
{
    if ((x >= SCREEN_WIDTH) || (y >= SCREEN_HEIGHT)) {
        return;
    }

    int w = img->width;
    int h = img->height;

    if ((x + w) >= SCREEN_WIDTH) {
        w = (SCREEN_WIDTH - x) - 1;
    }

    if ((y + h) >= SCREEN_HEIGHT) {
        h = (SCREEN_HEIGHT - y) - 1;
    }

    if ((w <= 0) || (h <= 0)) {
        return;
    }

    //if ((x + w) >= SCREEN_WIDTH) { printf("whoa, x=%d, w=%d, x+w=%d against screenw=%d !\n", x, w, x+w, SCREEN_WIDTH); }

    const int imgw = img->width;
    const uint16_t *csrc = img->rgb565;
    uint16_t *cdst = color_buffer->buffer + (y * SCREEN_WIDTH) + x;
    for (int i = 0; i < h; i++) {
        memcpy(cdst, csrc, w * sizeof (uint16_t));
        csrc += imgw;
        cdst += imgw;
    }

    if (set_transparency) {
        const uint8_t *tsrc = img->transparency;
        uint8_t *tdst = transparency_buffer->buffer + (y * SCREEN_WIDTH) + x;
        for (int i = 0; i < h; i++) {
            memcpy(tdst, tsrc, w * sizeof (uint16_t));
            tsrc += imgw;
            tdst += imgw;
        }
    }
}

static void draw_rectangle(int x, int y, int w, int h, uint16_t r, uint16_t g, uint16_t b)
{
    if ((x >= SCREEN_WIDTH) || (y >= SCREEN_HEIGHT)) {
        return;
    }

    if ((x + w) >= SCREEN_WIDTH) {
        w = (SCREEN_WIDTH - x) - 1;
    }

    if ((y + h) >= SCREEN_HEIGHT) {
        h = (SCREEN_HEIGHT - y) - 1;
    }

    if ((w <= 0) || (h <= 0)) {
        return;
    }

    uint16_t *cdst = color_buffer->buffer + (y * SCREEN_WIDTH) + x;
    const int dst_advance = SCREEN_WIDTH - w;
    const uint16_t rgb565 = ((r & 0b11111000) << 8) | ((g & 0b11111100) << 3) | (b >> 3);
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            *(cdst++) = rgb565;
        }
        cdst += dst_advance;
    }
}


#define IDLE_SLEEP_TICKS 200
static int sleepms = IDLE_SLEEP_TICKS;  // how much the mainloop sleeps per iteration. Drops lower when things are happening, goes higher again when idle.

typedef enum VolumeAnimationState
{
    VOLUME_ANIM_SLIDING_IN,
    VOLUME_ANIM_SHOWN,
    VOLUME_ANIM_SLIDING_OUT,
    VOLUME_ANIM_HIDDEN
} VolumeAnimationState;

static VolumeAnimationState volume_animation_state = VOLUME_ANIM_HIDDEN;  // off by default.
static int volume_slider_position = SCREEN_WIDTH;
static uint64_t volume_slider_animation_start_ticks = 0;
static int volume_slider_pct = 0;  // this is the volume's current level, not the animation.

#define VOLUME_SLIDER_ANIMATION_TIME 300
#define VOLUME_SLIDER_SHOWN_TIME 1000

static void update_overlay(void)
{
    if (updater->update) {
        return;  // still waiting for fbcp to pull in the previous frame, skip the update.
    } else if (volume_animation_state == VOLUME_ANIM_HIDDEN) {
        return;  // nothing to be done, we're idle.
    }

    const uint64_t now = get_ticks();
    if (volume_animation_state == VOLUME_ANIM_SLIDING_IN) {
         const uint64_t end = volume_slider_animation_start_ticks + VOLUME_SLIDER_ANIMATION_TIME;
         if (now >= end) {
             volume_slider_position = SCREEN_WIDTH - volume_slider.width;
             volume_animation_state = VOLUME_ANIM_SHOWN;
             volume_slider_animation_start_ticks = end;
             //printf("SLIDE IN COMPLETE\n");
         } else {
             const float pct = ((float) (end - now)) / ((float) VOLUME_SLIDER_ANIMATION_TIME);
             volume_slider_position = (SCREEN_WIDTH - volume_slider.width) + ((int) (volume_slider.width * pct));
         }
    } else if (volume_animation_state == VOLUME_ANIM_SHOWN) {
         const uint64_t end = volume_slider_animation_start_ticks + VOLUME_SLIDER_SHOWN_TIME;
         if (now >= end) {
             volume_animation_state = VOLUME_ANIM_SLIDING_OUT;
             volume_slider_animation_start_ticks = end;
             //printf("SLIDING OUT\n");
         }
    } else if (volume_animation_state == VOLUME_ANIM_SLIDING_OUT) {
         const uint64_t end = volume_slider_animation_start_ticks + VOLUME_SLIDER_ANIMATION_TIME;
         if (now >= end) {
             volume_slider_position = SCREEN_WIDTH;
             volume_animation_state = VOLUME_ANIM_HIDDEN;
             volume_slider_animation_start_ticks = 0;
             sleepms = IDLE_SLEEP_TICKS;   // we're officially idle again, slow down updates.
             //printf("SLIDE OUT COMPLETE\n");
         } else {
             const float pct = ((float) (end - now)) / ((float) VOLUME_SLIDER_ANIMATION_TIME);
             volume_slider_position = SCREEN_WIDTH - ((int) (volume_slider.width * pct));
         }
    }

    // draw the volume slider!
    const HardcodedImage *icon = NULL;
    if (volume_slider_pct == 0) {
        icon = &volume_mute;
    } else if (volume_slider_pct <= 35) {
        icon = &volume_low;
    } else if (volume_slider_pct <= 70) {
        icon = &volume_med;
    } else {
        icon = &volume_full;
    }

    const int full_slider_x = SCREEN_WIDTH - volume_slider.width;
    const int slider_x = volume_slider_position;
    const int slider_y = (SCREEN_HEIGHT - volume_slider.height) / 2;
    const int ICON_MARGIN = 5;
    const int bar_w = icon->width;
    const int bar_h = ((volume_slider.height - (ICON_MARGIN * 3)) - icon->height) - 15;  // -15 to fudge it
    const int bar_x = slider_x + ((volume_slider.width - bar_w) / 2);
    const int bar_y = slider_y + icon->height + (ICON_MARGIN * 2);
    const int used_bar_h = (int) (bar_h * (volume_slider_pct / 100.0f));
    const int unused_bar_h = bar_h - used_bar_h;
    clear_transparency(full_slider_x, slider_y, slider_x - full_slider_x, volume_slider.height);  // nuke any part of the area we aren't using.
    draw_hardcoded_image(&volume_slider, slider_x, slider_y, true);
    draw_hardcoded_image(icon, slider_x + ((volume_slider.width - icon->width) / 2) + 2, slider_y + ICON_MARGIN, false);  // +2 to fudge it
    draw_rectangle(bar_x, bar_y, bar_w, unused_bar_h, 0x00, 0x00, 0x00);
    draw_rectangle(bar_x, bar_y + unused_bar_h, bar_w, used_bar_h, 0xFF, 0xFF, 0x00);

    updater->update = true;
}

static void set_new_volume_animation(const int pct)
{
    //printf("NEW VOLUME %d%%\n", pct);

    // we need to slide in unless we're already there.
    if (volume_animation_state == VOLUME_ANIM_HIDDEN) {
        //printf("SLIDING IN\n");
        volume_animation_state = VOLUME_ANIM_SLIDING_IN;
        volume_slider_animation_start_ticks = get_ticks();
    } else if (volume_animation_state == VOLUME_ANIM_SHOWN) {
        volume_slider_animation_start_ticks = get_ticks();  // reset the timer so we stay shown as long as adjustments are still being made.
    } else if (volume_animation_state == VOLUME_ANIM_SLIDING_OUT) {
        //printf("SLIDING BACK IN\n");
        volume_animation_state = VOLUME_ANIM_SLIDING_IN;
        volume_slider_animation_start_ticks = get_ticks();

        // amount we have currently slid in: offset the start_ticks to reflect this.
        const int total_distance = volume_slider.width;
        const int distance = volume_slider_position - (SCREEN_WIDTH - total_distance);
        const float pct = ((float) distance) / ((float) total_distance);
        const int tick_offset = (int) (VOLUME_SLIDER_ANIMATION_TIME * pct);
        volume_slider_animation_start_ticks -= tick_offset;
    } // else if already sliding in, just keep on as you were.

    volume_slider_pct = pct;
    sleepms = 12;  // sleep less since dial motion is happening now. Remember that reading the ADC blocks for 8ms, and we want to draw animations, too.
}



static FILE *amixerpipe = NULL;

static void set_new_volume(int pct)
{
    static int first_time = 1;

    // sanity check this.
    if (pct > 100) {
        pct = 100;
    } else if (pct < 0) {
        pct = 0;
    }

    //printf("Setting volume to %d%%\n", pct);
    fprintf(amixerpipe, "set PCM %d%%\n", pct);
    fflush(amixerpipe);

    if (first_time) {
        first_time = 0;  // we're just setting the volume to start, don't show the UI.
    } else {
        set_new_volume_animation(pct);
    }
}

int main(int argc, char **argv)
{

    amixerpipe = popen("/usr/bin/amixer -s >/dev/null", "w");
    if (!amixerpipe) {
        fprintf(stderr, "Failed to pipe to amixer: %s\n", strerror(errno));
        return 1;
    }

    init_ticks();

    if (init_overlay() < 0) {
        pclose(amixerpipe);
        return 1;
    }

    const int DEVICE_ID = 0x48;
    const int fd = wiringPiI2CSetup(DEVICE_ID);
    if (fd == -1) {
        fprintf(stderr, "Failed to connect to i2c device!\n");
        pclose(amixerpipe);
        return 1;
    }

    //printf("fd == %d\n", fd);

    int lastpct = -1000;  // so we set the audio to a value on the first iteration.

    while (1) {
        const uint16_t val = readADC_SingleEnded(fd, 0);
        float voltage = (val * 4.096f) / 32767;

        if (voltage > 3.25f) {
            voltage = 3.3f;  // Sometimes its a little more than 3.3, but clamp lower anyhow so we can definitely max out at 100%.
        } else if (voltage < 0.01f) {
            voltage = 0.0f; // Sometimes it hovers a little above zero, bump it so we can have a total muting.
        }

        const int pct = roundf((voltage / 3.3f) * 100.0f);

        //printf("val=%u, voltage=%f, pct=%d\n", (unsigned int) val, voltage, pct);

        if (pct != lastpct) {
            lastpct = pct;
            set_new_volume(pct);
        }

        update_overlay();

        if (sleepms) {
            usleep(sleepms * 1000);
        }
    }

    close(fd);
    pclose(amixerpipe);
    return 0;
}

