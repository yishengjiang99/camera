#include "wasm_motion.h"

#include "csa_block.h"

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

typedef struct MotionDetector {
    int width;
    int height;
    int pixels;
    float threshold;
    int has_previous;
    float score;
    float level;
    int changed_pixels;

    unsigned char *rgba;
    float *input;
    float *features;
    float *previous;
    float *scratch;
    size_t scratch_len;

    float *dw5x5_weight;
    float *pw_c_to_hw_weight;
    float *pw_hw_to_c_weight;
    float *pw_c_to_3c_weight;
    float *pw_3c_to_c_weight;
    CsaBlockWeights weights;
} MotionDetector;

static void free_detector(MotionDetector *detector)
{
    if (!detector) {
        return;
    }

    free(detector->rgba);
    free(detector->input);
    free(detector->features);
    free(detector->previous);
    free(detector->scratch);
    free(detector->dw5x5_weight);
    free(detector->pw_c_to_hw_weight);
    free(detector->pw_hw_to_c_weight);
    free(detector->pw_c_to_3c_weight);
    free(detector->pw_3c_to_c_weight);
    free(detector);
}

static int init_weights(MotionDetector *detector)
{
    const int hw = detector->pixels;

    detector->dw5x5_weight = (float *)calloc(25, sizeof(float));
    detector->pw_c_to_hw_weight = (float *)calloc((size_t)hw, sizeof(float));
    detector->pw_hw_to_c_weight = (float *)calloc((size_t)hw, sizeof(float));
    detector->pw_c_to_3c_weight = (float *)calloc(3, sizeof(float));
    detector->pw_3c_to_c_weight = (float *)calloc(3, sizeof(float));

    if (!detector->dw5x5_weight || !detector->pw_c_to_hw_weight ||
        !detector->pw_hw_to_c_weight || !detector->pw_c_to_3c_weight ||
        !detector->pw_3c_to_c_weight) {
        return 0;
    }

    for (int i = 0; i < 25; ++i) {
        detector->dw5x5_weight[i] = 1.0f / 25.0f;
    }

    for (int i = 0; i < hw; ++i) {
        detector->pw_c_to_hw_weight[i] = 1.0f;
        detector->pw_hw_to_c_weight[i] = 1.0f / (float)hw;
    }

    detector->pw_c_to_3c_weight[0] = 1.0f;
    detector->pw_3c_to_c_weight[0] = 1.0f;

    memset(&detector->weights, 0, sizeof(detector->weights));
    detector->weights.dw5x5_weight = detector->dw5x5_weight;
    detector->weights.pw_c_to_hw_weight = detector->pw_c_to_hw_weight;
    detector->weights.pw_hw_to_c_weight = detector->pw_hw_to_c_weight;
    detector->weights.pw_c_to_3c_weight = detector->pw_c_to_3c_weight;
    detector->weights.pw_3c_to_c_weight = detector->pw_3c_to_c_weight;

    return 1;
}

WASM_EXPORT
void *md_create(int width, int height, float threshold)
{
    if (width <= 0 || height <= 0 || width * height > 4096) {
        return NULL;
    }

    MotionDetector *detector = (MotionDetector *)calloc(1, sizeof(*detector));
    if (!detector) {
        return NULL;
    }

    detector->width = width;
    detector->height = height;
    detector->pixels = width * height;
    detector->threshold = threshold > 0.0f ? threshold : 0.08f;
    detector->scratch_len = csa_block_scratch_len(1, height, width);

    detector->rgba =
        (unsigned char *)malloc((size_t)detector->pixels * 4 * sizeof(unsigned char));
    detector->input = (float *)calloc((size_t)detector->pixels, sizeof(float));
    detector->features = (float *)calloc((size_t)detector->pixels, sizeof(float));
    detector->previous = (float *)calloc((size_t)detector->pixels, sizeof(float));
    detector->scratch = (float *)calloc(detector->scratch_len, sizeof(float));

    if (!detector->rgba || !detector->input || !detector->features ||
        !detector->previous || !detector->scratch || !init_weights(detector)) {
        free_detector(detector);
        return NULL;
    }

    return detector;
}

WASM_EXPORT
void md_destroy(void *handle)
{
    free_detector((MotionDetector *)handle);
}

WASM_EXPORT
unsigned char *md_frame_ptr(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->rgba : NULL;
}

WASM_EXPORT
int md_frame_bytes(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->pixels * 4 : 0;
}

WASM_EXPORT
int md_process_rgba(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    if (!detector) {
        return -1;
    }

    for (int i = 0; i < detector->pixels; ++i) {
        const unsigned char *px = detector->rgba + (size_t)i * 4;
        detector->input[i] =
            (0.2126f * (float)px[0] + 0.7152f * (float)px[1] +
             0.0722f * (float)px[2]) /
            255.0f;
    }

    const int rc = csa_block_forward(
        detector->input, detector->features, 1, detector->height, detector->width,
        &detector->weights,
        (CsaBlockScratch){detector->scratch, detector->scratch_len});

    if (rc != 0) {
        return rc;
    }

    if (!detector->has_previous) {
        memcpy(detector->previous, detector->features,
               (size_t)detector->pixels * sizeof(float));
        detector->has_previous = 1;
        detector->score = 0.0f;
        detector->level = 0.0f;
        detector->changed_pixels = 0;
        return 0;
    }

    float sum = 0.0f;
    int changed = 0;
    for (int i = 0; i < detector->pixels; ++i) {
        float diff = detector->features[i] - detector->previous[i];
        if (diff < 0.0f) {
            diff = -diff;
        }

        sum += diff;
        if (diff > detector->threshold) {
            ++changed;
        }

        detector->previous[i] = detector->features[i];
    }

    detector->score = sum / (float)detector->pixels;
    detector->changed_pixels = changed;
    detector->level = (float)changed / (float)detector->pixels;

    return changed > detector->pixels / 25 ? 1 : 0;
}

WASM_EXPORT
void md_set_threshold(void *handle, float threshold)
{
    MotionDetector *detector = (MotionDetector *)handle;
    if (detector && threshold > 0.0f) {
        detector->threshold = threshold;
    }
}

WASM_EXPORT
float md_score(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->score : 0.0f;
}

WASM_EXPORT
float md_level(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->level : 0.0f;
}

WASM_EXPORT
int md_changed_pixels(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->changed_pixels : 0;
}
