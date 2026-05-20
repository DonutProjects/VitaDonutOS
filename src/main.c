#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "debugScreen.h"
#include "debugScreenFont.c"

#define DONUTOS_VERSION "4.2"
#define DONUTOS_HOME "ux0:data/DonutOS"
#define CREATOR_USERNAME "alex92567"
#define IDEA_CREATOR_USERNAME "iamdonut215"

#define LOG_LINES 128
#define LINE_LEN 160
#define INPUT_LEN 256
#define PATH_LEN 512
#define SCREEN_W 960
#define SCREEN_H 544
#define SCREEN_PITCH 1024
#define FB_SIZE (2 * 1024 * 1024)

#define BTN_CONFIRM SCE_CTRL_CROSS
#define BTN_BACK SCE_CTRL_SQUARE
#define BTN_SUBMIT SCE_CTRL_CIRCLE
#define BTN_SPACE SCE_CTRL_TRIANGLE

static char log_lines[LOG_LINES][LINE_LEN];
static int log_count = 0;
static char cwd[PATH_LEN] = DONUTOS_HOME;
static bool app_running = true;
static unsigned int prev_buttons = 0;
static SceUID fb_block = -1;
static uint32_t *fb = NULL;
static int fb_x = 0;
static int fb_y = 0;
static uint32_t fb_fg = 0xffffffff;
static uint32_t fb_bg = 0xff000000;

static const char *kbd_rows[] = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789_-.:/",
    " !?\"'(),"
};

static void wait_vblank(void) {
    sceDisplayWaitVblankStart();
}

static uint32_t rgb_to_a8b8g8r8(unsigned int r, unsigned int g, unsigned int b) {
    return 0xff000000u | ((b & 0xffu) << 16) | ((g & 0xffu) << 8) | (r & 0xffu);
}

static void video_present(void) {
    if (!fb) {
        return;
    }
    SceDisplayFrameBuf frame = {
        sizeof(frame),
        fb,
        SCREEN_PITCH,
        SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        SCREEN_W,
        SCREEN_H
    };
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_IMMEDIATE);
}

static int video_init(void) {
    fb_block = sceKernelAllocMemBlock("donutos_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_SIZE, NULL);
    if (fb_block < 0) {
        return fb_block;
    }
    int ret = sceKernelGetMemBlockBase(fb_block, (void **)&fb);
    if (ret < 0) {
        return ret;
    }
    memset(fb, 0, FB_SIZE);
    video_present();
    return 0;
}

static void video_clear(uint32_t color) {
    if (!fb) {
        return;
    }
    for (int y = 0; y < SCREEN_H; ++y) {
        uint32_t *row = fb + y * SCREEN_PITCH;
        for (int x = 0; x < SCREEN_W; ++x) {
            row[x] = color;
        }
    }
    fb_x = 0;
    fb_y = 0;
}

static void video_set_pos(int x, int y) {
    fb_x = x;
    fb_y = y;
}

static void video_newline(void) {
    fb_x = 0;
    fb_y += psvDebugScreenFont.size_h;
}

static void video_put_char(unsigned char ch) {
    if (!fb) {
        return;
    }
    if (ch == '\n') {
        video_newline();
        return;
    }
    if (ch == '\r') {
        fb_x = 0;
        return;
    }
    if (ch == '\t') {
        fb_x += psvDebugScreenFont.size_w * 4;
        return;
    }
    if (fb_x + psvDebugScreenFont.size_w >= SCREEN_W) {
        video_newline();
    }
    if (fb_y + psvDebugScreenFont.size_h >= SCREEN_H) {
        fb_x = 0;
        fb_y = 0;
    }

    int draw_dummy = (ch < psvDebugScreenFont.first || ch > psvDebugScreenFont.last);
    int bits_per_glyph = psvDebugScreenFont.width * psvDebugScreenFont.height;
    int bitmap_offset = draw_dummy ? 0 : (ch - psvDebugScreenFont.first) * bits_per_glyph;
    unsigned char *font = draw_dummy ? NULL : &psvDebugScreenFont.glyphs[bitmap_offset / 8];
    unsigned char mask = 1 << 7;
    for (int i = bitmap_offset % 8; i > 0; --i) {
        mask >>= 1;
    }

    for (int row = 0; row < psvDebugScreenFont.size_h; ++row) {
        uint32_t *pixel = fb + (fb_y + row) * SCREEN_PITCH + fb_x;
        for (int col = 0; col < psvDebugScreenFont.size_w; ++col) {
            uint32_t color = fb_bg;
            if (row < psvDebugScreenFont.height && col < psvDebugScreenFont.width) {
                if (draw_dummy) {
                    color = (row == psvDebugScreenFont.height / 2 && (col & 1)) ? fb_fg : fb_bg;
                } else {
                    if (!mask) {
                        font++;
                        mask = 1 << 7;
                    }
                    color = (*font & mask) ? fb_fg : fb_bg;
                    mask >>= 1;
                }
            }
            *pixel++ = color;
        }
    }
    fb_x += psvDebugScreenFont.size_w;
}

static void video_puts(const char *text) {
    for (size_t i = 0; text[i]; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\033' && text[i + 1] == '[') {
            i += 2;
            while (text[i] && !isalpha((unsigned char)text[i])) {
                i++;
            }
            continue;
        }
        video_put_char(ch);
    }
}

static void video_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    video_puts(buf);
}

