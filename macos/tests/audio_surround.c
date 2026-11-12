/* XAudio2 output layout through the real shim and SDL's dummy output device.
 * Run as "audio_surround stereo" (default layout) or, with
 * LB_AUDIO_CHANNELS=6, "audio_surround surround". Both need LB_AUDIO_METER=1,
 * which makes the engine keep the per-channel output levels read here.
 * - stereo: the device reports 2 channels (mask 3) and a mono voice with the
 *   default matrix reaches both channels equally;
 * - surround: the device reports 6 channels (mask 0x3F), a mastering voice
 *   created with the default channel count has 6, a mono voice sent to the
 *   back-left channel by SetOutputMatrix reaches only that output channel at
 *   its own level, and a mono voice with the default matrix reaches only the
 *   front pair.
 * No game image or physical device. */
#include "rt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

extern int xaudio2_create(uint32_t clsid, uint32_t *out);
int xaudio2_output_levels(double *sum2, int max, uint64_t *frames);

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)
#define LEVEL 0.25

static uint32_t method(CPU *cpu, uint32_t object, unsigned index, unsigned count, const uint32_t *arguments) {
    uint32_t values[8] = {object};
    if (count) memcpy(values + 1, arguments, count * sizeof(uint32_t));
    return rt_invoke(cpu, M32(M32(object) + 4 * index), (int)count + 1, values);
}
static void wait_ms(unsigned ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t)) {}
}

/* A started mono 48 kHz PCM16 voice looping a square wave of amplitude LEVEL. */
static int square_voice(CPU *cpu, uint32_t engine, uint32_t *voice) {
    enum { FRAMES = 4800 };
    uint32_t wf = gmalloc(18), out = gmalloc(4), data = gmalloc(FRAMES * 2), buf = gmalloc(36);
    memset(GPTR(wf), 0, 18);
    M16(wf) = 1; M16(wf + 2) = 1; M32(wf + 4) = 48000; M32(wf + 8) = 96000; M16(wf + 12) = 2; M16(wf + 14) = 16;
    for (int i = 0; i < FRAMES; i++) M16(data + 2 * (uint32_t)i) = (uint16_t)(int16_t)((i / 24 % 2 ? -1 : 1) * (int)(LEVEL * 32768));
    float two = 2.0f; uint32_t ratio; memcpy(&ratio, &two, 4);
    uint32_t create[] = {out, wf, 0, ratio, 0, 0, 0};
    CHECK(method(cpu, engine, 8, 7, create) == 0 && M32(out) != 0);  /* CreateSourceVoice */
    memset(GPTR(buf), 0, 36);
    M32(buf + 4) = FRAMES * 2; M32(buf + 8) = data; M32(buf + 28) = 255;  /* loop forever */
    uint32_t submit[] = {buf, 0}, start[] = {0, 0};
    CHECK(method(cpu, M32(out), 21, 2, submit) == 0);
    CHECK(method(cpu, M32(out), 19, 2, start) == 0);
    *voice = M32(out);
    return 0;
}

/* RMS per output channel over about `ms` of mixing from now. */
static int measure(CPU *cpu, uint32_t engine, unsigned ms, double *rms, int channels) {
    double a[8], b[8]; uint64_t fa, fb;
    CHECK(method(cpu, engine, 12, 0, NULL) == 0);  /* StopEngine */
    wait_ms(50);
    xaudio2_output_levels(a, 8, &fa);
    CHECK(method(cpu, engine, 11, 0, NULL) == 0);  /* StartEngine */
    wait_ms(ms);
    CHECK(method(cpu, engine, 12, 0, NULL) == 0);
    wait_ms(50);
    xaudio2_output_levels(b, 8, &fb);
    CHECK(fb > fa + 4800);
    for (int i = 0; i < channels; i++) rms[i] = sqrt((b[i] - a[i]) / (double)(fb - fa));
    CHECK(method(cpu, engine, 11, 0, NULL) == 0);
    return 0;
}
static int near_level(double v) { return fabs(v - LEVEL) < 0.01; }

