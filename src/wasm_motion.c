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

#define MD_MAX_MOTION_BOXES 6

typedef struct MotionDetector {
    int width;
    int height;
    int pixels;
    float threshold;
    int has_previous;
    float score;
    float level;
    int changed_pixels;
    int motion_left;
    int motion_top;
    int motion_right;
    int motion_bottom;
    int motion_box_count;
    int motion_box_left[MD_MAX_MOTION_BOXES];
    int motion_box_top[MD_MAX_MOTION_BOXES];
    int motion_box_right[MD_MAX_MOTION_BOXES];
    int motion_box_bottom[MD_MAX_MOTION_BOXES];
    int motion_box_area[MD_MAX_MOTION_BOXES];

    unsigned char *rgba;
    float *input;
    float *features;
    float *previous;
    float *previous_input;
    float *scratch;
    unsigned char *raw_changed;
    int *component_queue;
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
    free(detector->previous_input);
    free(detector->scratch);
    free(detector->raw_changed);
    free(detector->component_queue);
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

static float raw_motion_threshold(const MotionDetector *detector)
{
    float threshold = detector->threshold * 0.55f;
    if (threshold < 0.025f) {
        threshold = 0.025f;
    }
    if (threshold > 0.12f) {
        threshold = 0.12f;
    }
    return threshold;
}

static void clear_motion_bounds(MotionDetector *detector)
{
    detector->motion_left = 0;
    detector->motion_top = 0;
    detector->motion_right = 0;
    detector->motion_bottom = 0;
    detector->motion_box_count = 0;
    memset(detector->motion_box_left, 0, sizeof(detector->motion_box_left));
    memset(detector->motion_box_top, 0, sizeof(detector->motion_box_top));
    memset(detector->motion_box_right, 0, sizeof(detector->motion_box_right));
    memset(detector->motion_box_bottom, 0, sizeof(detector->motion_box_bottom));
    memset(detector->motion_box_area, 0, sizeof(detector->motion_box_area));
}

static void add_motion_box(MotionDetector *detector,
                           int count,
                           int min_x,
                           int min_y,
                           int max_x,
                           int max_y)
{
    if (count < 2) {
        return;
    }

    int insert_at = detector->motion_box_count;
    while (insert_at > 0 && count > detector->motion_box_area[insert_at - 1]) {
        --insert_at;
    }

    if (insert_at >= MD_MAX_MOTION_BOXES) {
        return;
    }

    const int last = detector->motion_box_count < MD_MAX_MOTION_BOXES - 1
                         ? detector->motion_box_count
                         : MD_MAX_MOTION_BOXES - 1;
    for (int i = last; i > insert_at; --i) {
        detector->motion_box_left[i] = detector->motion_box_left[i - 1];
        detector->motion_box_top[i] = detector->motion_box_top[i - 1];
        detector->motion_box_right[i] = detector->motion_box_right[i - 1];
        detector->motion_box_bottom[i] = detector->motion_box_bottom[i - 1];
        detector->motion_box_area[i] = detector->motion_box_area[i - 1];
    }

    detector->motion_box_left[insert_at] = min_x;
    detector->motion_box_top[insert_at] = min_y;
    detector->motion_box_right[insert_at] = max_x;
    detector->motion_box_bottom[insert_at] = max_y;
    detector->motion_box_area[insert_at] = count;

    if (detector->motion_box_count < MD_MAX_MOTION_BOXES) {
        ++detector->motion_box_count;
    }

    detector->motion_left = detector->motion_box_left[0];
    detector->motion_top = detector->motion_box_top[0];
    detector->motion_right = detector->motion_box_right[0];
    detector->motion_bottom = detector->motion_box_bottom[0];
}

static int boxes_are_close(const MotionDetector *detector, int a, int b)
{
    const int merge_gap = 2;

    return detector->motion_box_left[a] <= detector->motion_box_right[b] + merge_gap &&
           detector->motion_box_right[a] + merge_gap >= detector->motion_box_left[b] &&
           detector->motion_box_top[a] <= detector->motion_box_bottom[b] + merge_gap &&
           detector->motion_box_bottom[a] + merge_gap >= detector->motion_box_top[b];
}

static void remove_motion_box(MotionDetector *detector, int index)
{
    for (int i = index; i + 1 < detector->motion_box_count; ++i) {
        detector->motion_box_left[i] = detector->motion_box_left[i + 1];
        detector->motion_box_top[i] = detector->motion_box_top[i + 1];
        detector->motion_box_right[i] = detector->motion_box_right[i + 1];
        detector->motion_box_bottom[i] = detector->motion_box_bottom[i + 1];
        detector->motion_box_area[i] = detector->motion_box_area[i + 1];
    }

    if (detector->motion_box_count > 0) {
        --detector->motion_box_count;
    }
}

static void sort_motion_boxes(MotionDetector *detector)
{
    for (int i = 0; i < detector->motion_box_count; ++i) {
        int best = i;
        for (int j = i + 1; j < detector->motion_box_count; ++j) {
            if (detector->motion_box_area[j] > detector->motion_box_area[best]) {
                best = j;
            }
        }

        if (best != i) {
            const int left = detector->motion_box_left[i];
            const int top = detector->motion_box_top[i];
            const int right = detector->motion_box_right[i];
            const int bottom = detector->motion_box_bottom[i];
            const int area = detector->motion_box_area[i];

            detector->motion_box_left[i] = detector->motion_box_left[best];
            detector->motion_box_top[i] = detector->motion_box_top[best];
            detector->motion_box_right[i] = detector->motion_box_right[best];
            detector->motion_box_bottom[i] = detector->motion_box_bottom[best];
            detector->motion_box_area[i] = detector->motion_box_area[best];

            detector->motion_box_left[best] = left;
            detector->motion_box_top[best] = top;
            detector->motion_box_right[best] = right;
            detector->motion_box_bottom[best] = bottom;
            detector->motion_box_area[best] = area;
        }
    }
}

static void finalize_motion_boxes(MotionDetector *detector)
{
    int merged = 1;
    while (merged) {
        merged = 0;

        for (int i = 0; i < detector->motion_box_count && !merged; ++i) {
            for (int j = i + 1; j < detector->motion_box_count; ++j) {
                if (!boxes_are_close(detector, i, j)) {
                    continue;
                }

                if (detector->motion_box_left[j] < detector->motion_box_left[i]) {
                    detector->motion_box_left[i] = detector->motion_box_left[j];
                }
                if (detector->motion_box_top[j] < detector->motion_box_top[i]) {
                    detector->motion_box_top[i] = detector->motion_box_top[j];
                }
                if (detector->motion_box_right[j] > detector->motion_box_right[i]) {
                    detector->motion_box_right[i] = detector->motion_box_right[j];
                }
                if (detector->motion_box_bottom[j] > detector->motion_box_bottom[i]) {
                    detector->motion_box_bottom[i] = detector->motion_box_bottom[j];
                }

                detector->motion_box_area[i] += detector->motion_box_area[j];
                remove_motion_box(detector, j);
                merged = 1;
                break;
            }
        }
    }

    sort_motion_boxes(detector);

    if (detector->motion_box_count > 0) {
        detector->motion_left = detector->motion_box_left[0];
        detector->motion_top = detector->motion_box_top[0];
        detector->motion_right = detector->motion_box_right[0];
        detector->motion_bottom = detector->motion_box_bottom[0];
    } else {
        detector->motion_left = 0;
        detector->motion_top = 0;
        detector->motion_right = 0;
        detector->motion_bottom = 0;
    }
}

static void update_raw_motion_bounds(MotionDetector *detector)
{
    const int width = detector->width;
    const int height = detector->height;
    const int pixels = detector->pixels;
    const float threshold = raw_motion_threshold(detector);

    memset(detector->raw_changed, 0, (size_t)pixels * sizeof(*detector->raw_changed));
    clear_motion_bounds(detector);

    for (int i = 0; i < pixels; ++i) {
        float diff = detector->input[i] - detector->previous_input[i];
        if (diff < 0.0f) {
            diff = -diff;
        }

        if (diff > threshold) {
            detector->raw_changed[i] = 1;
        }
    }

    for (int start = 0; start < pixels; ++start) {
        if (detector->raw_changed[start] != 1) {
            continue;
        }

        int head = 0;
        int tail = 0;
        int count = 0;
        int min_x = start % width;
        int min_y = start / width;
        int max_x = min_x;
        int max_y = min_y;

        detector->raw_changed[start] = 2;
        detector->component_queue[tail++] = start;

        while (head < tail) {
            const int idx = detector->component_queue[head++];
            const int x = idx % width;
            const int y = idx / width;
            const int neighbors[4] = {
                x > 0 ? idx - 1 : -1,
                x + 1 < width ? idx + 1 : -1,
                y > 0 ? idx - width : -1,
                y + 1 < height ? idx + width : -1,
            };

            ++count;
            if (x < min_x) {
                min_x = x;
            }
            if (y < min_y) {
                min_y = y;
            }
            if (x > max_x) {
                max_x = x;
            }
            if (y > max_y) {
                max_y = y;
            }

            for (int n = 0; n < 4; ++n) {
                const int next = neighbors[n];
                if (next >= 0 && detector->raw_changed[next] == 1) {
                    detector->raw_changed[next] = 2;
                    detector->component_queue[tail++] = next;
                }
            }
        }

        add_motion_box(detector, count, min_x, min_y, max_x, max_y);
    }

    finalize_motion_boxes(detector);
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
    detector->previous_input = (float *)calloc((size_t)detector->pixels, sizeof(float));
    detector->scratch = (float *)calloc(detector->scratch_len, sizeof(float));
    detector->raw_changed =
        (unsigned char *)calloc((size_t)detector->pixels, sizeof(unsigned char));
    detector->component_queue = (int *)calloc((size_t)detector->pixels, sizeof(int));

    if (!detector->rgba || !detector->input || !detector->features ||
        !detector->previous || !detector->previous_input || !detector->scratch ||
        !detector->raw_changed || !detector->component_queue ||
        !init_weights(detector)) {
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
        memcpy(detector->previous_input, detector->input,
               (size_t)detector->pixels * sizeof(float));
        detector->has_previous = 1;
        detector->score = 0.0f;
        detector->level = 0.0f;
        detector->changed_pixels = 0;
        clear_motion_bounds(detector);
        return 0;
    }

    update_raw_motion_bounds(detector);

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
        detector->previous_input[i] = detector->input[i];
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

WASM_EXPORT
int md_motion_left(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->motion_left : 0;
}

WASM_EXPORT
int md_motion_top(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->motion_top : 0;
}

WASM_EXPORT
int md_motion_right(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->motion_right : 0;
}

WASM_EXPORT
int md_motion_bottom(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->motion_bottom : 0;
}

WASM_EXPORT
int md_motion_box_count(void *handle)
{
    MotionDetector *detector = (MotionDetector *)handle;
    return detector ? detector->motion_box_count : 0;
}

WASM_EXPORT
int md_motion_box_left(void *handle, int index)
{
    MotionDetector *detector = (MotionDetector *)handle;
    if (!detector || index < 0 || index >= detector->motion_box_count) {
        return 0;
    }
    return detector->motion_box_left[index];
}

WASM_EXPORT
int md_motion_box_top(void *handle, int index)
{
    MotionDetector *detector = (MotionDetector *)handle;
    if (!detector || index < 0 || index >= detector->motion_box_count) {
        return 0;
    }
    return detector->motion_box_top[index];
}

WASM_EXPORT
int md_motion_box_right(void *handle, int index)
{
    MotionDetector *detector = (MotionDetector *)handle;
    if (!detector || index < 0 || index >= detector->motion_box_count) {
        return 0;
    }
    return detector->motion_box_right[index];
}

WASM_EXPORT
int md_motion_box_bottom(void *handle, int index)
{
    MotionDetector *detector = (MotionDetector *)handle;
    if (!detector || index < 0 || index >= detector->motion_box_count) {
        return 0;
    }
    return detector->motion_box_bottom[index];
}