static unsigned int pressed_buttons(void) {
    SceCtrlData ctrl;
    sceCtrlPeekBufferPositive(0, &ctrl, 1);
    unsigned int pressed = ctrl.buttons & ~prev_buttons;
    prev_buttons = ctrl.buttons;
    return pressed;
}

static void log_raw(const char *line) {
    if (log_count == LOG_LINES) {
        memmove(log_lines, log_lines + 1, sizeof(log_lines[0]) * (LOG_LINES - 1));
        log_count--;
    }
    snprintf(log_lines[log_count++], LINE_LEN, "%s", line);
}

static void log_text(const char *text) {
    char line[LINE_LEN];
    int pos = 0;

    for (const char *p = text; ; ++p) {
        if (*p == '\n' || *p == '\0' || pos >= LINE_LEN - 1) {
            line[pos] = '\0';
            log_raw(line);
            pos = 0;
            if (*p == '\0') {
                break;
            }
        } else {
            line[pos++] = *p;
        }
    }
}

static void log_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_text(buf);
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void clear_log(void) {
    log_count = 0;
}

static void draw_donut(void) {
    const char *donut[] = {
        "         .:^^^^^:::..         ",
        "     .:~~!7!~~!!^^~^::..      ",
        "   .^7??777!!!!!!~~~^^^^:.    ",
        "  ^?????7?!7?JJJJ77!!~!^^::.  ",
        " ~J????7777??J55P5Y7777!~~::. ",
        "^YYJJ777!~!7J?JYPGPYJ7!7~~^::.",
        "?5Y?""?""?!!!~77.   .~PGPY~!!!~^^:",
        "J5YY?""?!~~~7:      ?G5J7777!~^:",
        "7P5J?""?""?!!~~~.   .^Y5J?77!~~~^:",
        ":5PYYY?7!~~~~^^!7JJ?""?""?""?!7!~~^.",
        " ~PP5YY?7!777~~!77?777!77!!!: ",
        "  ~5GPYYJ?J???77?????7~7!!~.  ",
        "   :?PGP55Y??JJ?JJJJJJ??7^    ",
        "     :7Y5PG5JY55YJJJYJ7~.     ",
        "        :~7?YJYJJ?!!^.        "
    };

    for (size_t i = 0; i < sizeof(donut) / sizeof(donut[0]); ++i) {
        log_printf("\033[38;5;211m%s\033[0m", donut[i]);
    }
}

