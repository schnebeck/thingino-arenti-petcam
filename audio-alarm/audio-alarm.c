/*
 * audio-alarm - baby-monitor style audio watchdog for the petcam.
 *
 * Reads the camera's own live mic feed via "rac record -" (rac is the
 * already-shipped Raptor Audio Control CLI - a confirmed, working wrapper
 * around the audio ring, unlike calling librss_ipc.so's ring API directly,
 * which would depend on undocumented signatures pulled from disassembly).
 * rac emits a continuous ADTS AAC byte stream (sync word 0xFFF, standard
 * framing) on stdout; decoded via the public Helix AAC decoder API
 * (libhelix-aac.so - real open-source library, no reverse engineering).
 *
 * On a sustained energy excursion above a rolling moving average, triggers
 * /usr/sbin/raptor-audio-alarm start/stop, which fans out to send2* and an
 * rmr recording exactly like package/thingino-raptor/files/raptor-motion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

/* --- libhelix-aac.so: real public Helix AAC decoder API ------------- */
typedef void *HAACDecoder;

typedef struct _AACFrameInfo {
    int bitRate;
    int nChans;
    int sampRateCore;
    int sampRateOut;
    int bitsPerSample;
    int outputSamps;
    int profile;
    int tnsUsed;
    int pnsUsed;
} AACFrameInfo;

extern HAACDecoder AACInitDecoder(void);
extern void          AACFreeDecoder(HAACDecoder dec);
extern int            AACDecode(HAACDecoder dec, unsigned char **inbuf, int *bytesLeft, short *outbuf);
extern void          AACGetLastFrameInfo(HAACDecoder dec, AACFrameInfo *info);

#define PCM_BUF_MAX     4096  /* samples, both channels interleaved */
#define RING_BUF_CAP    16384 /* raw ADTS bytes held between decode passes */
#define REFILL_CHUNK    4096

/* --- configurable detection parameters, loaded from thingino.json's
 * "audio_alarm" object (see load_config()); these are the defaults if a
 * key is absent. --------------------------------------------------- */
struct alarm_config {
    int    enabled;
    double threshold_ratio;   /* single frame this loud vs. baseline triggers immediately */
    double activity_ratio;    /* frames above this vs. baseline start feeding the integral */
    double integral_threshold;/* accumulated excess-energy*seconds that triggers */
    int    avg_window_n;      /* moving-average length, in decoded frames */
    int    record_secs;       /* base recording time after a trigger */
    int    extend_secs;       /* added per additional trigger while recording */
};

static struct alarm_config g_cfg = {
    .enabled = 1,
    .threshold_ratio = 7.0,
    .activity_ratio = 1.5,
    /* Starting point, not calibrated against any specific room - a frame's
     * excess (energy - baseline*activity_ratio) accumulates while above
     * activity_ratio and decays (INTEGRAL_DECAY, below) while not; this is
     * roughly "~1-2s of moderate excess, or a shorter burst of a stronger
     * one". Expect to tune it from observed integral=... values in the log,
     * same as threshold_ratio already gets tuned per room. */
    .integral_threshold = 4000.0,
    /* ~19s at 1024 samples/16kHz per frame. A short window adapts to a
     * sustained loud sound (continuous crying) within a couple of seconds
     * and then stops re-extending it, since the average catches up to the
     * new "normal" - defeats the point for this use case. */
    .avg_window_n = 300,
    .record_secs = 20,
    .extend_secs = 10,
};

/* Per-frame multiplicative decay applied to the excess integral when the
 * current frame isn't contributing (energy at or below activity_ratio *
 * baseline) - not exposed in config, tune here if needed. At ~64ms/frame
 * (1024 samples/16kHz) this roughly halves the integral every ~400ms, so
 * isolated brief bumps drain out well before an unrelated later one could
 * add to the same integral. */
#define INTEGRAL_DECAY 0.90

#define THINGINO_JSON "/etc/thingino.json"

/* Shells out to jct once per key at startup - this runs a handful of times
 * total, not per audio frame, so the fork/exec cost is irrelevant. */
static int jct_get(const char *key, char *out, size_t out_len) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "jct %s get audio_alarm.%s 2>/dev/null", THINGINO_JSON, key);
    FILE *fp = popen(cmd, "r");
    if (!fp)
        return 0;
    size_t n = fread(out, 1, out_len - 1, fp);
    pclose(fp);
    if (n == 0)
        return 0;
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return out[0] != '\0' && strcmp(out, "null") != 0;
}

