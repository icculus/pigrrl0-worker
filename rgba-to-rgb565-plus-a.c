#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "hardcoded-images.h"

static void convert_hardcoded_image(const char *name, const HardcodedImage *img)
{
    const int pixel_count = img->width * img->height;
    uint16_t *color = (uint16_t *) malloc(pixel_count * 2);
    uint8_t *transparency = (uint8_t *) malloc(pixel_count);
    const uint8_t *src = img->pixel_data;
    for (int i = 0; i < pixel_count; i++) {
        const uint16_t r = (uint16_t) src[0];
        const uint16_t g = (uint16_t) src[1];
        const uint16_t b = (uint16_t) src[2];
        const uint16_t a = (uint16_t) src[3];
        src += 4;
        color[i] = ((r & 0b11111000) << 8) | ((g & 0b11111100) << 3) | (b >> 3);
        transparency[i] = a;
    }

    int counter;

    printf("static const uint16_t %s_color_data[] = {\n    ", name);
    counter = 0;
    for (int i = 0; i < pixel_count; i++) {
        printf("0x%04X", (unsigned int) color[i]);
        if (i < (pixel_count-1)) {
            printf(",");
            if (++counter >= 15) {
                counter = 0;
                printf("\n    ");
            } else {
                printf(" ");
            }
        }
    }
    printf("\n};\n\n");

    printf("static const uint16_t %s_transparency_data[] = {\n    ", name);
    counter = 0;
    for (int i = 0; i < pixel_count; i++) {
        printf("0x%02X", (unsigned int) transparency[i]);
        if (i < (pixel_count-1)) {
            printf(",");
            if (++counter >= 20) {
                counter = 0;
                printf("\n    ");
            } else {
                printf(" ");
            }
        }
    }
    printf("\n};\n\n");

    printf("static const HardcodedImage %s = { %d, %d, %s_color_data, %s_transparency_data };\n\n", name, img->width, img->height, name, name);

    free(color);
    free(transparency);
}


int main(void)
{
    printf("#ifndef INCL_HARDCODED_IMAGES_H\n"
           "#define INCL_HARDCODED_IMAGES_H\n"
           "\n"
           "typedef struct HardcodedImage {\n"
           "    unsigned int width;\n"
           "    unsigned int height;\n"
           "    const uint16_t *rgb565;\n"
           "    const uint8_t *transparency;\n"
           "} HardcodedImage;\n"
           "\n"
           "\n"
           "// images were edited and converted to C arrays from https://github.com/The-Next-Guy/picotroller (in the assets directory).\n"
           "\n");

    #define IMG(x) convert_hardcoded_image(#x, &x);
    IMG(volume_slider);
    IMG(volume_mute);
    IMG(volume_low);
    IMG(volume_med);
    IMG(volume_full);
    #undef IMG

    printf("#endif\n\n");

    return 0;
}

