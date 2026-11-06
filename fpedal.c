/*
fpedal - safe programming tool for the FS3-P / PCsensor 1a86:e026 USB triple
foot switch on Linux.

Safety model:
  - The pedal's flash can only be written by sending a full 3-pedal image,
    so `set`/`clear` first READ all three pedals from the device and only
    replace the one you ask for. The other two are written back byte-for-byte.
  - If reading the current state fails, or returns an unknown type, the
    write is aborted.
  - `show` and `listen` are strictly read-only.

Protocol (reverse-engineered in rgerganov/footswitch, MIT license;
vendor/common.c is from that project):
  - Config channel: HID interface 1 of the pedal.
  - Read pedal n:  out {0x01, 0x82, 0x08, n, 0,0,0,0}; in 8-byte response.
      resp[1]: 0=unconfigured, 1/0x81=key, 2=mouse, 3=key+mouse, 4=string
      resp[2]=modifiers, resp[3]=keycode, resp[4]=button, resp[5..7]=x,y,w
      string: resp[0]=total len, payload starts at resp[2]
  - Write:         out {0x01, 0x80, 0x08, 0,0,0,0}, then for n=1..3:
                   out {0x01, 0x81, 0x08, n, 0,0,0,0} followed by the
                   pedal's 8-byte data record (data[0]=len, data[1]=type).

Copyright (c) 2025 - MIT (derived from rgerganov/footswitch, MIT).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <hidapi/hidapi.h>
#include "common.h"

#define VID 0x1a86
#define PID 0xe026
#define N_PEDALS 3

static hid_device *dev = NULL;

typedef struct {
    unsigned char header[8];
    unsigned char data[48];
    int data_len;
} pedal_data;

static pedal_data pedals[N_PEDALS];

/* ---------- low level ---------- */