static void redraw_console(const char *prompt, const char *input, bool input_mode,
                           int kbd_row, int kbd_col) {
    video_clear(fb_bg);
    video_set_pos(0, 0);
    fb_fg = rgb_to_a8b8g8r8(255, 105, 210);
    video_printf("DonutOS Vita %s", DONUTOS_VERSION);
    fb_fg = 0xffffffff;
    video_printf("  cwd: %s\n", cwd);
    video_printf("START input  CIRCLE submit  SQUARE backspace  SELECT exit\n");
    video_printf("--------------------------------------------------------------------------------\n");

    int max_lines = input_mode ? 24 : 28;
    int start = log_count > max_lines ? log_count - max_lines : 0;
    for (int i = start; i < log_count; ++i) {
        video_printf("%s\n", log_lines[i]);
    }

    video_printf("\n%s%s\n", prompt ? prompt : "", input ? input : "");
    if (input_mode) {
        video_printf("\nD-pad move  CROSS char  TRIANGLE space  L/R cursor\n");
        for (int r = 0; r < 4; ++r) {
            const char *row = kbd_rows[r];
            for (int c = 0; row[c]; ++c) {
                if (r == kbd_row && c == kbd_col) {
                    uint32_t old_fg = fb_fg;
                    uint32_t old_bg = fb_bg;
                    fb_fg = 0xff000000;
                    fb_bg = 0xffffffff;
                    video_printf("%c", row[c]);
                    fb_fg = old_fg;
                    fb_bg = old_bg;
                    video_printf(" ");
                } else {
                    video_printf("%c ", row[c]);
                }
            }
            video_printf("\n");
        }
    }
    video_present();
}

static void insert_char(char *buf, int *len, int *cursor, char ch) {
    if (*len >= INPUT_LEN - 1) {
        return;
    }
    memmove(buf + *cursor + 1, buf + *cursor, (size_t)(*len - *cursor + 1));
    buf[*cursor] = ch;
    (*cursor)++;
    (*len)++;
}

static bool read_line(const char *prompt, char *out, size_t out_size) {
    char input[INPUT_LEN] = "";
    int len = 0;
    int cursor = 0;
    int row = 0;
    int col = 0;
    prev_buttons = 0;

    while (app_running) {
        redraw_console(prompt, input, true, row, col);
        unsigned int p = pressed_buttons();

        if (p & SCE_CTRL_SELECT) {
            app_running = false;
            return false;
        }
        if (p & SCE_CTRL_UP) {
            row = (row + 3) % 4;
            int row_len = (int)strlen(kbd_rows[row]);
            if (col >= row_len) col = row_len - 1;
        }
        if (p & SCE_CTRL_DOWN) {
            row = (row + 1) % 4;
            int row_len = (int)strlen(kbd_rows[row]);
            if (col >= row_len) col = row_len - 1;
        }
        if (p & SCE_CTRL_LEFT) {
            col = col > 0 ? col - 1 : (int)strlen(kbd_rows[row]) - 1;
        }
        if (p & SCE_CTRL_RIGHT) {
            int row_len = (int)strlen(kbd_rows[row]);
            col = (col + 1) % row_len;
        }
        if (p & SCE_CTRL_LTRIGGER) {
            if (cursor > 0) cursor--;
        }
        if (p & SCE_CTRL_RTRIGGER) {
            if (cursor < len) cursor++;
        }
        if (p & BTN_CONFIRM) {
            insert_char(input, &len, &cursor, kbd_rows[row][col]);
        }
        if (p & BTN_SPACE) {
            insert_char(input, &len, &cursor, ' ');
        }
        if (p & BTN_BACK) {
            if (cursor > 0) {
                memmove(input + cursor - 1, input + cursor, (size_t)(len - cursor + 1));
                cursor--;
                len--;
            }
        }
        if (p & BTN_SUBMIT) {
            snprintf(out, out_size, "%s", input);
            log_printf("DonutOS> %s", out);
            prev_buttons = 0;
            return true;
        }

        wait_vblank();
    }

    return false;
}

static int make_home(void) {
    mkdir("ux0:data", 0777);
    return mkdir(DONUTOS_HOME, 0777);
}

static bool is_absolute_vita_path(const char *path) {
    return strlen(path) >= 4 && isalpha((unsigned char)path[0]) &&
           path[1] == 'x' && path[2] == '0' && path[3] == ':';
}

static void normalize_path(char *path) {
    char temp[PATH_LEN];
    char *parts[64];
    int count = 0;
    char prefix[8] = "";
    char rest[PATH_LEN] = "";

    snprintf(temp, sizeof(temp), "%s", path);
    char *colon = strchr(temp, ':');
    if (colon) {
        size_t prefix_len = (size_t)(colon - temp + 1);
        if (prefix_len < sizeof(prefix)) {
            memcpy(prefix, temp, prefix_len);
            prefix[prefix_len] = '\0';
            snprintf(rest, sizeof(rest), "%s", colon + 1);
        }
    }
    if (prefix[0] == '\0') {
        return;
    }

    char *token = strtok(rest, "/");
    while (token && count < 64) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok(NULL, "/");
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (count > 0) count--;
        } else {
            parts[count++] = token;
        }
        token = strtok(NULL, "/");
    }

    snprintf(path, PATH_LEN, "%s", prefix);
    for (int i = 0; i < count; ++i) {
        strncat(path, "/", PATH_LEN - strlen(path) - 1);
        strncat(path, parts[i], PATH_LEN - strlen(path) - 1);
    }
}

