#include <cmath>
#include <cassert>
#include <cstdio>
#include <pthread.h>
#include "quants.hpp"
#include "funcs.hpp"

#if defined(__ARM_NEON)
    #include <arm_neon.h>
#endif

void softmax(float* x, const int size) {
    float maxVal;
#if defined(__ARM_NEON)
    float32x4_t fs;
    float32x4_t fmaxv = vld1q_f32(&x[0]);
    for (int i = 4; i < size; i += 4) {
        fs = vld1q_f32(&x[i]);
        fmaxv = vmaxq_f32(fmaxv, fs);
    }
    maxVal = vmaxvq_f32(fmaxv);
#else
    // find max value (for numerical stability)
    maxVal = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > maxVal) {
            maxVal = x[i];
        }
    }
#endif
    // exp and sum
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - maxVal);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++) {
        x[i] /= sum;
    }
}

float rms(const float* x, const int size) {
    assert(size % 4 == 0);
    float ss;
#if defined(__ARM_NEON)
    float32x4_t fsq;
    float32x4_t fs = vmovq_n_f32(0);
    for (int j = 0; j < size; j += 4) {
        fsq = vld1q_f32(&x[j]);
        fs = vmlaq_f32(fs, fsq, fsq);
    }
    ss = vaddvq_f32(fs);
#else
    ss = 0;
    for (int j = 0; j < size; j++) {
        ss += x[j] * x[j];
    }
#endif
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    return ss;
}

void rmsnorm(float* o, const float* x, const float ms, const float* weight, const int size, unsigned int nThreads, unsigned int threadIndex) {
    assert(size % 4 == 0);
    assert(size % nThreads == 0);

    int slice = size / nThreads;
    int start = threadIndex * slice;
    int end = start + slice;

#if defined(__ARM_NEON)
    float32x4_t fw;
    float32x4_t fx;
    float32x4_t fss = vmovq_n_f32(ms);
    for (int j = start; j < end; j += 4) {
        fw = vld1q_f32(&weight[j]);
        fx = vld1q_f32(&x[j]);
        fx = vmulq_f32(fx, fw);
        fx = vmulq_f32(fx, fss);
        vst1q_f32(&o[j], fx);
    }
#else
    for (int j = start; j < end; j++) {
        o[j] = weight[j] * (ms * x[j]);
    }
#endif
}

struct MatmulThreadInfo {
    pthread_t handler;
    float* output;
    void* input;
    void* weights;
    int n;
    int ds;
    int de;
};

void matmulF32(MatmulThreadInfo* a) {
    const float* input = (float*)a->input;
    float* w = (float*)a->weights;
    int d;

#if defined(__ARM_NEON)
    float32x4_t q;
    float32x4_t p;
    float32x4_t z;
    for (d = a->ds; d < a->de; d++) {
        z = vmovq_n_f32(0);
        for (int j = 0; j < a->n; j += 4) {
            q = vld1q_f32(&input[j]);
            p = vld1q_f32(&w[d * a->n + j]);
            z = vfmaq_f32(z, q, p);
        }
        a->output[d] = vaddvq_f32(z);
    }
#else
    for (d = a->ds; d < a->de; d++) {
        float val = 0.0f;
        for (int j = 0; j < a->n; j++) {
            val += w[d * a->n + j] * input[j];
        }
        a->output[d] = val;
    }
#endif
}

void matmulF16(MatmulThreadInfo* a) {
    const float* input = (float*)a->input;
    uint16_t* w = (uint16_t*)a->weights;
    int d;
    for (d = a->ds; d < a->de; d++) {
        float val = 0.0f;
        for (int j = 0; j < a->n; j++) {
            float ww = convertF16ToF32(w[d * a->n + j]);
            val += ww * input[j];
        }
        a->output[d] = val;
    }
}

