#include "csa_block.h"

#include <stdio.h>
#include <stdlib.h>

static void fill(float *x, size_t n, float scale)
{
    for (size_t i = 0; i < n; ++i) {
        x[i] = scale * (float)((int)(i % 17) - 8);
    }
}

int main(void)
{
    enum { C = 3, H = 4, W = 4, HW = H * W };

    float input[C * H * W];
    float output[C * H * W];

    float dw[C * 5 * 5];
    float dw_bias[C];
    float pw_c_hw[HW * C];
    float pw_c_hw_bias[HW];
    float pw_hw_c[C * HW];
    float pw_hw_c_bias[C];
    float pw_c_3c[(3 * C) * C];
    float pw_c_3c_bias[3 * C];
    float pw_3c_c[C * (3 * C)];
    float pw_3c_c_bias[C];

    fill(input, C * H * W, 0.05f);
    fill(dw, C * 5 * 5, 0.01f);
    fill(dw_bias, C, 0.001f);
    fill(pw_c_hw, HW * C, 0.01f);
    fill(pw_c_hw_bias, HW, 0.001f);
    fill(pw_hw_c, C * HW, 0.01f);
    fill(pw_hw_c_bias, C, 0.001f);
    fill(pw_c_3c, (3 * C) * C, 0.01f);
    fill(pw_c_3c_bias, 3 * C, 0.001f);
    fill(pw_3c_c, C * (3 * C), 0.01f);
    fill(pw_3c_c_bias, C, 0.001f);

    CsaBlockWeights weights = {
        .dw5x5_weight = dw,
        .dw5x5_bias = dw_bias,
        .pw_c_to_hw_weight = pw_c_hw,
        .pw_c_to_hw_bias = pw_c_hw_bias,
        .pw_hw_to_c_weight = pw_hw_c,
        .pw_hw_to_c_bias = pw_hw_c_bias,
        .pw_c_to_3c_weight = pw_c_3c,
        .pw_c_to_3c_bias = pw_c_3c_bias,
        .pw_3c_to_c_weight = pw_3c_c,
        .pw_3c_to_c_bias = pw_3c_c_bias,
    };

    const size_t scratch_len = csa_block_scratch_len(C, H, W);
    float *scratch = (float *)calloc(scratch_len, sizeof(*scratch));
    if (!scratch) {
        return 1;
    }

    const int rc = csa_block_forward(input, output, C, H, W, &weights,
                                     (CsaBlockScratch){scratch, scratch_len});
    free(scratch);

    if (rc != 0) {
        fprintf(stderr, "csa_block_forward failed: %d\n", rc);
        return 1;
    }

    for (int i = 0; i < 8; ++i) {
        printf("% .6f%s", output[i], i == 7 ? "\n" : " ");
    }

    return 0;
}