int main(int argc, char **argv) {
    int surround = argc > 1 && !strcmp(argv[1], "surround");
    int want = surround ? 6 : 2;
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(MEM != MAP_FAILED && mprotect(MEM, 0x10000, PROT_NONE) == 0);
    rt_init_shims();
    CPU *cpu = rt_new_cpu();
    rt_thread_init(cpu);
    static const uint8_t guid[16] = {0x49, 0x9b, 0xda, 0x3e, 0x85, 0x20, 0x8b, 0x49, 0x9b, 0xb2, 0x39, 0xa6, 0x77, 0x84, 0x93, 0xde};
    uint32_t clsid = gmalloc(16), engine = 0;
    memcpy(GPTR(clsid), guid, sizeof guid);
    CHECK(xaudio2_create(clsid, &engine) != 0 && engine != 0);
    CHECK(xaudio2_output_levels(NULL, 0, NULL) == want);

    uint32_t details = gmalloc(1068), dargs[] = {0, details};
    CHECK(method(cpu, engine, 4, 2, dargs) == 0);  /* GetDeviceDetails */
    uint32_t f = details + 1028;
    CHECK(M16(f + 2) == want && M16(f + 12) == 4 * want && M32(f + 8) == 48000u * 4 * (uint32_t)want);
    CHECK(M32(f + 20) == (surround ? 0x3Fu : 3u));

    /* InputChannels 0 is XAUDIO2_DEFAULT_CHANNELS: the device's count. */
    uint32_t master_out = gmalloc(4), master_args[] = {master_out, 0, 48000, 0, 0, 0};
    CHECK(method(cpu, engine, 10, 6, master_args) == 0 && M32(master_out) != 0);
    uint32_t vd = gmalloc(12);
    CHECK(method(cpu, M32(master_out), 0, 1, &vd) == 0 && M32(vd + 4) == (uint32_t)want);
    CHECK(method(cpu, engine, 11, 0, NULL) == 0);

    double rms[8];
    uint32_t voice;
    if (square_voice(cpu, engine, &voice)) return 1;
    if (measure(cpu, engine, 400, rms, want)) return 1;
    if (!surround) {
        fprintf(stderr, "stereo default matrix: L %.4f R %.4f\n", rms[0], rms[1]);
        CHECK(near_level(rms[0]) && near_level(rms[1]));
    } else {
        fprintf(stderr, "5.1 default matrix: %.4f %.4f %.4f %.4f %.4f %.4f\n", rms[0], rms[1], rms[2], rms[3], rms[4], rms[5]);
        CHECK(near_level(rms[0]) && near_level(rms[1]));
        CHECK(rms[2] == 0 && rms[3] == 0 && rms[4] == 0 && rms[5] == 0);
        /* the same voice, placed on back left only */
        uint32_t m = gmalloc(24);
        for (uint32_t k = 0; k < 6; k++) MF32(m + 4 * k) = k == 4 ? 1.0f : 0.0f;
        uint32_t margs[] = {M32(master_out), 1, 6, m, 0};
        CHECK(method(cpu, voice, 16, 5, margs) == 0);  /* SetOutputMatrix */
        wait_ms(50);
        if (measure(cpu, engine, 400, rms, 6)) return 1;
        fprintf(stderr, "5.1 back-left matrix: %.4f %.4f %.4f %.4f %.4f %.4f\n", rms[0], rms[1], rms[2], rms[3], rms[4], rms[5]);
        CHECK(near_level(rms[4]));
        CHECK(rms[0] == 0 && rms[1] == 0 && rms[2] == 0 && rms[3] == 0 && rms[5] == 0);
    }
    printf("audio_surround %s: %u checks passed\n", surround ? "surround" : "stereo", checks);
    return 0;
}