static void resolve_path(const char *in, char *out, size_t out_size) {
    if (!in || in[0] == '\0') {
        copy_string(out, out_size, cwd);
    } else if (is_absolute_vita_path(in)) {
        copy_string(out, out_size, in);
    } else if (in[0] == '/') {
        snprintf(out, out_size, "%s%s", DONUTOS_HOME, in);
    } else {
        copy_string(out, out_size, cwd);
        strncat(out, "/", out_size - strlen(out) - 1);
        strncat(out, in, out_size - strlen(out) - 1);
    }
    normalize_path(out);
}

static void split_args(char *line, int *argc, char **argv, int max_args) {
    *argc = 0;
    char *token = strtok(line, " \t");
    while (token && *argc < max_args - 1) {
        argv[(*argc)++] = token;
        token = strtok(NULL, " \t");
    }
    argv[*argc] = NULL;
}

static void cmd_help(void) {
    log_text("\033[38;5;201mCommands:\033[0m");
    log_text("  hello        - Prints Hello World");
    log_text("  cat FILE     - Outputs a file");
    log_text("  bomb         - Displays an ANSI bomb");
    log_text("  donut        - Displays an ANSI donut");
    log_text("  dir [DIR]    - Lists files");
    log_text("  cut SRC DST  - Moves a file");
    log_text("  cd [DIR]     - Changes directory");
    log_text("  mkdir DIR    - Creates a directory");
    log_text("  mkdirc DIR   - Creates a directory and enters it");
    log_text("  wd, pwd      - Prints current directory");
    log_text("  del FILE     - Deletes a file");
    log_text("  deldir DIR   - Deletes a directory tree");
    log_text("  clear        - Clears the console");
    log_text("  about        - Shows information about DonutOS");
    log_text("  dofetch      - Displays Vita build information");
    log_text("  games        - Opens the game menu");
    log_text("  touch FILE   - Creates an empty file");
    log_text("  txted FILE   - Simple line-based text editor");
    log_text("  exit         - Closes DonutOS Vita");
}

static int remove_recursive(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        log_printf("deldir: cannot access '%s': %s", path, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            log_printf("deldir: cannot open '%s': %s", path, strerror(errno));
            return -1;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[PATH_LEN];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            if (remove_recursive(child) != 0) {
                closedir(dir);
                return -1;
            }
        }
        closedir(dir);
        if (rmdir(path) != 0) {
            log_printf("deldir: failed to remove '%s': %s", path, strerror(errno));
            return -1;
        }
    } else if (unlink(path) != 0) {
        log_printf("deldir: failed to remove '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void cmd_dir(const char *arg) {
    char path[PATH_LEN];
    resolve_path(arg ? arg : ".", path, sizeof(path));
    DIR *dir = opendir(path);
    if (!dir) {
        log_printf("dir: cannot open '%s': %s", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    bool any = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        log_text(entry->d_name);
        any = true;
    }
    if (!any) {
        log_text("(empty)");
    }
    closedir(dir);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        log_text("Usage: cat FILE...");
        return;
    }

    for (int i = 1; i < argc; ++i) {
        char path[PATH_LEN];
        resolve_path(argv[i], path, sizeof(path));
        FILE *fp = fopen(path, "r");
        if (!fp) {
            log_printf("cat: cannot open '%s': %s", argv[i], strerror(errno));
            continue;
        }
        char line[LINE_LEN];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = '\0';
            log_text(line);
        }
        fclose(fp);
    }
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        log_text("Usage: touch FILE...");
        return;
    }
    for (int i = 1; i < argc; ++i) {
        char path[PATH_LEN];
        resolve_path(argv[i], path, sizeof(path));
        FILE *fp = fopen(path, "ab");
        if (!fp) {
            log_printf("touch: cannot touch '%s': %s", argv[i], strerror(errno));
            continue;
        }
        fclose(fp);
    }
}