static void load_config(void) {
    char val[64];
    if (jct_get("enabled", val, sizeof(val)))
        g_cfg.enabled = (strcmp(val, "true") == 0);
    if (jct_get("threshold_ratio", val, sizeof(val)))
        g_cfg.threshold_ratio = atof(val);
    if (jct_get("activity_ratio", val, sizeof(val)))
        g_cfg.activity_ratio = atof(val);
    if (jct_get("integral_threshold", val, sizeof(val)))
        g_cfg.integral_threshold = atof(val);
    if (jct_get("avg_window_n", val, sizeof(val)))
        g_cfg.avg_window_n = atoi(val);
    if (jct_get("record_secs", val, sizeof(val)))
        g_cfg.record_secs = atoi(val);
    if (jct_get("extend_secs", val, sizeof(val)))
        g_cfg.extend_secs = atoi(val);

    if (g_cfg.avg_window_n < 2)
        g_cfg.avg_window_n = 2;
    if (g_cfg.threshold_ratio < 1.1)
        g_cfg.threshold_ratio = 1.1;
    if (g_cfg.activity_ratio < 1.0)
        g_cfg.activity_ratio = 1.0;
    if (g_cfg.integral_threshold < 1.0)
        g_cfg.integral_threshold = 1.0;
    if (g_cfg.record_secs < 1)
        g_cfg.record_secs = 1;
    if (g_cfg.extend_secs < 0)
        g_cfg.extend_secs = 0;
}

/* Rolling moving average of per-frame RMS energy. */
struct energy_avg {
    double *buf;
    int     n;
    int     pos;
    int     filled;
    double  sum;
};

static void energy_avg_init(struct energy_avg *a, int n) {
    a->buf = calloc((size_t)n, sizeof(double));
    a->n = n;
    a->pos = 0;
    a->filled = 0;
    a->sum = 0.0;
}

/* Feeds one frame's energy value in, returns the average *before* this
 * sample was added (so a genuine spike is compared against the recent
 * baseline, not diluted by itself). */
static double energy_avg_push(struct energy_avg *a, double value) {
    double avg = a->filled ? a->sum / a->filled : value;
    a->sum -= a->buf[a->pos];
    a->buf[a->pos] = value;
    a->sum += value;
    a->pos = (a->pos + 1) % a->n;
    if (a->filled < a->n) a->filled++;
    return avg;
}

static double pcm_rms(const short *samples, int n) {
    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        double s = samples[i];
        sum_sq += s * s;
    }
    return n ? sqrt(sum_sq / n) : 0.0;
}

/* --- alarm state machine -------------------------------------------- */
static long g_recording_until_ms = 0; /* monotonic ms; 0 = not recording */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define AUDIO_ALARM_HOOK "/usr/sbin/raptor-audio-alarm"

/* The pan motor sits close enough to the mic that its own rotation reads as
 * a loud, sudden sound - without this, every pan movement (webui clicks,
 * anything else that moves it) fires a false alarm. /run/motors-active
 * (written by the motors CLI itself) only exists for a brief instant at
 * the very start of a move, not for its full audible duration, so it's not
 * reliable here; motors -j's own "status" field stays "1" for as long as
 * it's actually moving. Only checked on a candidate trigger (not every
 * frame) so this doesn't add a process spawn to the normal quiet case. */
static int motor_is_active(void) {
    FILE *fp = popen("motors -j 2>/dev/null", "r");
    if (!fp)
        return 0;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    if (n == 0)
        return 0;
    buf[n] = '\0';
    return strstr(buf, "\"status\":\"1\"") != NULL;
}

static void run_hook(const char *arg) {
    if (access(AUDIO_ALARM_HOOK, X_OK) != 0)
        return;
    pid_t pid = fork();
    if (pid == 0) {
        execl(AUDIO_ALARM_HOOK, AUDIO_ALARM_HOOK, arg, (char *)NULL);
        _exit(127);
    }
    /* Deliberately not waited on - the hook backgrounds itself for
     * "start"/"stop" (send2 dispatch can take a while); nothing here
     * depends on its exit status. */
}

/* Mirrors package/thingino-raptor/files/raptor-motion, but a "start" here
 * begins a continuous rmr recording rather than a fixed-length clip:
 * extension is handled entirely by this daemon (push the deadline out),
 * the hook is only invoked on the idle->alarming and alarming->idle edges. */
static void trigger_alarm(double energy, double avg) {
    long t = now_ms();
    int already_recording = g_recording_until_ms > t;

    fprintf(stderr, "audio-alarm: TRIGGER energy=%.1f avg=%.1f ratio=%.2f (%s)\n",
            energy, avg, avg > 0 ? energy / avg : 0.0,
            already_recording ? "extending" : "starting");

    if (already_recording) {
        g_recording_until_ms += g_cfg.extend_secs * 1000L;
    } else {
        g_recording_until_ms = t + g_cfg.record_secs * 1000L;
        run_hook("start");
    }
}

