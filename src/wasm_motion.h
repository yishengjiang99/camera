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
float md_score(void *handle);
float md_level(void *handle);
int md_changed_pixels(void *handle);

#ifdef __cplusplus
}
#endif

#endif