static void cmd_txted(const char *arg) {
    if (!arg) {
        log_text("Usage: txted FILE");
        return;
    }

    char path[PATH_LEN];
    resolve_path(arg, path, sizeof(path));
    FILE *fp = fopen(path, "w");
    if (!fp) {
        log_printf("txted: cannot open '%s': %s", arg, strerror(errno));
        return;
    }

    log_text("Enter text lines. Submit a single '.' line to save.");
    while (app_running) {
        char line[INPUT_LEN];
        if (!read_line("txted> ", line, sizeof(line))) {
            break;
        }
        if (strcmp(line, ".") == 0) {
            break;
        }
        fprintf(fp, "%s\n", line);
    }
    fclose(fp);
    log_printf("Saved %s", arg);
}

static void cmd_cut(int argc, char **argv) {
    if (argc != 3) {
        log_text("Usage: cut SOURCE DEST");
        return;
    }
    char src[PATH_LEN], dst[PATH_LEN];
    resolve_path(argv[1], src, sizeof(src));
    resolve_path(argv[2], dst, sizeof(dst));
    if (rename(src, dst) != 0) {
        log_printf("cut: failed to move '%s' to '%s': %s", argv[1], argv[2], strerror(errno));
    }
}

static void cmd_del(int argc, char **argv) {
    if (argc < 2) {
        log_text("Usage: del FILE...");
        return;
    }
    for (int i = 1; i < argc; ++i) {
        char path[PATH_LEN];
        resolve_path(argv[i], path, sizeof(path));
        if (unlink(path) != 0) {
            log_printf("del: cannot remove '%s': %s", argv[i], strerror(errno));
        }
    }
}

static void cmd_dofetch(void) {
    log_printf("OS: \033[38;5;201mDonutOS\033[0m %s (PS Vita homebrew)", DONUTOS_VERSION);
    log_text("Runtime: VitaSDK user app");
    log_text("Storage: ux0:data/DonutOS");
    log_text("Input: controller on-screen keyboard");
}

static void game_guess(void) {
    srand((unsigned int)time(NULL));
    bool playing = true;
    while (playing && app_running) {
        int secret = rand() % 100 + 1;
        int attempts = 0;
        log_text("I've picked a number between 1 and 100.");
        log_text("Use 0 to quit, -1 to restart.");
        while (app_running) {
            char line[INPUT_LEN];
            if (!read_line("guess> ", line, sizeof(line))) return;
            int guess = atoi(line);
            if (guess == 0) return;
            if (guess == -1) break;
            if (guess < 1 || guess > 100) {
                log_text("Number must be between 1 and 100.");
                continue;
            }
            attempts++;
            if (guess < secret) log_text("Too low!");
            else if (guess > secret) log_text("Too high!");
            else {
                log_printf("Correct! The number was %d.", secret);
                log_printf("Total attempts: %d", attempts);
                char again[INPUT_LEN];
                if (!read_line("Play again? y/n> ", again, sizeof(again))) return;
                playing = (tolower((unsigned char)again[0]) == 'y');
                break;
            }
        }
    }
}

#define TTT_SIZE 3

static void ttt_init(char b[TTT_SIZE][TTT_SIZE]) {
    for (int r = 0; r < TTT_SIZE; ++r)
        for (int c = 0; c < TTT_SIZE; ++c)
            b[r][c] = ' ';
}

static void ttt_print(char b[TTT_SIZE][TTT_SIZE]) {
    log_text("    1   2   3");
    for (int r = 0; r < TTT_SIZE; ++r) {
        log_printf(" %d  %c | %c | %c", r + 1, b[r][0], b[r][1], b[r][2]);
        if (r < TTT_SIZE - 1) log_text("   ---+---+---");
    }
}

static char ttt_winner(char b[TTT_SIZE][TTT_SIZE]) {
    for (int i = 0; i < TTT_SIZE; ++i) {
        if (b[i][0] != ' ' && b[i][0] == b[i][1] && b[i][1] == b[i][2]) return b[i][0];
        if (b[0][i] != ' ' && b[0][i] == b[1][i] && b[1][i] == b[2][i]) return b[0][i];
    }
    if (b[0][0] != ' ' && b[0][0] == b[1][1] && b[1][1] == b[2][2]) return b[0][0];
    if (b[0][2] != ' ' && b[0][2] == b[1][1] && b[1][1] == b[2][0]) return b[0][2];
    for (int r = 0; r < TTT_SIZE; ++r)
        for (int c = 0; c < TTT_SIZE; ++c)
            if (b[r][c] == ' ') return ' ';
    return 'D';
}