static FILE *g_rac_fp = NULL;
static pid_t g_rac_pid = -1;

/* Not popen(): its pclose() blocks in waitpid() until the child exits, and
 * "rac record -" does NOT exit merely because its stdout pipe is closed -
 * it only notices on its next write, which can be however long it's
 * currently blocked reading the next audio frame for. Owning the pid
 * directly lets shutdown kill() it instead of waiting on its own schedule. */
static FILE *start_rac_record(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        fprintf(stderr, "audio-alarm: pipe() failed: %s\n", strerror(errno));
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "audio-alarm: fork() failed: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execlp("rac", "rac", "record", "-", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    g_rac_pid = pid;
    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp) {
        fprintf(stderr, "audio-alarm: fdopen() failed: %s\n", strerror(errno));
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        g_rac_pid = -1;
    }
    return fp;
}

/* Kill and reap the rac child directly rather than relying on it to notice
 * a closed pipe on its own schedule (see start_rac_record()). */
static void stop_rac_record(FILE *fp) {
    if (fp)
        fclose(fp);
    if (g_rac_pid > 0) {
        kill(g_rac_pid, SIGTERM);
        int status;
        pid_t r = waitpid(g_rac_pid, &status, WNOHANG);
        if (r == 0) {
            usleep(200000);
            r = waitpid(g_rac_pid, &status, WNOHANG);
        }
        if (r == 0) {
            kill(g_rac_pid, SIGKILL);
            waitpid(g_rac_pid, &status, 0);
        }
        g_rac_pid = -1;
    }
}

static volatile sig_atomic_t g_stop = 0;
static void on_term(int sig) { (void)sig; g_stop = 1; }
static void on_sigchld(int sig) {
    (void)sig;
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) { }
}

/* signal(3)'s glibc/uClibc default re-arms SA_RESTART, which transparently
 * restarts an interrupted blocking read() - g_stop would never be seen
 * while blocked waiting on rac's pipe. sigaction() without SA_RESTART is
 * required so the read actually returns EINTR on SIGTERM/SIGINT. */
