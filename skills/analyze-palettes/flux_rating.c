/* Rates one proposed song: every stem in song time, one analyzer each, one
   shared group history. Prints the average of the scores inside the given
   span, after warming the 15 s window up on the audio before it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>

#include "cumulative_flux.h"

#define WINDOW_MS 15000
#define BANDS 4
#define STEP_MS 100

static unsigned int le32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int le16(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned char* slurp(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc((size_t)size);
    if (!buf || (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return buf;
}

static float* read_pcm16(const char* path, long long* frames, int* sample_rate) {
    size_t len = 0;
    unsigned char* file = slurp(path, &len);
    if (!file) return NULL;
    if (len < 12 || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) { free(file); return NULL; }

    int channels = 0, bits = 0, sr = 0;
    const unsigned char* pcm = NULL;
    size_t pcm_bytes = 0;
    for (size_t pos = 12; pos + 8 <= len; ) {
        unsigned int size = le32(file + pos + 4);
        size_t avail = len - (pos + 8);
        if (size > avail) size = (unsigned int)avail;
        if (!memcmp(file + pos, "fmt ", 4) && size >= 16) {
            channels = (int)le16(file + pos + 10);
            sr = (int)le32(file + pos + 12);
            bits = (int)le16(file + pos + 22);
        } else if (!memcmp(file + pos, "data", 4)) {
            pcm = file + pos + 8;
            pcm_bytes = size;
        }
        pos += 8 + size + (size & 1);
    }

    float* out = NULL;
    if (pcm && bits == 16 && channels >= 1 && sr > 0) {
        long long n = (long long)(pcm_bytes / (size_t)(2 * channels));
        out = (float*)malloc(sizeof(float) * (size_t)(n > 0 ? n : 1));
        if (out) {
            for (long long f = 0; f < n; f++) {
                double sum = 0.0;
                for (int c = 0; c < channels; c++) {
                    sum += (double)(short)le16(pcm + (size_t)(f * channels + c) * 2);
                }
                out[f] = (float)(sum / (channels * 32768.0));
            }
            *frames = n;
            *sample_rate = sr;
        }
    }
    free(file);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 5 || strcmp(argv[1], "--score")) {
        fprintf(stderr, "usage: flux_rating --score <from s> <to s> <stem.wav> [stem.wav ...]\n");
        return 2;
    }
    double from_s = atof(argv[2]), to_s = atof(argv[3]);
    if (!(to_s > from_s)) {
        fprintf(stderr, "score span is empty\n");
        return 2;
    }

    int count = argc - 4;
    float** audio = (float**)calloc((size_t)count, sizeof(float*));
    long long* frames = (long long*)calloc((size_t)count, sizeof(long long));
    TransientAnalyzer** analyzers = (TransientAnalyzer**)calloc((size_t)count, sizeof(TransientAnalyzer*));
    if (!audio || !frames || !analyzers) return 2;

    int sr = 0;
    long long total = 0;
    for (int i = 0; i < count; i++) {
        int file_sr = 0;
        audio[i] = read_pcm16(argv[i + 4], &frames[i], &file_sr);
        if (!audio[i]) {
            fprintf(stderr, "not a 16-bit PCM WAV: %s\n", argv[i + 4]);
            return 2;
        }
        if (!sr) sr = file_sr;
        if (file_sr != sr) {
            fprintf(stderr, "sample rate mismatch: %s\n", argv[i + 4]);
            return 2;
        }
        if (frames[i] > total) total = frames[i];
    }

    SharedTransientBuffer shared;
    memset(&shared, 0, sizeof(shared));
    shared.max_peak = 1.0;
    shared.min_score_seen = DBL_MAX;
    shared.max_score_seen = -DBL_MAX;

    for (int i = 0; i < count; i++) {
        analyzers[i] = analyzer_create(1.0, &shared, NULL, NULL, NULL, WINDOW_MS, BANDS);
        if (!analyzers[i]) return 2;
        analyzer_set_sample_rate(analyzers[i], sr);
    }

    int hop = sr / 1000;
    if (hop < 1) hop = 1;
    int step = hop * STEP_MS;
    int flush = sr / 5;
    long long window = (long long)sr * WINDOW_MS / 1000;
    long long score_from = (long long)(from_s * sr + 0.5);
    long long score_to = (long long)(to_s * sr + 0.5);
    if (score_to > total) score_to = total;
    long long origin = score_from - flush - (long long)(WINDOW_MS / STEP_MS + 4) * step;
    if (origin < 0) origin = 0;
    long long steps = (score_to + flush - origin + step - 1) / step;
    float* push = (float*)calloc((size_t)step, sizeof(float));
    ChunkAnalysisResult* chunk = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));
    if (!push || !chunk) return 2;

    int scoring = 0;
    for (long long t = 0; t < steps; t++) {
        long long last = origin + t * step;
        long long act = last - flush;
        if (!scoring && act >= score_from) {
            shared.total_score_sum = 0.0;
            shared.score_count = 0;
            scoring = 1;
        }
        act -= origin;
        long long win = act - window;
        if (win < 0) win = 0;

        for (int i = 0; i < count; i++) {
            memset(push, 0, sizeof(float) * (size_t)step);
            if (last < frames[i]) {
                long long rem = frames[i] - last;
                memcpy(push, audio[i] + last, sizeof(float) * (size_t)(rem < step ? rem : step));
            }
            analyzer_analyze_chunk(analyzers[i], push, step, sr, (int)(win / hop), (int)(act / hop), chunk);
        }
    }

    double rating = shared.score_count > 0 ? shared.total_score_sum / (double)shared.score_count : 0.0;
    printf("%.9f\n", rating);

    for (int i = 0; i < count; i++) analyzer_destroy(analyzers[i]);
    return 0;
}
