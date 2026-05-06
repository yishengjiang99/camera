#include "csa_block.h"

#include <math.h>
#include <string.h>

#define CSA_DW_KERNEL 5
#define CSA_DW_RADIUS 2

static size_t tensor_len(int channels, int height, int width)
{
    return (size_t)channels * (size_t)height * (size_t)width;
}

static size_t chw_index(int c, int h, int w, int height, int width)
{
    return ((size_t)c * (size_t)height + (size_t)h) * (size_t)width + (size_t)w;
}

static float sigmoidf_stable(float x)
{
    if (x >= 0.0f) {
        const float z = expf(-x);
        return 1.0f / (1.0f + z);
    }

    const float z = expf(x);
    return z / (1.0f + z);
}

static void depthwise_conv5x5_same(const float *input,
                                   float *output,
                                   int channels,
                                   int height,
                                   int width,
                                   const float *weight,
                                   const float *bias)
{
    for (int c = 0; c < channels; ++c) {
        for (int h = 0; h < height; ++h) {
            for (int w = 0; w < width; ++w) {
                float acc = bias ? bias[c] : 0.0f;

                for (int kh = 0; kh < CSA_DW_KERNEL; ++kh) {
                    const int ih = h + kh - CSA_DW_RADIUS;
                    if (ih < 0 || ih >= height) {
                        continue;
                    }

                    for (int kw = 0; kw < CSA_DW_KERNEL; ++kw) {
                        const int iw = w + kw - CSA_DW_RADIUS;
                        if (iw < 0 || iw >= width) {
                            continue;
                        }

                        const size_t x_idx = chw_index(c, ih, iw, height, width);
                        const size_t k_idx =
                            ((size_t)c * CSA_DW_KERNEL + (size_t)kh) * CSA_DW_KERNEL +
                            (size_t)kw;
                        acc += input[x_idx] * weight[k_idx];
                    }
                }

                output[chw_index(c, h, w, height, width)] = acc;
            }
        }
    }
}

static void pointwise_conv1x1(const float *input,
                              float *output,
                              int in_channels,
                              int out_channels,
                              int height,
                              int width,
                              const float *weight,
                              const float *bias)
{
    for (int h = 0; h < height; ++h) {
        for (int w = 0; w < width; ++w) {
            for (int oc = 0; oc < out_channels; ++oc) {
                float acc = bias ? bias[oc] : 0.0f;

                for (int ic = 0; ic < in_channels; ++ic) {
                    acc += input[chw_index(ic, h, w, height, width)] *
                           weight[(size_t)oc * (size_t)in_channels + (size_t)ic];
                }

                output[chw_index(oc, h, w, height, width)] = acc;
            }
        }
    }
}

size_t csa_block_scratch_len(int channels, int height, int width)
{
    if (channels <= 0 || height <= 0 || width <= 0) {
        return 0;
    }

    const int hw = height * width;
    const size_t c_hw = tensor_len(channels, height, width);
    const size_t hw_hw = tensor_len(hw, height, width);
    const size_t c3_hw = tensor_len(3 * channels, height, width);

    return c_hw + c_hw + hw_hw + hw_hw + hw_hw + c_hw + c_hw + c3_hw;
}

int csa_block_forward(const float *input,
                      float *output,
                      int channels,
                      int height,
                      int width,
                      const CsaBlockWeights *weights,
                      CsaBlockScratch scratch)
{
    if (!input || !output || !weights || !scratch.buffer || channels <= 0 ||
        height <= 0 || width <= 0) {
        return -1;
    }

    if (!weights->dw5x5_weight || !weights->pw_c_to_hw_weight ||
        !weights->pw_hw_to_c_weight || !weights->pw_c_to_3c_weight ||
        !weights->pw_3c_to_c_weight) {
        return -1;
    }

    const int hw = height * width;
    const size_t c_hw = tensor_len(channels, height, width);
    const size_t hw_hw = tensor_len(hw, height, width);
    const size_t need = csa_block_scratch_len(channels, height, width);

    if (scratch.len < need) {
        return -2;
    }

    float *residual = scratch.buffer;
    float *v = residual + c_hw;
    float *q = v + c_hw;
    float *k = q + hw_hw;
    float *qk_hw = k + hw_hw;
    float *qk_c = qk_hw + hw_hw;
    float *qkv = qk_c + c_hw;
    float *expanded = qkv + c_hw;

    /*
     * Diagram path:
     * input -> DwConv5x5 gives V, then PwConv(C, HW) gives Q.
     * Flatten/transpose/reshape Q gives K.
     */
    memcpy(residual, input, c_hw * sizeof(*residual));

    depthwise_conv5x5_same(input, v, channels, height, width,
                           weights->dw5x5_weight, weights->dw5x5_bias);
    pointwise_conv1x1(v, q, channels, hw, height, width,
                      weights->pw_c_to_hw_weight, weights->pw_c_to_hw_bias);

    for (int src_pos = 0; src_pos < hw; ++src_pos) {
        const int src_h = src_pos / width;
        const int src_w = src_pos % width;

        for (int dst_pos = 0; dst_pos < hw; ++dst_pos) {
            const int dst_h = dst_pos / width;
            const int dst_w = dst_pos % width;

            k[chw_index(dst_pos, src_h, src_w, height, width)] =
                q[chw_index(src_pos, dst_h, dst_w, height, width)];
        }
    }

    for (size_t i = 0; i < hw_hw; ++i) {
        qk_hw[i] = q[i] * k[i];
    }

    pointwise_conv1x1(qk_hw, qk_c, hw, channels, height, width,
                      weights->pw_hw_to_c_weight, weights->pw_hw_to_c_bias);

    for (size_t i = 0; i < c_hw; ++i) {
        qk_c[i] = sigmoidf_stable(qk_c[i]);
        qkv[i] = v[i] * qk_c[i];
    }

    pointwise_conv1x1(qkv, expanded, channels, 3 * channels, height, width,
                      weights->pw_c_to_3c_weight, weights->pw_c_to_3c_bias);
    pointwise_conv1x1(expanded, output, 3 * channels, channels, height, width,
                      weights->pw_3c_to_c_weight, weights->pw_3c_to_c_bias);

    for (size_t i = 0; i < c_hw; ++i) {
        output[i] += residual[i];
    }

    return 0;
}