static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* run_hook()'s forked children are never wait()ed on directly
     * (deliberately - "start" backgrounds send2 dispatch): reap whichever
     * child just exited so they don't sit as zombies. This can race with
     * stop_rac_record()'s own waitpid(g_rac_pid, ...) reaping the rac
     * child first; that's fine; ECHILD there just means "already gone". */
    struct sigaction sc;
    memset(&sc, 0, sizeof(sc));
    sc.sa_handler = on_sigchld;
    sigemptyset(&sc.sa_mask);
    sc.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sc, NULL);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    install_signal_handlers();

    load_config();
    if (!g_cfg.enabled) {
        fprintf(stderr, "audio-alarm: disabled in config, exiting\n");
        return 0;
    }
    fprintf(stderr, "audio-alarm: threshold_ratio=%.2f activity_ratio=%.2f integral_threshold=%.1f avg_window_n=%d record_secs=%d extend_secs=%d\n",
            g_cfg.threshold_ratio, g_cfg.activity_ratio, g_cfg.integral_threshold,
            g_cfg.avg_window_n, g_cfg.record_secs, g_cfg.extend_secs);

    HAACDecoder dec = AACInitDecoder();
    if (!dec) {
        fprintf(stderr, "audio-alarm: AACInitDecoder failed\n");
        return 1;
    }

    struct energy_avg avg;
    energy_avg_init(&avg, g_cfg.avg_window_n);
    double excess_integral = 0.0;

    unsigned char ring[RING_BUF_CAP];
    int ring_len = 0;
    short pcm[PCM_BUF_MAX];

    g_rac_fp = start_rac_record();

    while (!g_stop) {
        if (!g_rac_fp) {
            sleep(1);
            g_rac_fp = start_rac_record();
            continue;
        }

        if (ring_len < RING_BUF_CAP - REFILL_CHUNK) {
            /* Raw read() on the pipe fd, not fread(): stdio may retry an
             * EINTR-interrupted read internally before returning to us,
             * which would defeat install_signal_handlers()'s whole point. */
            ssize_t n = read(fileno(g_rac_fp), ring + ring_len, REFILL_CHUNK);
            if (n < 0) {
                if (errno == EINTR)
                    continue; /* g_stop check at the top of the loop */
                fprintf(stderr, "audio-alarm: read from 'rac record' failed: %s\n", strerror(errno));
                stop_rac_record(g_rac_fp);
                g_rac_fp = NULL;
                ring_len = 0;
                sleep(1);
                continue;
            }
            if (n == 0) {
                /* rac exited (rad restarted, camera reconfigured, etc.) -
                 * drop it and reconnect rather than spinning on EOF. */
                fprintf(stderr, "audio-alarm: 'rac record' ended, reconnecting\n");
                stop_rac_record(g_rac_fp);
                g_rac_fp = NULL;
                ring_len = 0;
                sleep(1);
                continue;
            }
            ring_len += (int)n;
        }

        unsigned char *inptr = ring;
        int bytesLeft = ring_len;
        for (;;) {
            if (bytesLeft < 7) /* smaller than one ADTS header - need more data */
                break;
            if (inptr[0] != 0xFF || (inptr[1] & 0xF0) != 0xF0) {
                /* Not a sync word - only expected right after a genuine
                 * stream glitch, so a slow byte-at-a-time scan back to
                 * alignment is fine here; it just must not be the normal
                 * per-frame path (that's what was burning ~70% CPU: trusting
                 * AACDecode's own byte accounting instead of the ADTS
                 * header's own frame_length made every frame take this
                 * path). */
                inptr++;
                bytesLeft--;
                continue;
            }
            /* ADTS frame_length: 13 bits spanning the low 2 bits of byte 3,
             * all of byte 4, and the high 3 bits of byte 5. Includes the
             * 7-byte header itself. Public, documented format - no need to
             * trust AACDecode's own advancement of inbuf/bytesLeft at all. */
            int frame_len = ((inptr[3] & 0x03) << 11) | (inptr[4] << 3) | ((inptr[5] & 0xE0) >> 5);
            if (frame_len < 7) {
                inptr++;
                bytesLeft--;
                continue;
            }
            if (frame_len > bytesLeft)
                break; /* incomplete frame in buffer - need more data */

            unsigned char *decode_ptr = inptr;
            int decode_left = frame_len;
            int err = AACDecode(dec, &decode_ptr, &decode_left, pcm);
            inptr += frame_len;
            bytesLeft -= frame_len;
            if (err != 0)
                continue; /* this frame failed to decode; still move past it */

            AACFrameInfo fi;
            AACGetLastFrameInfo(dec, &fi);
            if (fi.outputSamps <= 0)
                continue;

            double energy = pcm_rms(pcm, fi.outputSamps);
            double baseline = energy_avg_push(&avg, energy);

            /* Leaky integrator: frames moderately above baseline accumulate
             * excess energy*time; isolated brief bumps decay back out
             * before an unrelated later one could add to the same total,
             * but genuinely sustained or repeated activity builds up past
             * integral_threshold. A single very loud frame (threshold_ratio)
             * still bypasses this and triggers immediately either way. */
            double frame_secs = fi.sampRateOut > 0 ? (double)fi.outputSamps / fi.sampRateOut : 0.0;
            double excess = baseline > 0.0 ? energy - baseline * g_cfg.activity_ratio : 0.0;
            if (excess > 0.0)
                excess_integral += excess * frame_secs;
            else
                excess_integral *= INTEGRAL_DECAY;

            static int frame_count = 0;
            if (++frame_count % 50 == 0)
                fprintf(stderr, "audio-alarm: frame#%d energy=%.1f baseline=%.1f integral=%.1f rate=%d chans=%d samps=%d\n",
                        frame_count, energy, baseline, excess_integral, fi.sampRateOut, fi.nChans, fi.outputSamps);

            int immediate = baseline > 0.0 && energy > baseline * g_cfg.threshold_ratio;
            int integrated = excess_integral > g_cfg.integral_threshold;
            if (immediate || integrated) {
                if (motor_is_active()) {
                    fprintf(stderr, "audio-alarm: suppressed trigger - motor is moving\n");
                } else {
                    fprintf(stderr, "audio-alarm: %s trigger (integral=%.1f)\n",
                            immediate ? "immediate" : "integrated", excess_integral);
                    trigger_alarm(energy, baseline);
                }
                excess_integral = 0.0; /* must rebuild before this path fires again */
            }
        }

        /* Compact: keep only the unconsumed tail for the next refill. */
        if (bytesLeft > 0 && inptr != ring)
            memmove(ring, inptr, (size_t)bytesLeft);
        ring_len = bytesLeft;

        if (g_recording_until_ms && now_ms() >= g_recording_until_ms) {
            fprintf(stderr, "audio-alarm: recording window elapsed\n");
            g_recording_until_ms = 0;
            run_hook("stop");
        }
    }

    if (g_recording_until_ms)
        run_hook("stop");
    stop_rac_record(g_rac_fp);
    AACFreeDecoder(dec);
    return 0;
}
