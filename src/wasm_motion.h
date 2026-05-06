#ifndef WASM_MOTION_H
#define WASM_MOTION_H

#ifdef __cplusplus
extern "C" {
#endif

void *md_create(int width, int height, float threshold);
void md_destroy(void *handle);
unsigned char *md_frame_ptr(void *handle);
int md_frame_bytes(void *handle);
int md_process_rgba(void *handle);
void md_set_threshold(void *handle, float threshold);
void md_set_box_options(void *handle, int min_blob_size, int merge_gap, int max_boxes);
float md_score(void *handle);
float md_level(void *handle);
int md_changed_pixels(void *handle);
int md_motion_left(void *handle);
int md_motion_top(void *handle);
int md_motion_right(void *handle);
int md_motion_bottom(void *handle);
int md_motion_box_count(void *handle);
int md_motion_box_left(void *handle, int index);
int md_motion_box_top(void *handle, int index);
int md_motion_box_right(void *handle, int index);
int md_motion_box_bottom(void *handle, int index);

#ifdef __cplusplus
}
#endif

#endif
