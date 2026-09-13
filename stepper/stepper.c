/*
 * stepper - minimal half-step (8-phase) driver for 28BYJ-48-style unipolar
 * steppers via 4 raw sysfs GPIOs, for the Ingenic T31 / Thingino petcam.
 *
 * Sequence: A -> AB -> B -> BC -> C -> CD -> D -> DA (soft/wave+full mix,
 * i.e. classic ULN2003 half-step table).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>

#define DEFAULT_PIDFILE    "/var/run/stepper.pid"
#define DEFAULT_STATUSFILE "/var/run/stepper.status"
#define DEFAULT_SPEED      800   /* half-steps per second */
#define DEFAULT_MAX_RUNTIME 30   /* seconds; caps --continuous so a lost --stop can't hold coils energized indefinitely */

static const unsigned char HALFSTEP[8] = {
    0x1, /* A   1000 */
    0x3, /* AB  1100 */
    0x2, /* B   0100 */
    0x6, /* BC  0110 */
    0x4, /* C   0010 */
    0xC, /* CD  0011 */
    0x8, /* D   0001 */
    0x9, /* DA  1001 */
};

/* Small unipolar 28BYJ-48-style coils have no continuous-duty rating; two
 * half-step states out of eight energize two coils at once (see HALFSTEP),
 * and repeated cycles run back-to-back (e.g. many dispense triggers) add up
 * even though each run powers down cleanly afterward. Cap total energized
 * time to 20% over a rolling window so rapid repeated use can't cook the
 * windings, independent of any single run's own cleanup. */
#define DUTY_WINDOW_SEC 300
#define DUTY_MAX_ON_MS  60000
#define DUTY_STATE_FILE "/var/run/stepper.duty"

static volatile sig_atomic_t g_stop = 0;
static int g_pins[4] = {46, 40, 10, 11}; /* default: Ruehrwerk (agitator) stepper */
static int g_value_fd[4] = {-1, -1, -1, -1};
static const char *g_pidfile = DEFAULT_PIDFILE;
static const char *g_statusfile = DEFAULT_STATUSFILE;

static void die(const char *msg) {
    fprintf(stderr, "stepper: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void write_status(int in_motion) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_statusfile);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "in_motion=%s\n", in_motion ? "true" : "false");
    fclose(f);
    rename(tmp, g_statusfile);
}

static int read_pidfile(void) {
    FILE *f = fopen(g_pidfile, "r");
    if (!f) return -1;
    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);
    return pid;
}

static int pid_alive(int pid) {
    if (pid <= 0) return 0;
    return kill(pid, 0) == 0;
}

static void write_pidfile(void) {
    FILE *f = fopen(g_pidfile, "w");
    if (!f) die("cannot write pidfile");
    fprintf(f, "%d\n", getpid());
    fclose(f);
}

static void gpio_setup_pin(int pin) {
    char path[128];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
    if (access(path, F_OK) != 0) {
        fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd < 0) die("cannot open /sys/class/gpio/export");
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", pin);
        if (write(fd, buf, n) < 0 && errno != EBUSY) {
            close(fd);
            die("cannot export gpio");
        }
        close(fd);
    }

    /* Ingenic vendor kernel: claim pin away from other subsystems.
       Not all kernels have this proc entry; ignore if missing. Must run
       after export, matching the vendor gpio script's ordering. */
    fd = open("/proc/gpio_claim/gpio", O_WRONLY);
    if (fd >= 0) {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", pin);
        if (write(fd, buf, n) < 0) { /* ignore */ }
        close(fd);
    }

    /* Some Ingenic gpiochips register pins as fixed-direction output-only,
       in which case sysfs has no "direction" attribute at all (only
       "value"). Missing direction file is not fatal - just skip it. */
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (errno != ENOENT)
            die("cannot open gpio direction");
        fprintf(stderr, "stepper: pin %d has no direction attribute, assuming fixed output\n", pin);
    } else {
        if (write(fd, "out", 3) < 0) die("cannot set gpio direction");
        close(fd);
    }
}

static int gpio_open_value(int pin) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) die("cannot open gpio value");
    return fd;
}

static void gpio_write(int fd, int high) {
    const char *v = high ? "1" : "0";
    if (pwrite(fd, v, 1, 0) < 0) { /* ignore transient errors */ }
}