void matmulQ40(MatmulThreadInfo* a) {
    const int blocksPerRow = 8;
    const int k = QK40 * blocksPerRow;
    BlockQ40* w = (BlockQ40*)a->weights;
    assert(a->n % k == 0);
    const float* input = (float*)a->input;
    const int n = a->n / k;
    float group[k];

#if defined(__ARM_NEON)
    assert(k % 16 == 0);
    float32x4_t a0;
    float32x4_t b0;
    float32x4_t u;
    for (int d = a->ds; d < a->de; d++) {
        u = vmovq_n_f32(0);
        for (int j = 0; j < n; j++) {
            dequantizeQ40Row(&w[d * n * blocksPerRow + j * blocksPerRow], group, k);
            for (int z = 0; z < k; z += 4) {
                a0 = vld1q_f32(&input[j * k + z]);
                b0 = vld1q_f32(&group[z]);
                u = vfmaq_f32(u, a0, b0);
            }
        }
        a->output[d] = vaddvq_f32(u);
    }
#else
    for (int d = a->ds; d < a->de; d++) {
        float val = 0.0f;
        for (int j = 0; j < n; j++) {
            dequantizeQ40Row(&w[d * n * blocksPerRow + j * blocksPerRow], group, k);
            for (int z = 0; z < k; z++) {
                val += group[z] * input[j * k + z];
            }
        }
        a->output[d] = val;
    }
#endif
}

void matmulQ40vQ80(MatmulThreadInfo* a) {
    const BlockQ40* w = (BlockQ40*)a->weights;
    const BlockQ80* input = (BlockQ80*)a->input;
    assert(a->n % QK40 == 0);
    const int n = a->n / QK40;

#if defined(__ARM_NEON)
    float32x4_t sumv0;
    float32x4_t sumv1;
    for (int d = a->ds; d < a->de; d++) {
        sumv0 = vmovq_n_f32(0);
        sumv1 = vmovq_n_f32(0);
        for (int j = 0; j < n; j += 2) {
            const BlockQ40* x0 = &w[d * n + j];
            const BlockQ40* x1 = &w[d * n + j + 1];
            const BlockQ80* y0 = &input[j];
            const BlockQ80* y1 = &input[j + 1];

            const uint8x16_t m4b = vdupq_n_u8(0x0F);
            const int8x16_t  s8b = vdupq_n_s8(0x8);

            const uint8x16_t v0_0 = vld1q_u8(x0->qs);
            const uint8x16_t v0_1 = vld1q_u8(x1->qs);

            // 4-bit -> 8-bit
            const int8x16_t v0_0l = vreinterpretq_s8_u8(vandq_u8  (v0_0, m4b));
            const int8x16_t v0_0h = vreinterpretq_s8_u8(vshrq_n_u8(v0_0, 4));
            const int8x16_t v0_1l = vreinterpretq_s8_u8(vandq_u8  (v0_1, m4b));
            const int8x16_t v0_1h = vreinterpretq_s8_u8(vshrq_n_u8(v0_1, 4));

            // sub 8
            const int8x16_t v0_0ls = vsubq_s8(v0_0l, s8b);
            const int8x16_t v0_0hs = vsubq_s8(v0_0h, s8b);
            const int8x16_t v0_1ls = vsubq_s8(v0_1l, s8b);
            const int8x16_t v0_1hs = vsubq_s8(v0_1h, s8b);

            // load y
            const int8x16_t v1_0l = vld1q_s8(y0->qs);
            const int8x16_t v1_0h = vld1q_s8(y0->qs + 16);
            const int8x16_t v1_1l = vld1q_s8(y1->qs);
            const int8x16_t v1_1h = vld1q_s8(y1->qs + 16);


#if defined(__ARM_FEATURE_DOTPROD)
            const int32x4_t p_0 = vdotq_s32(vdotq_s32(vdupq_n_s32(0), v0_0ls, v1_0l), v0_0hs, v1_0h);
            const int32x4_t p_1 = vdotq_s32(vdotq_s32(vdupq_n_s32(0), v0_1ls, v1_1l), v0_1hs, v1_1h);

            sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(p_0), convertF16ToF32(x0->d)*convertF16ToF32(y0->d));
            sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(p_1), convertF16ToF32(x1->d)*convertF16ToF32(y1->d));
#else
            const int16x8_t pl0l = vmull_s8(vget_low_s8 (v0_0ls), vget_low_s8 (v1_0l));
            const int16x8_t pl0h = vmull_s8(vget_high_s8(v0_0ls), vget_high_s8(v1_0l));
            const int16x8_t ph0l = vmull_s8(vget_low_s8 (v0_0hs), vget_low_s8 (v1_0h));
            const int16x8_t ph0h = vmull_s8(vget_high_s8(v0_0hs), vget_high_s8(v1_0h));

            const int16x8_t pl1l = vmull_s8(vget_low_s8 (v0_1ls), vget_low_s8 (v1_1l));
            const int16x8_t pl1h = vmull_s8(vget_high_s8(v0_1ls), vget_high_s8(v1_1l));
            const int16x8_t ph1l = vmull_s8(vget_low_s8 (v0_1hs), vget_low_s8 (v1_1h));
            const int16x8_t ph1h = vmull_s8(vget_high_s8(v0_1hs), vget_high_s8(v1_1h));

            const int32x4_t pl0 = vaddq_s32(vpaddlq_s16(pl0l), vpaddlq_s16(pl0h));
            const int32x4_t ph0 = vaddq_s32(vpaddlq_s16(ph0l), vpaddlq_s16(ph0h));
            const int32x4_t pl1 = vaddq_s32(vpaddlq_s16(pl1l), vpaddlq_s16(pl1h));
            const int32x4_t ph1 = vaddq_s32(vpaddlq_s16(ph1l), vpaddlq_s16(ph1h));

            sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(pl0, ph0)), convertF16ToF32(x0->d) * convertF16ToF32(y0->d));
            sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(pl1, ph1)), convertF16ToF32(x1->d) * convertF16ToF32(y1->d));