static void fatal(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void open_device(void) {
    struct hid_device_info *info, *ptr;
    hid_init();
    info = hid_enumerate(VID, PID);
    for (ptr = info; ptr != NULL; ptr = ptr->next) {
        if (ptr->interface_number == 1) {
            dev = hid_open_path(ptr->path);
            break;
        }
    }
    hid_free_enumeration(info);
    if (!dev) {
        hid_exit();
        fprintf(stderr,
            "error: foot switch (1a86:e026, interface 1) not found.\n"
            "Check the USB cable and permissions (run as root, or install\n"
            "the udev rule: sudo cp 50-fpedal.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger)\n");
        exit(1);
    }
}

static void close_device(void) {
    if (dev) hid_close(dev);
    hid_exit();
}

static void usb_write(const unsigned char data[8]) {
    if (hid_write(dev, data, 8) < 0)
        fatal("hid_write failed (check permissions / device)");
    usleep(30 * 1000);
}

/* ---------- read ---------- */

static bool read_pedal(int idx) {
    unsigned char query[8] = {0x01, 0x82, 0x08, 0, 0, 0, 0, 0};
    unsigned char resp[8];
    pedal_data *p = &pedals[idx];

    query[3] = idx + 1;
    if (hid_write(dev, query, 8) < 0)
        return false;
    usleep(30 * 1000);
    if (hid_read_timeout(dev, resp, 8, 1000) < 0)
        return false;

    p->header[0] = 0x01;
    p->header[1] = 0x81;
    p->header[2] = 8;
    p->header[3] = idx + 1;
    memset(&p->header[4], 0, 4);

    memset(p->data, 0, sizeof(p->data));

    if (resp[1] == 4) {
        /* string type: pull in extra 8-byte chunks until the payload is complete */
        int total = resp[0];              /* includes the first 2 bytes */
        int left = total - 2;             /* string chars remaining */
        int got = 0;
        p->data[0] = resp[0];
        p->data[1] = 4;
        while (left > 0) {
            int take = left < 6 ? left : 6;   /* first chunk: resp[2..7] */
            if (got == 0) {
                memcpy(&p->data[2], &resp[2], take);
                left -= take;
                got += take;
            } else {
                unsigned char more[8];
                if (hid_read_timeout(dev, more, 8, 1000) < 0)
                    return false;
                take = left < 8 ? left : 8;
                memcpy(&p->data[2 + got], more, take);
                got += take;
                left -= take;
            }
        }
        p->data_len = total;
        p->header[2] = total;
    } else {
        memcpy(p->data, resp, 8);
        p->data[0] = 8;
        p->data_len = 8;
    }
    return true;
}

static bool read_all_pedals(void) {
    for (int i = 0; i < N_PEDALS; i++)
        if (!read_pedal(i)) {
            fprintf(stderr, "error: failed to read pedal %d from the device.\n", i + 1);
            return false;
        }
    return true;
}

/* ---------- printing ---------- */

static const char *modifier_names[8] = {
    "l_ctrl", "l_shift", "l_alt", "l_win", "r_ctrl", "r_shift", "r_alt", "r_win"
};

static void print_pedal(const pedal_data *p, const char *prefix) {
    unsigned char t = p->data[1];
    switch (t) {
        case 0:
            printf("%sunconfigured", prefix);
            break;
        case 1:
        case 0x81:
        case 2:
        case 3: {
            bool first = true;
            for (int i = 0; i < 8; i++) {
                if (p->data[2] & (1 << i)) {
                    if (!first) printf("+");
                    printf("%s", modifier_names[i]);
                    first = false;
                }
            }
            if (t == 1 || t == 0x81 || t == 3) {
                if (p->data[3] != 0) {
                    if (!first) printf("+");
                    printf("%s", decode_byte(p->data[3]));
                    first = false;
                }
            }
            if (t == 2 || t == 3) {
                if (p->data[4] & 1) { printf("%smouse_left", first ? "" : "+"); first = false; }
                if (p->data[4] & 2) { printf("%smouse_right", first ? "" : "+"); first = false; }
                if (p->data[4] & 4) { printf("%smouse_middle", first ? "" : "+"); first = false; }
                if (p->data[4] & 8) { printf("%smouse_double", first ? "" : "+"); first = false; }
            }
            if (p->data[5] || p->data[6] || p->data[7]) {
                printf("%sx:%d y:%d w:%d", first ? "" : " ",
                       p->data[5] > 127 ? p->data[5] - 256 : p->data[5],
                       p->data[6] > 127 ? p->data[6] - 256 : p->data[6],
                       p->data[7] > 127 ? p->data[7] - 256 : p->data[7]);
            }
            if (first) printf("unconfigured");
            break;
        }
        case 4: {
            int len = p->data[0] - 2;
            printf("%s\"", prefix);
            for (int i = 0; i < len && i < 38; i++) {
                unsigned char c = p->data[2 + i];
                if (c >= 32 && c < 127) putchar(c);
                else printf("\\x%02x", c);
            }
            printf("\"");
            break;
        }
        default:
            printf("%sUNKNOWN type 0x%02x (raw:", prefix, t);
            for (int i = 0; i < p->data_len; i++) printf(" %02x", p->data[i]);
            printf(")");
            break;
    }
}

static bool pedal_known_type(const pedal_data *p) {
    return p->data[1] == 0 || p->data[1] == 1 || p->data[1] == 2 ||
           p->data[1] == 3 || p->data[1] == 4 || p->data[1] == 0x81;
}

/* ---------- spec parsing ----------
 * none                      -> unconfigured
 * ctrl+shift+a              -> key combo (modifiers: ctrl|shift|alt|win,
 *                               optionally prefixed l_/r_)
 * mouse_left|mouse_right|mouse_middle
 * x:10 y:-5 w:2             -> mouse motion (may combine with a button)
 * key + mouse can be combined with '+'
 * str:hello world           -> string (standalone)
 */
typedef struct {
    unsigned char type;      /* 0 unconfigured, 1 key, 2 mouse, 3 both, 4 string */
    unsigned char mods;
    unsigned char key;
    unsigned char btn;
    int x, y, w;
    bool have_x, have_y, have_w;
    char str[40];
} spec;

static bool classify_part(const char *part, spec *s) {
    enum modifier m;
    enum mouse_button btn;
    unsigned char k;
    int v;

    if (parse_modifier(part, &m)) {
        s->mods |= m;
        return true;
    }
    if (sscanf(part, "x:%d", &v) == 1) {
        if (v < -128 || v > 127) { fprintf(stderr, "error: x out of range [-128,127]\n"); return false; }
        s->x = v; s->have_x = true; return true;
    }
    if (sscanf(part, "y:%d", &v) == 1) {
        if (v < -128 || v > 127) { fprintf(stderr, "error: y out of range [-128,127]\n"); return false; }
        s->y = v; s->have_y = true; return true;
    }
    if (sscanf(part, "w:%d", &v) == 1) {
        if (v < -128 || v > 127) { fprintf(stderr, "error: w out of range [-128,127]\n"); return false; }
        s->w = v; s->have_w = true; return true;
    }
    if (parse_mouse_button(part, &btn)) {
        if (s->btn) { fprintf(stderr, "error: only one mouse button per pedal\n"); return false; }
        s->btn = btn; return true;
    }
    if (encode_key(part, &k)) {
        if (s->key) { fprintf(stderr, "error: only one key per pedal\n"); return false; }
        s->key = k; return true;
    }
    fprintf(stderr, "error: cannot parse '%s' (try: keys like 'a','f5','enter', modifiers ctrl+shift+..., mouse_left, x:N y:N w:N, str:text)\n", part);
    return false;
}

static bool parse_spec(const char *arg, spec *s) {
    memset(s, 0, sizeof(*s));

    if (strcasecmp(arg, "none") == 0) {
        s->type = 0;
        return true;
    }
    if (strncmp(arg, "str:", 4) == 0) {
        const char *str = arg + 4;
        if (strlen(str) == 0 || strlen(str) > 38) {
            fprintf(stderr, "error: string must be 1..38 chars\n");
            return false;
        }
        s->type = 4;
        memcpy(s->str, str, strlen(str));
        return true;
    }

    char buf[128];
    strncpy(buf, arg, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    bool any = false;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        /* split each token on '+' and classify the parts */
        char *part_save = NULL;
        for (char *part = strtok_r(tok, "+", &part_save); part; part = strtok_r(NULL, "+", &part_save)) {
            if (!classify_part(part, s))
                return false;
            any = true;
        }
    }

    if (s->key || s->mods) s->type |= 1;
    if (s->btn || s->have_x || s->have_y || s->have_w) s->type |= 2;
    if (!any) {
        fprintf(stderr, "error: empty spec (use 'none' to clear)\n");
        return false;
    }
    return true;
}

static void spec_to_pedal(int idx, const spec *s, pedal_data *p) {
    memset(p->data, 0, sizeof(p->data));
    p->header[0] = 0x01;
    p->header[1] = 0x81;
    p->header[3] = idx + 1;

    if (s->type == 4) {
        int len = strlen(s->str);
        if (!encode_string(s->str, &p->data[2]))
            fatal("cannot encode string");
        p->data[0] = len + 2;
        p->data[1] = 4;
        p->data_len = len + 2;
        p->header[2] = p->data[0];
    } else {
        p->data[0] = 8;
        p->data[1] = s->type;
        p->data[2] = s->mods;
        p->data[3] = s->key;
        p->data[4] = s->btn;
        p->data[5] = s->have_x ? (unsigned char)(s->x < 0 ? s->x + 256 : s->x) : 0;
        p->data[6] = s->have_y ? (unsigned char)(s->y < 0 ? s->y + 256 : s->y) : 0;
        p->data[7] = s->have_w ? (unsigned char)(s->w < 0 ? s->w + 256 : s->w) : 0;
        p->data_len = 8;
        p->header[2] = 8;
    }
}

/* ---------- write ---------- */

static void write_pedal(const pedal_data *p) {
    unsigned char data[8];
    int arr_ind = 0, data_ind = 0;

    usb_write(p->header);
    memset(data, 0, 8);
    while (arr_ind < p->data_len) {
        if (data_ind == 8) {
            usb_write(data);
            memset(data, 0, 8);
            data_ind = 0;
        }
        data[data_ind++] = p->data[arr_ind++];
    }
    usb_write(data);
}

static void write_all_pedals(void) {
    unsigned char start[8] = {0x01, 0x80, 0x08, 0, 0, 0, 0, 0};
    usb_write(start);
    usleep(1000 * 1000);
    for (int i = 0; i < N_PEDALS; i++)
        write_pedal(&pedals[i]);
}

/* ---------- commands ---------- */

static void cmd_show(void) {
    open_device();
    if (!read_all_pedals()) {
        close_device();
        exit(1);
    }
    printf("FS3-P foot switch (1a86:e026) current configuration:\n");
    for (int i = 0; i < N_PEDALS; i++) {
        printf("  pedal %d: ", i + 1);
        print_pedal(&pedals[i], "");
        printf("\n");
    }
    close_device();
}

static void cmd_set(int idx, const char *specstr, bool yes) {
    spec s;
    if (!parse_spec(specstr, &s))
        exit(1);

    open_device();
    if (!read_all_pedals()) {
        close_device();
        exit(1);
    }
    for (int i = 0; i < N_PEDALS; i++) {
        if (!pedal_known_type(&pedals[i])) {
            fprintf(stderr, "error: pedal %d returned an unknown type; aborting to be safe.\n", i + 1);
            close_device();
            exit(1);
        }
    }

    pedal_data old = pedals[idx];
    spec_to_pedal(idx, &s, &pedals[idx]);

    printf("  pedal %d:\n", idx + 1);
    printf("    was:  "); print_pedal(&old, ""); printf("\n");
    printf("    now:  "); print_pedal(&pedals[idx], ""); printf("\n");

    if (!yes) {
        char answer[16];
        printf("Write to pedal firmware? [y/N] ");
        fflush(stdout);
        if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
            printf("aborted, nothing written.\n");
            close_device();
            exit(0);
        }
    }

    /* remember expected bytes for verification */
    unsigned char expected[48];
    int expected_len = pedals[idx].data_len;
    memcpy(expected, pedals[idx].data, sizeof(expected));

    write_all_pedals();
    usleep(200 * 1000);

    /* verify with a read-back */
    if (!read_pedal(idx)) {
        fprintf(stderr, "warning: could not re-read pedal %d after write.\n", idx + 1);
        close_device();
        exit(1);
    }
    printf("  pedal %d after write: ", idx + 1);
    print_pedal(&pedals[idx], "");
    printf("\n");
    if (memcmp(&pedals[idx].data, expected, expected_len) != 0)
        fprintf(stderr, "warning: read-back differs from what was programmed!\n");
    else
        printf("read-back OK.\n");
    close_device();
}