static int ttt_score(char winner, int depth, char ai, char human) {
    if (winner == ai) return 10 - depth;
    if (winner == human) return depth - 10;
    return 0;
}

static int ttt_minimax(char b[TTT_SIZE][TTT_SIZE], int depth, bool maxing, char ai, char human) {
    char winner = ttt_winner(b);
    if (winner != ' ') return ttt_score(winner, depth, ai, human);

    int best = maxing ? -10000 : 10000;
    for (int r = 0; r < TTT_SIZE; ++r) {
        for (int c = 0; c < TTT_SIZE; ++c) {
            if (b[r][c] != ' ') continue;
            b[r][c] = maxing ? ai : human;
            int score = ttt_minimax(b, depth + 1, !maxing, ai, human);
            b[r][c] = ' ';
            if (maxing && score > best) best = score;
            if (!maxing && score < best) best = score;
        }
    }
    return best;
}

static void ttt_ai_move(char b[TTT_SIZE][TTT_SIZE], char ai, char human) {
    int best = -10000;
    int br = -1, bc = -1;
    for (int r = 0; r < TTT_SIZE; ++r) {
        for (int c = 0; c < TTT_SIZE; ++c) {
            if (b[r][c] != ' ') continue;
            b[r][c] = ai;
            int score = ttt_minimax(b, 0, false, ai, human);
            b[r][c] = ' ';
            if (score > best) {
                best = score;
                br = r;
                bc = c;
            }
        }
    }
    if (br >= 0) {
        b[br][bc] = ai;
        log_printf("AI move (%c): %d %d", ai, br + 1, bc + 1);
    }
}

static void game_ttt(void) {
    char b[TTT_SIZE][TTT_SIZE];
    char choice[INPUT_LEN];
    log_text("Tic-Tac-Toe. Choose first player: 1 me, 2 computer, q quit.");
    if (!read_line("ttt> ", choice, sizeof(choice))) return;
    if (tolower((unsigned char)choice[0]) == 'q') return;

    char human = (choice[0] == '2') ? 'O' : 'X';
    char ai = (human == 'X') ? 'O' : 'X';
    char turn = 'X';
    ttt_init(b);

    while (app_running) {
        ttt_print(b);
        if (turn == human) {
            char line[INPUT_LEN];
            if (!read_line("move row col, r restart, q quit> ", line, sizeof(line))) return;
            if (tolower((unsigned char)line[0]) == 'q') return;
            if (tolower((unsigned char)line[0]) == 'r') {
                ttt_init(b);
                turn = 'X';
                continue;
            }
            int r, c;
            if (sscanf(line, "%d %d", &r, &c) != 2 || r < 1 || r > 3 || c < 1 || c > 3) {
                log_text("Invalid move. Example: 2 3");
                continue;
            }
            if (b[r - 1][c - 1] != ' ') {
                log_text("Cell is occupied.");
                continue;
            }
            b[r - 1][c - 1] = human;
        } else {
            ttt_ai_move(b, ai, human);
        }

        char winner = ttt_winner(b);
        if (winner != ' ') {
            ttt_print(b);
            if (winner == 'D') log_text("Draw!");
            else if (winner == human) log_text("You win!");
            else log_text("AI wins.");
            return;
        }
        turn = (turn == 'X') ? 'O' : 'X';
    }
}

static void cmd_games(void) {
    while (app_running) {
        log_text("Choose a game:");
        log_text("1: Guess the number");
        log_text("2: Tic-tac-toe");
        log_text("q: Back to shell");
        char line[INPUT_LEN];
        if (!read_line("game> ", line, sizeof(line))) return;
        if (line[0] == '1') game_guess();
        else if (line[0] == '2') game_ttt();
        else if (tolower((unsigned char)line[0]) == 'q') return;
        else log_text("Unknown choice.");
    }
}