#endif
        }
        a->output[d] = vaddvq_f32(sumv0) + vaddvq_f32(sumv1);
    }
#else
    printf("matmulQ40vQ80 - not implemented\n");
    exit(EXIT_FAILURE);
#endif
}

//     weights      input    output
//   ___________     ___      ___
//   |         |     | |      | |
// d |         | *   | |  = d | |
//   |_________|   n | |      |_|
//        n          |_|       1
//                    1
void matmul(FloatType weightsFloatType, FloatType inputFloatType, float* output, void* input, void* weights, int n, int d, unsigned int nThreads, unsigned int threadIndex) {
    MatmulThreadInfo s;
    s.output = output;
    s.input = input;
    s.weights = weights;
    s.n = n;
    s.ds = threadIndex * d / nThreads;
    s.de = (threadIndex + 1) * d / nThreads;

    if (inputFloatType == F32) {
        if (weightsFloatType == F32) {
            matmulF32(&s);
            return;
        }
        if (weightsFloatType == F16) {
            matmulF16(&s);
            return;
        }
        if (weightsFloatType == Q40) {
            matmulQ40(&s);
            return;
        }
    }
    if (inputFloatType == Q80 && weightsFloatType == Q40) {
        matmulQ40vQ80(&s);
        return;
    }

    printf("Unsupported float types: %d/%d\n", weightsFloatType, inputFloatType);
    exit(EXIT_FAILURE);
}

float dotProduct(const float* a, const float* b, const int size) {
    assert(size % 4 == 0);
#if defined(__ARM_NEON)
    float32x4_t fa;
    float32x4_t fb;
    float32x4_t fs = vmovq_n_f32(0);
    for (int i = 0; i < size; i += 4) {
        fa = vld1q_f32(&a[i]);
        fb = vld1q_f32(&b[i]);
        fs = vmlaq_f32(fs, fa, fb);
    }
    return vaddvq_f32(fs);
#else
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        sum += a[i] * b[i];
    }
    return sum;
#endif
}