static void cmd_listen(int seconds) {
    char path[512];
    snprintf(path, sizeof(path), "/dev/input/by-id/usb-%04x_%04x-event-kbd", VID, PID);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* try other by-id entries */
        DIR *dir;
        struct dirent *ent;
        int found = 0;
        dir = opendir("/dev/input/by-id");
        if (!dir) fatal("cannot open /dev/input/by-id");
        while ((ent = readdir(dir)) != NULL) {
            if (strstr(ent->d_name, "e026") && strstr(ent->d_name, "kbd")) {
                snprintf(path, sizeof(path), "/dev/input/by-id/%s", ent->d_name);
                fd = open(path, O_RDONLY);
                if (fd >= 0) { found = 1; break; }
            }
        }
        closedir(dir);
        if (!found) fatal("cannot open keyboard event device (try sudo)");
    }

    const struct { int code; const char *name; } names[] = {
        {KEY_A, "a"}, {KEY_B, "b"}, {KEY_C, "c"}, {KEY_D, "d"}, {KEY_E, "e"},
        {KEY_F, "f"}, {KEY_G, "g"}, {KEY_H, "h"}, {KEY_I, "i"}, {KEY_J, "j"},
        {KEY_K, "k"}, {KEY_L, "l"}, {KEY_M, "m"}, {KEY_N, "n"}, {KEY_O, "o"},
        {KEY_P, "p"}, {KEY_Q, "q"}, {KEY_R, "r"}, {KEY_S, "s"}, {KEY_T, "t"},
        {KEY_U, "u"}, {KEY_V, "v"}, {KEY_W, "w"}, {KEY_X, "x"}, {KEY_Y, "y"},
        {KEY_Z, "z"},
        {KEY_1, "1"}, {KEY_2, "2"}, {KEY_3, "3"}, {KEY_4, "4"}, {KEY_5, "5"},
        {KEY_6, "6"}, {KEY_7, "7"}, {KEY_8, "8"}, {KEY_9, "9"}, {KEY_0, "0"},
        {KEY_LEFTCTRL, "lctrl"}, {KEY_LEFTSHIFT, "lshift"}, {KEY_LEFTALT, "lalt"},
        {KEY_LEFTMETA, "lwin"}, {KEY_RIGHTCTRL, "rctrl"}, {KEY_RIGHTSHIFT, "rshift"},
        {KEY_RIGHTALT, "ralt"}, {KEY_RIGHTMETA, "rwin"},
        {KEY_ENTER, "enter"}, {KEY_TAB, "tab"}, {KEY_SPACE, "space"},
        {KEY_F1, "f1"}, {KEY_F2, "f2"}, {KEY_F3, "f3"}, {KEY_F4, "f4"},
        {KEY_F5, "f5"}, {KEY_F6, "f6"}, {KEY_F7, "f7"}, {KEY_F8, "f8"},
        {KEY_F9, "f9"}, {KEY_F10, "f10"}, {KEY_F11, "f11"}, {KEY_F12, "f12"},
        {KEY_ESC, "esc"}, {KEY_BACKSPACE, "backspace"},
    };
    #define N_NAMES (sizeof(names)/sizeof(names[0]))

    printf("listening on %s for %d seconds (press pedals / Ctrl-C to stop)...\n", path, seconds);
    struct input_event ev;
    for (;;) {
        fd_set fds;
        struct timeval tv = {1, 0};
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            seconds--;
            if (seconds <= 0) break;
            continue;
        }
        if (read(fd, &ev, sizeof(ev)) < (int)sizeof(ev))
            continue;
        if (ev.type != EV_KEY)
            continue;
        const char *name = "?";
        for (size_t i = 0; i < N_NAMES; i++)
            if (names[i].code == ev.code) { name = names[i].name; break; }
        printf("  key %-10s %s\n", name, ev.value ? "down" : "up");
        fflush(stdout);
    }
    close(fd);
}

