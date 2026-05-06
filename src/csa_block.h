#ifndef CSA_BLOCK_H
#define CSA_BLOCK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CsaBlockWeights {
    /* Depth-wise 5x5 convolution: [channels, 5, 5]. */
    const float *dw5x5_weight;
    const float *dw5x5_bias;

    /* Point-wise projections use row-major [out_channels, in_channels]. */
    const float *pw_c_to_hw_weight;
    const float *pw_c_to_hw_bias;

    const float *pw_hw_to_c_weight;
    const float *pw_hw_to_c_bias;

    const float *pw_c_to_3c_weight;
    const float *pw_c_to_3c_bias;

    const float *pw_3c_to_c_weight;
    const float *pw_3c_to_c_bias;
} CsaBlockWeights;

typedef struct CsaBlockScratch {
    float *buffer;
    size_t len;
} CsaBlockScratch;

size_t csa_block_scratch_len(int channels, int height, int width);

/*
 * Runs the convolutional self-attention block shown in the diagram.
 *
 * Tensor layout is NCHW without a batch dimension: tensor[c][h][w] is stored at
 * ((c * height) + h) * width + w. The output buffer may alias input.
 *
 * Returns 0 on success, -1 for invalid arguments, and -2 when scratch is too
 * small.
 */
int csa_block_forward(const float *input,
                      float *output,
                      int channels,
                      int height,
                      int width,
                      const CsaBlockWeights *weights,
                      CsaBlockScratch scratch);

#ifdef __cplusplus
}
#endif

#endif