static void set_phase(unsigned char mask) {
    for (int i = 0; i < 4; i++)
        gpio_write(g_value_fd[i], (mask >> i) & 1);
}

static void power_down(void) {
    set_phase(0x0);
}

static void setup_gpios(void) {
    for (int i = 0; i < 4; i++)
        gpio_setup_pin(g_pins[i]);
    for (int i = 0; i < 4; i++)
        g_value_fd[i] = gpio_open_value(g_pins[i]);
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void duty_load(long *window_start_ms, long *accumulated_ms) {
    FILE *f = fopen(DUTY_STATE_FILE, "r");
    *window_start_ms = 0;
    *accumulated_ms = 0;
    if (!f) return;
    if (fscanf(f, "%ld %ld", window_start_ms, accumulated_ms) != 2) {
        *window_start_ms = 0;
        *accumulated_ms = 0;
    }
    fclose(f);
}

static void duty_save(long window_start_ms, long accumulated_ms) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", DUTY_STATE_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ld %ld\n", window_start_ms, accumulated_ms);
    fclose(f);
    rename(tmp, DUTY_STATE_FILE);
}

/* Reserves expected_ms of energized time against the rolling duty budget.
 * Returns 1 if there's room (and reserves it), 0 if starting now would
 * exceed the cap - caller must not energize any coil in that case. */
static int duty_reserve(long expected_ms) {
    long window_start, accumulated, t;
    duty_load(&window_start, &accumulated);
    t = now_ms();
    if (window_start == 0 || t - window_start > (long)DUTY_WINDOW_SEC * 1000) {
        window_start = t;
        accumulated = 0;
    }
    if (accumulated + expected_ms > DUTY_MAX_ON_MS) {
        fprintf(stderr,
            "stepper: duty cycle limit reached (%ld/%dms energized in last %ds) - refusing to start, let coils cool down\n",
            accumulated, DUTY_MAX_ON_MS, DUTY_WINDOW_SEC);
        return 0;
    }
    duty_save(window_start, accumulated + expected_ms);
    return 1;
}

static void sleep_ns(long ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000L, .tv_nsec = ns % 1000000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !g_stop)
        ;
}

static void run_steps(long steps, int speed) {
    int dir = steps < 0 ? -1 : 1;
    long count = steps < 0 ? -steps : steps;
    long delay_ns = 1000000000L / speed;
    int idx = 0;

    setup_gpios();
    write_pidfile();
    write_status(1);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    for (long i = 0; i < count && !g_stop; i++) {
        idx = ((idx + dir) % 8 + 8) % 8;
        set_phase(HALFSTEP[idx]);
        sleep_ns(delay_ns);
    }

    power_down();
    write_status(0);
    unlink(g_pidfile);
}

static void run_continuous(int dir, int speed, int max_runtime) {
    long delay_ns = 1000000000L / speed;
    long deadline_ms = now_ms() + (long)max_runtime * 1000;
    int idx = 0;
    int step = dir >= 0 ? 1 : -1;

    setup_gpios();
    write_pidfile();
    write_status(1);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    while (!g_stop) {
        if (now_ms() >= deadline_ms) {
            fprintf(stderr, "stepper: max runtime (%ds) reached, powering down\n", max_runtime);
            break;
        }
        idx = ((idx + step) % 8 + 8) % 8;
        set_phase(HALFSTEP[idx]);
        sleep_ns(delay_ns);
    }

    power_down();
    write_status(0);
    unlink(g_pidfile);
}

static void cmd_stop(void) {
    int pid = read_pidfile();
    if (!pid_alive(pid)) {
        fprintf(stderr, "stepper: not running\n");
        write_status(0);
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 50 && pid_alive(pid); i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
        nanosleep(&ts, NULL);
    }
}

static void cmd_status(void) {
    FILE *f = fopen(g_statusfile, "r");
    if (!f) {
        printf("in_motion=false\n");
        return;
    }
    char line[64] = "in_motion=false";
    if (fgets(line, sizeof(line), f)) {
        /* also verify the pid in pidfile is actually still alive */
        int pid = read_pidfile();
        if (strstr(line, "true") && !pid_alive(pid)) {
            printf("in_motion=false\n");
            fclose(f);
            return;
        }
        fputs(line, stdout);
        if (line[strlen(line) - 1] != '\n') printf("\n");
    }
    fclose(f);
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) die("fork failed");
    if (pid > 0) _exit(0);
    if (setsid() < 0) die("setsid failed");
    pid = fork();
    if (pid < 0) die("fork failed");
    if (pid > 0) _exit(0);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        if (fd > 2) close(fd);
    }
}

