#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "assets/alert_wav.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BAR_WIDTH 24
#define NS_PER_SECOND 1000000000LL

struct timer_options {
    long long total_seconds;
    bool repeat;
    bool sound;
    bool notify;
};

static const char *program_name_from_path(const char *path) {
    const char *slash;

    if (path == NULL || *path == '\0') {
        return "ctimer";
    }

    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void print_usage(FILE *stream, const char *program_name) {
    const char *name = program_name_from_path(program_name);

    fprintf(stream,
            "Usage:\n"
            "  %s HH:MM:SS [--repeat] [--sound] [--notify]\n"
            "  %s help\n"
            "  %s -h\n"
            "  %s --help\n"
            "\n"
            "Flags:\n"
            "  --repeat   Wait for Enter to restart the same timer, or q to quit.\n"
            "  --sound    Play the bundled alert sound when the timer completes.\n"
            "  --notify   Send a best-effort desktop notification with notify-send.\n"
            "  -h, --help Show this help message.\n"
            "\n"
            "Examples:\n"
            "  %s 00:30:00\n"
            "  %s 00:05:00 --sound --notify\n"
            "  %s 00:00:10 --repeat\n"
            "\n"
            "Notes:\n"
            "  Linux-first implementation.\n"
            "  Sound and desktop notifications are optional integrations.\n",
            name,
            name,
            name,
            name,
            name,
            name,
            name);
}

static int parse_unsigned_part(const char *start,
                               size_t length,
                               long long max_value,
                               long long *value) {
    size_t i;
    long long result = 0;

    if (length == 0) {
        return -1;
    }

    for (i = 0; i < length; ++i) {
        int digit;

        if (!isdigit((unsigned char) start[i])) {
            return -1;
        }

        digit = start[i] - '0';
        if (result > (max_value - digit) / 10) {
            return -1;
        }
        result = (result * 10) + digit;
    }

    *value = result;
    return 0;
}

static int parse_time_string(const char *text, long long *total_seconds) {
    const char *first_colon;
    const char *second_colon;
    long long hours;
    long long minutes;
    long long seconds;
    long long max_total_seconds = LLONG_MAX / NS_PER_SECOND;
    long long total;

    if (text == NULL) {
        return -1;
    }

    first_colon = strchr(text, ':');
    if (first_colon == NULL) {
        return -1;
    }

    second_colon = strchr(first_colon + 1, ':');
    if (second_colon == NULL || strchr(second_colon + 1, ':') != NULL) {
        return -1;
    }

    if (parse_unsigned_part(text,
                            (size_t) (first_colon - text),
                            max_total_seconds / 3600,
                            &hours) != 0) {
        return -1;
    }

    if (parse_unsigned_part(first_colon + 1,
                            (size_t) (second_colon - first_colon - 1),
                            59,
                            &minutes) != 0) {
        return -1;
    }

    if (parse_unsigned_part(second_colon + 1,
                            strlen(second_colon + 1),
                            59,
                            &seconds) != 0) {
        return -1;
    }

    if (hours > (max_total_seconds - (minutes * 60) - seconds) / 3600) {
        return -1;
    }

    total = (hours * 3600) + (minutes * 60) + seconds;
    if (total <= 0) {
        return -1;
    }

    *total_seconds = total;
    return 0;
}

static void format_duration(long long total_seconds, char *buffer, size_t buffer_size) {
    long long hours = total_seconds / 3600;
    long long minutes = (total_seconds % 3600) / 60;
    long long seconds = total_seconds % 60;

    snprintf(buffer, buffer_size, "%lld:%02lld:%02lld", hours, minutes, seconds);
}

static long long monotonic_time_ns(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (ts.tv_sec * NS_PER_SECOND) + ts.tv_nsec;
}

static void render_progress_line(long long total_seconds,
                                 long long remaining_seconds,
                                 long long elapsed_ns,
                                 long long total_ns,
                                 bool interactive_stdout) {
    char time_buffer[32];
    char bar[BAR_WIDTH + 1];
    int filled;
    int percent;
    int i;

    if (elapsed_ns < 0) {
        elapsed_ns = 0;
    }
    if (elapsed_ns > total_ns) {
        elapsed_ns = total_ns;
    }

    filled = total_ns == 0 ? BAR_WIDTH : (int) ((elapsed_ns * BAR_WIDTH) / total_ns);
    percent = total_ns == 0 ? 100 : (int) ((elapsed_ns * 100) / total_ns);

    if (remaining_seconds == 0) {
        filled = BAR_WIDTH;
        percent = 100;
    }

    for (i = 0; i < BAR_WIDTH; ++i) {
        bar[i] = i < filled ? '*' : '-';
    }
    bar[BAR_WIDTH] = '\0';

    format_duration(remaining_seconds, time_buffer, sizeof(time_buffer));

    if (interactive_stdout) {
        printf("\rremaining %s [%s] %3d%%", time_buffer, bar, percent);
    } else {
        printf("remaining %s [%s] %3d%%\n", time_buffer, bar, percent);
    }
    fflush(stdout);

    (void) total_seconds;
}

static void run_timer(long long total_seconds, bool interactive_stdout) {
    long long total_ns = total_seconds * NS_PER_SECOND;
    long long start_ns = monotonic_time_ns();
    long long last_remaining = -1;
    struct timespec pause_time = {0, 100000000L};

    for (;;) {
        long long now_ns = monotonic_time_ns();
        long long elapsed_ns = now_ns - start_ns;
        long long remaining_ns;
        long long remaining_seconds;

        if (elapsed_ns < 0) {
            elapsed_ns = 0;
        }
        if (elapsed_ns > total_ns) {
            elapsed_ns = total_ns;
        }

        remaining_ns = total_ns - elapsed_ns;
        remaining_seconds = remaining_ns == 0
            ? 0
            : (remaining_ns + NS_PER_SECOND - 1) / NS_PER_SECOND;

        if (remaining_seconds != last_remaining || remaining_seconds == 0) {
            render_progress_line(total_seconds,
                                 remaining_seconds,
                                 elapsed_ns,
                                 total_ns,
                                 interactive_stdout);
            last_remaining = remaining_seconds;
        }

        if (remaining_ns == 0) {
            break;
        }

        nanosleep(&pause_time, NULL);
    }

    if (interactive_stdout) {
        putchar('\n');
    }
}

static int find_command_in_path(const char *command, char *resolved_path, size_t resolved_size) {
    const char *path_env;
    const char *segment_start;

    if (command == NULL || *command == '\0') {
        return 0;
    }

    if (strchr(command, '/') != NULL) {
        if (access(command, X_OK) == 0) {
            snprintf(resolved_path, resolved_size, "%s", command);
            return 1;
        }
        return 0;
    }

    path_env = getenv("PATH");
    if (path_env == NULL || *path_env == '\0') {
        return 0;
    }

    segment_start = path_env;
    while (*segment_start != '\0') {
        const char *segment_end = strchr(segment_start, ':');
        size_t segment_length = segment_end != NULL
            ? (size_t) (segment_end - segment_start)
            : strlen(segment_start);

        if (segment_length == 0) {
            if (snprintf(resolved_path, resolved_size, "./%s", command) < (int) resolved_size &&
                access(resolved_path, X_OK) == 0) {
                return 1;
            }
        } else if (snprintf(resolved_path,
                            resolved_size,
                            "%.*s/%s",
                            (int) segment_length,
                            segment_start,
                            command) < (int) resolved_size &&
                   access(resolved_path, X_OK) == 0) {
            return 1;
        }

        if (segment_end == NULL) {
            break;
        }

        segment_start = segment_end + 1;
    }

    return 0;
}

static int run_command(const char *resolved_path, char *const argv[]) {
    pid_t child_pid;
    int status;

    child_pid = fork();
    if (child_pid < 0) {
        return -1;
    }

    if (child_pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }

        execv(resolved_path, argv);
        _exit(127);
    }

    do {
        if (waitpid(child_pid, &status, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        break;
    } while (1);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }

    return 0;
}

static int send_desktop_notification(char *warning, size_t warning_size) {
    char command_path[PATH_MAX];
    char *const argv[] = {"notify-send", "ctimer", "Timer complete.", NULL};

    if (!find_command_in_path("notify-send", command_path, sizeof(command_path))) {
        snprintf(warning,
                 warning_size,
                 "notify-send not available; skipped desktop notification.");
        return -1;
    }

    if (run_command(command_path, argv) != 0) {
        snprintf(warning,
                 warning_size,
                 "notify-send failed; skipped desktop notification.");
        return -1;
    }

    return 0;
}

static int write_embedded_sound_file(char *temp_path, size_t temp_path_size) {
    char temp_template[] = "/tmp/ctimer-alert-XXXXXX";
    size_t total_written = 0;
    int temp_fd = mkstemp(temp_template);

    if (temp_fd < 0) {
        return -1;
    }

    while (total_written < assets_alert_wav_len) {
        ssize_t bytes_written = write(temp_fd,
                                      assets_alert_wav + total_written,
                                      assets_alert_wav_len - total_written);

        if (bytes_written < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(temp_fd);
            unlink(temp_template);
            return -1;
        }

        total_written += (size_t) bytes_written;
    }

    if (close(temp_fd) != 0) {
        unlink(temp_template);
        return -1;
    }

    if (snprintf(temp_path, temp_path_size, "%s", temp_template) >= (int) temp_path_size) {
        unlink(temp_template);
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

static int play_alert_sound(char *warning, size_t warning_size) {
    char temp_path[PATH_MAX];
    char command_path[PATH_MAX];
    char *const paplay_argv[] = {"paplay", temp_path, NULL};
    char *const aplay_argv[] = {"aplay", "-q", temp_path, NULL};
    bool paplay_found = false;
    int result = -1;

    if (write_embedded_sound_file(temp_path, sizeof(temp_path)) != 0) {
        snprintf(warning,
                 warning_size,
                 "could not materialize embedded alert sound; skipped sound playback.");
        return -1;
    }

    if (find_command_in_path("paplay", command_path, sizeof(command_path))) {
        paplay_found = true;
        if (run_command(command_path, paplay_argv) == 0) {
            result = 0;
            goto cleanup;
        }
    }

    if (find_command_in_path("aplay", command_path, sizeof(command_path))) {
        if (run_command(command_path, aplay_argv) == 0) {
            result = 0;
            goto cleanup;
        }

        snprintf(warning,
                 warning_size,
                 paplay_found
                     ? "paplay failed and aplay also failed; skipped sound playback."
                     : "aplay failed; skipped sound playback.");
        goto cleanup;
    }

    snprintf(warning,
             warning_size,
             paplay_found
                 ? "paplay failed and aplay is not available; skipped sound playback."
                 : "sound requested but neither paplay nor aplay is available.");

cleanup:
    unlink(temp_path);
    return result;
}

static void emit_completion_alerts(const struct timer_options *options) {
    char warning[160];

    if (options->notify) {
        if (send_desktop_notification(warning, sizeof(warning)) != 0) {
            fprintf(stderr, "Warning: %s\n", warning);
        }
    }

    if (options->sound) {
        if (play_alert_sound(warning, sizeof(warning)) != 0) {
            fprintf(stderr, "Warning: %s\n", warning);
        }
    }
}

static int wait_for_repeat_choice(void) {
    struct termios original_termios;
    struct termios raw_termios;

    if (tcgetattr(STDIN_FILENO, &original_termios) != 0) {
        return -1;
    }

    raw_termios = original_termios;
    raw_termios.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw_termios.c_cc[VMIN] = 1;
    raw_termios.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios) != 0) {
        return -1;
    }

    for (;;) {
        unsigned char key;
        ssize_t bytes_read = read(STDIN_FILENO, &key, 1);

        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
            return -1;
        }

        if (bytes_read == 0) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
            return 0;
        }

        if (key == '\n' || key == '\r') {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
            return 1;
        }

        if (key == 'q' || key == 'Q' || key == 3 || key == 4) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
            return 0;
        }
    }
}

