#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <vulkan/vulkan.h>

#define LABEL_LEN 14 /* widest label: "Runtime Loader", "VULKAN_SDK Env" */
#define LABEL_WIDTH (LABEL_LEN + 2) /* the label padded, then ": " */
#define FALLBACK_COLS 80 /* used when stdout is not a terminal */
#define MIN_COLS 40
#define MIN_PATH_BUDGET 3 /* below this a path collapses to "..." */
#define MAX_COMPONENTS 128
#define ELISION "/.../"
#define ELISION_HEAD ".../"
#define VALUE_MAX (PATH_MAX * 2 + 64)
#define OUTPUT_MAX (VALUE_MAX * 4 + LABEL_WIDTH * 4 + 64)

static int verbose = 0;
static int columns = FALLBACK_COLS;

static char output[OUTPUT_MAX];
static size_t output_len;

/* Append one "label: value" line to the report. Padding every label to the
 * same width is what lines up both the colons and the values. */
static void append_line(const char *label, const char *value) {
    size_t room = sizeof(output) - output_len;
    if (room <= 1) return;
    int n = snprintf(output + output_len, room, "%-*s: %s\n", LABEL_LEN, label, value);
    if (n < 0) return;
    output_len += (size_t)n < room ? (size_t)n : room - 1;
}

static void detect_columns(void) {
    struct winsize ws;
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= MIN_COLS) {
        columns = ws.ws_col;
    }
}

/* Columns still free on a line that already prints `used` columns. */
static size_t path_budget(size_t used) {
    if (verbose) return SIZE_MAX;
    size_t avail = (size_t)columns > used ? (size_t)columns - used : 0;
    return avail < MIN_PATH_BUDGET ? MIN_PATH_BUDGET : avail;
}

/* Copy `path` into `out`, eliding interior components until it fits in `budget`
 * columns. The first component says where the tree is rooted and the last ones
 * name the file, so those are kept and the middle goes first. A budget too small
 * for the filename alone leaves a bare "...", which -v spells out. */
static void shorten_path(const char *path, size_t budget, char *out, size_t outsz) {
    size_t n = strlen(path);
    if (n <= budget) {
        snprintf(out, outsz, "%s", path);
        return;
    }

    const char *comp[MAX_COMPONENTS];
    size_t count = 0;
    for (const char *p = path; *p != '\0' && count < MAX_COMPONENTS; p++) {
        if (*p == '/') continue;
        if (p == path || *(p - 1) == '/') comp[count++] = p;
    }

    if (count == 0) { /* a path of nothing but separators */
        snprintf(out, outsz, "...");
        return;
    }

    if (count >= 2) {
        size_t head_len = (size_t)(comp[1] - path) - 1;
        /* Tail length shrinks as i grows, so the first fit keeps the most. */
        for (size_t i = 1; i < count; i++) {
            size_t total = head_len + (sizeof(ELISION) - 1) + n - (size_t)(comp[i] - path);
            if (total <= budget) {
                snprintf(out, outsz, "%.*s" ELISION "%s", (int)head_len, path, comp[i]);
                return;
            }
        }
    }

    size_t last_len = n - (size_t)(comp[count - 1] - path);
    if (sizeof(ELISION_HEAD) - 1 + last_len <= budget) {
        snprintf(out, outsz, ELISION_HEAD "%s", comp[count - 1]);
        return;
    }

    snprintf(out, outsz, "...");
}

void collect_header_version(void) {
    uint32_t ver = VK_HEADER_VERSION_COMPLETE;
    char value[48];
    snprintf(value, sizeof(value), "%u.%u.%u",
             VK_API_VERSION_MAJOR(ver),
             VK_API_VERSION_MINOR(ver),
             VK_API_VERSION_PATCH(ver));
    append_line("Headers", value);
}

void collect_loader_version(void) {
    uint32_t ver = 0;
    char value[48];
    if (vkEnumerateInstanceVersion(&ver) == VK_SUCCESS) {
        snprintf(value, sizeof(value), "%u.%u.%u",
                 VK_API_VERSION_MAJOR(ver),
                 VK_API_VERSION_MINOR(ver),
                 VK_API_VERSION_PATCH(ver));
    } else {
        snprintf(value, sizeof(value), "Failed to query");
    }
    append_line("Runtime Loader", value);
}

void collect_sdk_env_version(void) {
    const char *sdk_path = getenv("VULKAN_SDK");
    if (!sdk_path) {
        append_line("VULKAN_SDK Env", "Not set");
        return;
    }

    char shown[PATH_MAX];
    char value[VALUE_MAX];
    const char *macos_dir = strstr(sdk_path, "/macOS");
    if (macos_dir && macos_dir > sdk_path) {
        const char *ver_start = macos_dir - 1;
        while (ver_start > sdk_path && *ver_start != '/') {
            ver_start--;
        }
        if (*ver_start == '/') ver_start++;
        size_t ver_len = (size_t)(macos_dir - ver_start);

        size_t used = LABEL_WIDTH + ver_len + sizeof(" (from ") - 1 + sizeof(")") - 1 +
                      sizeof("\n") - 1;
        shorten_path(sdk_path, path_budget(used), shown, sizeof(shown));
        snprintf(value, sizeof(value), "%.*s (from %s)", (int)ver_len, ver_start, shown);
    } else {
        size_t used = LABEL_WIDTH + sizeof("Set to ") - 1 + sizeof(" (non-versioned path)") - 1 +
                      sizeof("\n") - 1;
        shorten_path(sdk_path, path_budget(used), shown, sizeof(shown));
        snprintf(value, sizeof(value), "Set to %s (non-versioned path)", shown);
    }
    append_line("VULKAN_SDK Env", value);
}

void collect_dylib_symlink_version(void) {
    // Check VULKAN_SDK/lib first, fall back to global /usr/local/lib
    char dylib_path[PATH_MAX];
    const char *sdk_path = getenv("VULKAN_SDK");
    if (sdk_path) {
        snprintf(dylib_path, sizeof(dylib_path), "%s/lib/libvulkan.1.dylib", sdk_path);
    } else {
        snprintf(dylib_path, sizeof(dylib_path), "/usr/local/lib/libvulkan.1.dylib");
    }

    char shown[PATH_MAX];
    char target[PATH_MAX];
    char value[VALUE_MAX];
    ssize_t len = readlink(dylib_path, target, sizeof(target) - 1);
    if (len != -1) {
        target[len] = '\0';
        // Symlink target typically resolves to "libvulkan.1.x.xxx.dylib"
        const char *filename = strrchr(target, '/');
        filename = filename ? filename + 1 : target;

        size_t used = LABEL_WIDTH + sizeof(" -> ") - 1 + strlen(filename) + sizeof("\n") - 1;
        shorten_path(dylib_path, path_budget(used), shown, sizeof(shown));
        snprintf(value, sizeof(value), "%s -> %s", shown, filename);
    } else {
        size_t used = LABEL_WIDTH + sizeof("Could not resolve ") - 1 + sizeof("\n") - 1;
        shorten_path(dylib_path, path_budget(used), shown, sizeof(shown));
        snprintf(value, sizeof(value), "Could not resolve %s", shown);
    }
    append_line("Dylib", value);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-v]\n"
            "\n"
            "  -v, --verbose   print full paths instead of abbreviating them\n"
            "  -h, --help      show this help\n",
            prog);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "vk_version: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    detect_columns();
    collect_header_version();
    collect_loader_version();
    collect_sdk_env_version();
    collect_dylib_symlink_version();
    fputs(output, stdout);
    return 0;
}