static void usage(void) {
    fprintf(stderr,
"Usage:\n"
"  stepper --steps N [--speed HZ] [--pins a,b,c,d] [--daemon]\n"
"  stepper --continuous [--dir cw|ccw] [--speed HZ] [--pins a,b,c,d]\n"
"  stepper --stop\n"
"  stepper --status\n"
"\n"
"  --steps N       half-steps to run, negative = reverse direction\n"
"  --continuous    run until --stop is called (implies --daemon)\n"
"  --dir cw|ccw    direction for --continuous (default cw)\n"
"  --speed HZ      half-steps per second (default %d)\n"
"  --pins a,b,c,d  GPIO numbers for coil A,B,C,D (default 46,40,10,11)\n"
"  --daemon        fork to background before running\n"
"  --pidfile PATH  (default %s)\n"
"  --statusfile P  (default %s)\n"
"  --max-runtime S cap on --continuous runtime in seconds (default %d)\n"
"  --stop          stop a running instance and power down coils\n"
"  --status        print in_motion=true|false\n",
    DEFAULT_SPEED, DEFAULT_PIDFILE, DEFAULT_STATUSFILE, DEFAULT_MAX_RUNTIME);
    exit(1);
}

int main(int argc, char **argv) {
    long steps = 0;
    int have_steps = 0;
    int continuous = 0;
    int dir = 1;
    int speed = DEFAULT_SPEED;
    int want_daemon = 0;
    int do_stop = 0;
    int do_status = 0;
    int max_runtime = DEFAULT_MAX_RUNTIME;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
            steps = atol(argv[++i]);
            have_steps = 1;
        } else if (!strcmp(argv[i], "--continuous")) {
            continuous = 1;
        } else if (!strcmp(argv[i], "--dir") && i + 1 < argc) {
            i++;
            dir = !strcmp(argv[i], "ccw") ? -1 : 1;
        } else if (!strcmp(argv[i], "--speed") && i + 1 < argc) {
            speed = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pins") && i + 1 < argc) {
            int a, b, c, d;
            if (sscanf(argv[++i], "%d,%d,%d,%d", &a, &b, &c, &d) != 4) usage();
            g_pins[0] = a; g_pins[1] = b; g_pins[2] = c; g_pins[3] = d;
        } else if (!strcmp(argv[i], "--daemon")) {
            want_daemon = 1;
        } else if (!strcmp(argv[i], "--pidfile") && i + 1 < argc) {
            g_pidfile = argv[++i];
        } else if (!strcmp(argv[i], "--statusfile") && i + 1 < argc) {
            g_statusfile = argv[++i];
        } else if (!strcmp(argv[i], "--max-runtime") && i + 1 < argc) {
            max_runtime = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--stop")) {
            do_stop = 1;
        } else if (!strcmp(argv[i], "--status")) {
            do_status = 1;
        } else {
            usage();
        }
    }

    if (speed <= 0 || max_runtime <= 0) usage();

    if (do_stop) { cmd_stop(); return 0; }
    if (do_status) { cmd_status(); return 0; }

    if (!have_steps && !continuous) usage();

    int existing = read_pidfile();
    if (pid_alive(existing)) {
        fprintf(stderr, "stepper: already running (pid %d), use --stop first\n", existing);
        return 1;
    }

    /* Reserve the expected on-time against the duty budget before doing
     * anything else - refuse up front rather than energize coils and find
     * out mid-run. --continuous reserves its full cap; if stopped early the
     * unused portion is simply not spent, which is fine (conservative). */
    long expected_ms = continuous
        ? (long)max_runtime * 1000
        : (steps < 0 ? -steps : steps) * 1000L / speed;
    if (!duty_reserve(expected_ms))
        return 1;

    if (continuous || want_daemon) daemonize();

    if (continuous)
        run_continuous(dir, speed, max_runtime);
    else
        run_steps(steps, speed);

    return 0;
}
