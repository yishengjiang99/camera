#include "csa_block.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(expr)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__,         \
                    __LINE__, #expr);                                           \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define ASSERT_CLOSE(actual, expected, eps)                                      \
    do {                                                                        \
        const float actual_value = (actual);                                     \
        const float expected_value = (expected);                                 \
        if (fabsf(actual_value - expected_value) > (eps)) {                      \
            fprintf(stderr,                                                      \
                    "%s:%d: expected %.8f, got %.8f, diff %.8f\n", __FILE__,    \
                    __LINE__, expected_value, actual_value,                      \
                    fabsf(actual_value - expected_value));                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

static CsaBlockWeights make_weights(float *dw,
                                    float *pw_c_hw,
                                    float *pw_hw_c,
                                    float *pw_c_3c,
                                    float *pw_3c_c)
{
    CsaBlockWeights weights;
    memset(&weights, 0, sizeof(weights));
    weights.dw5x5_weight = dw;
    weights.pw_c_to_hw_weight = pw_c_hw;
    weights.pw_hw_to_c_weight = pw_hw_c;
    weights.pw_c_to_3c_weight = pw_c_3c;
    weights.pw_3c_to_c_weight = pw_3c_c;
    return weights;
}

static int test_scratch_len(void)
{
    ASSERT_TRUE(csa_block_scratch_len(0, 1, 1) == 0);
    ASSERT_TRUE(csa_block_scratch_len(1, 0, 1) == 0);
    ASSERT_TRUE(csa_block_scratch_len(1, 1, 0) == 0);

    /*
     * C=2, H=3, W=4:
     * residual C*HW + V C*HW + Q HW*HW + K HW*HW + QK HW*HW
     * + QK projected C*HW + QKV C*HW + expanded 3C*HW.
     */
    ASSERT_TRUE(csa_block_scratch_len(2, 3, 4) == 600);
    return 0;
}

static int test_invalid_args(void)
{
    float input[1] = {1.0f};
    float output[1] = {0.0f};
    float scratch[10] = {0.0f};
    float dw[25] = {0.0f};
    float pw_c_hw[1] = {1.0f};
    float pw_hw_c[1] = {1.0f};
    float pw_c_3c[3] = {1.0f, 0.0f, 0.0f};
    float pw_3c_c[3] = {1.0f, 0.0f, 0.0f};
    CsaBlockWeights weights =
        make_weights(dw, pw_c_hw, pw_hw_c, pw_c_3c, pw_3c_c);

    dw[12] = 1.0f;

    ASSERT_TRUE(csa_block_forward(NULL, output, 1, 1, 1, &weights,
                                  (CsaBlockScratch){scratch, 10}) == -1);
    ASSERT_TRUE(csa_block_forward(input, NULL, 1, 1, 1, &weights,
                                  (CsaBlockScratch){scratch, 10}) == -1);
    ASSERT_TRUE(csa_block_forward(input, output, 1, 1, 1, NULL,
                                  (CsaBlockScratch){scratch, 10}) == -1);
    ASSERT_TRUE(csa_block_forward(input, output, 1, 1, 1, &weights,
                                  (CsaBlockScratch){NULL, 10}) == -1);
    ASSERT_TRUE(csa_block_forward(input, output, 1, 1, 1, &weights,
                                  (CsaBlockScratch){scratch, 9}) == -2);

    weights.dw5x5_weight = NULL;
    ASSERT_TRUE(csa_block_forward(input, output, 1, 1, 1, &weights,
                                  (CsaBlockScratch){scratch, 10}) == -1);

    return 0;
}

static int test_forward_simple_expected_value(void)
{
    enum { C = 1, H = 1, W = 1 };
    float input[1] = {3.0f};
    float output[1] = {0.0f};
    float dw[25] = {0.0f};
    float pw_c_hw[1] = {1.0f};
    float pw_hw_c[1] = {0.0f};
    float pw_c_3c[3] = {1.0f, 0.0f, 0.0f};
    float pw_3c_c[3] = {2.0f, 0.0f, 0.0f};
    const size_t scratch_len = csa_block_scratch_len(C, H, W);
    float scratch[10] = {0.0f};
    CsaBlockWeights weights =
        make_weights(dw, pw_c_hw, pw_hw_c, pw_c_3c, pw_3c_c);

    dw[12] = 1.0f;
    ASSERT_TRUE(scratch_len == 10);
    ASSERT_TRUE(csa_block_forward(input, output, C, H, W, &weights,
                                  (CsaBlockScratch){scratch, scratch_len}) == 0);

    /*
     * V=input, sigmoid(Pw(Q*K))=sigmoid(0)=0.5,
     * final projection multiplies QKV by 2, then residual adds input.
     */
    ASSERT_CLOSE(output[0], 6.0f, 1e-6f);
    return 0;
}

static int test_output_may_alias_input(void)
{
    enum { C = 1, H = 1, W = 1 };
    float value[1] = {-4.0f};
    float dw[25] = {0.0f};
    float pw_c_hw[1] = {1.0f};
    float pw_hw_c[1] = {0.0f};
    float pw_c_3c[3] = {1.0f, 0.0f, 0.0f};
    float pw_3c_c[3] = {2.0f, 0.0f, 0.0f};
    float scratch[10] = {0.0f};
    CsaBlockWeights weights =
        make_weights(dw, pw_c_hw, pw_hw_c, pw_c_3c, pw_3c_c);

    dw[12] = 1.0f;
    ASSERT_TRUE(csa_block_forward(value, value, C, H, W, &weights,
                                  (CsaBlockScratch){scratch, 10}) == 0);

    ASSERT_CLOSE(value[0], -8.0f, 1e-6f);
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_scratch_len();
    failures += test_invalid_args();
    failures += test_forward_simple_expected_value();
    failures += test_output_may_alias_input();

    if (failures != 0) {
        fprintf(stderr, "%d test group(s) failed\n", failures);
        return 1;
    }

    puts("csa_block unit tests passed");
    return 0;
}