/* ---------- main ---------- */

static void usage(void) {
    printf(
"usage: fpedal <command> [args]\n"
"\n"
"commands:\n"
"  show                     show current pedal configuration (read-only)\n"
"  set <1|2|3> <spec>       program one pedal, preserving the other two\n"
"                           (add --yes to skip the confirmation prompt)\n"
"  clear <1|2|3>            reset one pedal to unconfigured, preserving others\n"
"  listen [seconds]         watch key events from the pedal (default 15s)\n"
"\n"
"spec formats:\n"
"  none                     clear the pedal\n"
"  a | f5 | enter           a single key\n"
"  ctrl+shift+a             key with modifiers (ctrl|shift|alt|win, l_/r_ prefix)\n"
"  mouse_left               mouse button (mouse_left|mouse_right|mouse_middle)\n"
"  x:10 y:-5 w:2            mouse motion, combinable with a button\n"
"  ctrl+a mouse_left        key and mouse button combined\n"
"  str:hello world          type a string (standalone, max 38 chars)\n"
"\n"
"examples:\n"
"  fpedal show\n"
"  fpedal set 1 ctrl+shift+f\n"
"  fpedal set 2 mouse_left\n"
"  fpedal set 3 str:hello\n"
"  fpedal clear 2\n"
"  fpedal listen 10\n"
"\n"
"safety: 'set'/'clear' read the device first and rewrite all three pedals so\n"
"unmentioned pedals keep their current mapping. If the read fails, nothing\n"
"is written.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }

    bool yes = false;
    for (int i = 2; i < argc; i++)
        if (strcmp(argv[i], "--yes") == 0 || strcmp(argv[i], "-y") == 0)
            yes = true;

    if (strcmp(argv[1], "show") == 0) {
        cmd_show();
    } else if (strcmp(argv[1], "set") == 0 || strcmp(argv[1], "clear") == 0) {
        if (argc < 4) { usage(); return 1; }
        int idx = atoi(argv[2]);
        if (idx < 1 || idx > 3) {
            fprintf(stderr, "error: pedal must be 1, 2 or 3\n");
            return 1;
        }
        const char *spec = strcmp(argv[1], "clear") == 0 ? "none" : argv[3];
        cmd_set(idx - 1, spec, yes);
    } else if (strcmp(argv[1], "listen") == 0) {
        int seconds = argc > 2 ? atoi(argv[2]) : 15;
        cmd_listen(seconds);
    } else if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
    } else {
        fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
        usage();
        return 1;
    }
    return 0;
}