static int parse_options(int argc,
                         char *argv[],
                         struct timer_options *options,
                         const char **duration_text) {
    int i;

    memset(options, 0, sizeof(*options));
    *duration_text = NULL;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--repeat") == 0) {
            options->repeat = true;
        } else if (strcmp(argv[i], "--sound") == 0) {
            options->sound = true;
        } else if (strcmp(argv[i], "--notify") == 0) {
            options->notify = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'.\n", argv[i]);
            return -1;
        } else if (*duration_text == NULL) {
            *duration_text = argv[i];
        } else {
            fprintf(stderr, "Error: expected exactly one HH:MM:SS duration.\n");
            return -1;
        }
    }

    if (*duration_text == NULL) {
        fprintf(stderr, "Error: missing HH:MM:SS duration.\n");
        return -1;
    }

    if (parse_time_string(*duration_text, &options->total_seconds) != 0) {
        fprintf(stderr,
                "Error: invalid duration '%s'. Expected HH:MM:SS with minutes and seconds in 00-59.\n",
                *duration_text);
        return -1;
    }

    if (options->repeat && !isatty(STDIN_FILENO)) {
        fprintf(stderr, "Error: --repeat requires an interactive stdin TTY.\n");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    struct timer_options options;
    const char *duration_text = NULL;
    bool interactive_stdout = isatty(STDOUT_FILENO);

    if (argc == 2 &&
        (strcmp(argv[1], "help") == 0 ||
         strcmp(argv[1], "-h") == 0 ||
         strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout, argv[0]);
        return EXIT_SUCCESS;
    }

    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    if (parse_options(argc, argv, &options, &duration_text) != 0) {
        fprintf(stderr, "\n");
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    (void) duration_text;

    for (;;) {
        int repeat_choice;

        run_timer(options.total_seconds, interactive_stdout);
        printf("Timer complete.\n");
        fflush(stdout);
        emit_completion_alerts(&options);

        if (!options.repeat) {
            break;
        }

        printf("Press Enter to restart or q to quit: ");
        fflush(stdout);
        repeat_choice = wait_for_repeat_choice();
        putchar('\n');

        if (repeat_choice < 0) {
            perror("repeat input");
            return EXIT_FAILURE;
        }

        if (repeat_choice == 0) {
            break;
        }
    }

    return EXIT_SUCCESS;
}