static void run_command(char *line) {
    char *argv[32];
    int argc = 0;
    split_args(line, &argc, argv, 32);
    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "help") == 0) cmd_help();
    else if (strcmp(argv[0], "hello") == 0) log_text("Hello World");
    else if (strcmp(argv[0], "clear") == 0) clear_log();
    else if (strcmp(argv[0], "donut") == 0) draw_donut();
    else if (strcmp(argv[0], "about") == 0) {
        draw_donut();
        log_printf("DonutOS %s", DONUTOS_VERSION);
        log_printf("DonutOS was made by Alex (Discord: %s).", CREATOR_USERNAME);
        log_printf("The idea of making DonutOS came from %s.", IDEA_CREATOR_USERNAME);
    } else if (strcmp(argv[0], "wd") == 0 || strcmp(argv[0], "pwd") == 0) log_text(cwd);
    else if (strcmp(argv[0], "dir") == 0) cmd_dir(argc >= 2 ? argv[1] : ".");
    else if (strcmp(argv[0], "cat") == 0) cmd_cat(argc, argv);
    else if (strcmp(argv[0], "touch") == 0) cmd_touch(argc, argv);
    else if (strcmp(argv[0], "txted") == 0) cmd_txted(argc >= 2 ? argv[1] : NULL);
    else if (strcmp(argv[0], "cut") == 0) cmd_cut(argc, argv);
    else if (strcmp(argv[0], "del") == 0) cmd_del(argc, argv);
    else if (strcmp(argv[0], "deldir") == 0) {
        if (argc < 2) log_text("Usage: deldir DIRECTORY...");
        for (int i = 1; i < argc; ++i) {
            char path[PATH_LEN];
            resolve_path(argv[i], path, sizeof(path));
            remove_recursive(path);
        }
    } else if (strcmp(argv[0], "mkdir") == 0 || strcmp(argv[0], "mkdirc") == 0) {
        if (argc != 2) {
            log_printf("Usage: %s DIRECTORY", argv[0]);
            return;
        }
        char path[PATH_LEN];
        resolve_path(argv[1], path, sizeof(path));
        if (mkdir(path, 0777) != 0) {
            log_printf("mkdir: %s", strerror(errno));
        } else if (strcmp(argv[0], "mkdirc") == 0) {
            snprintf(cwd, sizeof(cwd), "%s", path);
        }
    } else if (strcmp(argv[0], "cd") == 0) {
        char path[PATH_LEN];
        resolve_path(argc >= 2 ? argv[1] : DONUTOS_HOME, path, sizeof(path));
        DIR *dir = opendir(path);
        if (!dir) {
            log_printf("cd: %s: %s", argc >= 2 ? argv[1] : DONUTOS_HOME, strerror(errno));
        } else {
            closedir(dir);
            snprintf(cwd, sizeof(cwd), "%s", path);
        }
    } else if (strcmp(argv[0], "dofetch") == 0) cmd_dofetch();
    else if (strcmp(argv[0], "games") == 0) cmd_games();
    else if (strcmp(argv[0], "bomb") == 0) {
        log_text("       ,--.!,");
        log_text("    __/   -*-");
        log_text("  ,d08b.  '|`");
        log_text("  0088MM");
        log_text("  `9MMP'");
    } else if (strcmp(argv[0], "poweroff") == 0 || strcmp(argv[0], "reboot") == 0 ||
               strcmp(argv[0], "exit") == 0) {
        app_running = false;
    } else {
        log_printf("DoSH: command not found: %s", argv[0]);
    }
}

int main(int argc, char *argv[]) {
    video_init();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    make_home();
    snprintf(cwd, sizeof(cwd), "%s", DONUTOS_HOME);

    log_printf("Welcome to DonutOS %s for PS Vita", DONUTOS_VERSION);
    draw_donut();
    log_text("Press START, type 'help', then CIRCLE.");

    while (app_running) {
        redraw_console("DonutOS> ", "", false, 0, 0);
        unsigned int p = pressed_buttons();
        if (p & SCE_CTRL_SELECT) {
            app_running = false;
        } else if (p & SCE_CTRL_START) {
            char line[INPUT_LEN];
            if (read_line("DonutOS> ", line, sizeof(line))) {
                run_command(line);
            }
        }
        wait_vblank();
    }

    video_printf("\nExiting DonutOS Vita...\n");
    video_present();
    sceKernelDelayThread(500000);
    if (fb_block >= 0) {
        sceKernelFreeMemBlock(fb_block);
    }
    sceKernelExitProcess(0);
    return 0;
}
