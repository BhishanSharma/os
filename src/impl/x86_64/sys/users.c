// users.c - user accounts, login sessions and file permissions
//
// Accounts live in PASSWD in the root of the mounted disk, one per line:
//
//     name:uid:salt:hash:home
//
// salt is 16 random bytes and hash is SHA-256 applied HASH_ROUNDS times to
// the salt and password (both hex). uid 0 is root. FAT32 has no file owners,
// so permissions go by location: root may change anything, everyone else only
// what is under their home folder (/HOME/<NAME>) and /TMP. The check sits in
// the FAT32 driver (fat32_set_write_guard), so it covers shell commands, the
// editor, downloads, scripts and user programs alike.
#include "sys/users.h"
#include "sys/sysinfo.h"
#include "sys/task.h"
#include "drivers/fat32.h"
#include "drivers/disk.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/rtc.h"
#include "drivers/heap.h"
#include "lib/print.h"
#include "lib/string.h"
#include "bearssl_hash.h"

#define DB_FILE      "PASSWD"
#define MAX_USERS    16
#define MAX_SESSIONS 8
#define HASH_ROUNDS  2000
#define FIRST_UID    1000

typedef struct {
    user_t user;
    uint8_t salt[16];
    uint8_t hash[32];
} account_t;

static account_t accounts[MAX_USERS];
static int account_count;

/* Login session, then one entry per nested `su`. */
static user_t sessions[MAX_SESSIONS];
static int session_depth;

/* While > 0 the write guard lets everything through (saving PASSWD, making
 * home folders): like a setuid program acting on the user's behalf. */
static int privileged;

/* ---- Helpers ------------------------------------------------------------ */

static void to_upper(char *s) {
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s -= 32;
}

static void hex_encode(char *out, const uint8_t *data, int len) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 15];
    }
    out[2 * len] = 0;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(uint8_t *out, const char *hex, int len) {
    for (int i = 0; i < len; i++) {
        int hi = hex_value(hex[2 * i]), lo = hex_value(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return hex[2 * len] == 0 ? 0 : -1;
}

static void hash_password(const char *password, const uint8_t salt[16], uint8_t out[32]) {
    br_sha256_context ctx;
    size_t len = strlen(password);
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, salt, 16);
    br_sha256_update(&ctx, password, len);
    br_sha256_out(&ctx, out);
    for (int i = 1; i < HASH_ROUNDS; i++) {
        br_sha256_init(&ctx);
        br_sha256_update(&ctx, out, 32);
        br_sha256_update(&ctx, salt, 16);
        br_sha256_update(&ctx, password, len);
        br_sha256_out(&ctx, out);
    }
}

/* Salt: hash of whatever varies (TSC, timer, clock, RDRAND if the CPU has it). */
static void random_salt(uint8_t salt[16]) {
    static uint64_t counter;
    struct {
        uint64_t tsc, rdrand, counter;
        uint32_t tick;
        rtc_time_t time;
    } seed;
    memset(&seed, 0, sizeof(seed));
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    seed.tsc = ((uint64_t)hi << 32) | lo;
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    if (ecx & (1u << 30)) {
        unsigned char ok = 0;
        for (int tries = 0; tries < 10 && !ok; tries++)
            __asm__ volatile("rdrand %0; setc %1" : "=r"(seed.rdrand), "=qm"(ok));
    }
    seed.counter = ++counter;
    seed.tick = get_tick();
    rtc_read(&seed.time);

    uint8_t digest[32];
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, &seed, sizeof(seed));
    br_sha256_out(&ctx, digest);
    memcpy(salt, digest, 16);
}

/* Compare without stopping at the first difference (no timing hint). */
static int same_hash(const uint8_t *a, const uint8_t *b) {
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

static account_t *find_account(const char *name) {
    for (int i = 0; i < account_count; i++)
        if (strcmp(accounts[i].user.name, name) == 0) return &accounts[i];
    return 0;
}

static int check_password(const account_t *a, const char *password) {
    uint8_t hash[32];
    hash_password(password, a->salt, hash);
    return same_hash(hash, a->hash);
}

static void set_password(account_t *a, const char *password) {
    random_salt(a->salt);
    hash_password(password, a->salt, a->hash);
}

/* Keyboard line without the shell's history; `echo` 0 for passwords.
 * Returns the length, or -1 on Ctrl+C. */
static int read_input(char *buf, int size, int echo) {
    int len = 0;
    while (1) {
        int c = get_char();
        if (!c) {
            keyboard_idle();
            continue;
        }
        if (c == KEY_CTRL_C) {
            keyboard_ctrl_c = 0;
            print_str("^C\n");
            buf[0] = 0;
            return -1;
        }
        if (c == '\n' || c == '\r') {
            print_str("\n");
            buf[len] = 0;
            return len;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                if (echo) print_str("\b \b");
            }
            continue;
        }
        if (c >= 32 && c < 127 && len < size - 1) {
            buf[len++] = (char)c;
            if (echo) print_char((char)c);
        }
    }
}

/* Ask for a new password twice. Returns 0 when `out` holds it. */
static int ask_new_password(char *out, int size) {
    char again[64];
    for (int tries = 0; tries < 3; tries++) {
        print_str("New password: ");
        if (read_input(out, size, 0) < 0) return -1;
        if (!out[0]) {
            print_str("The password must not be empty.\n");
            continue;
        }
        print_str("Retype new password: ");
        if (read_input(again, sizeof(again), 0) < 0) return -1;
        if (strcmp(out, again) == 0) return 0;
        print_str("The passwords do not match.\n");
    }
    return -1;
}

/* User names: 1-8 lower-case letters and digits, starting with a letter. */
static int valid_name(const char *name) {
    int n = 0;
    for (; name[n]; n++) {
        char c = name[n];
        if (!((c >= 'a' && c <= 'z') || (n > 0 && c >= '0' && c <= '9'))) return 0;
    }
    return n >= 1 && n < USER_NAME_MAX;
}

/* ---- The database file -------------------------------------------------- */

/* Run file operations from the root directory, then go back. */
static void enter_root(char *saved, uint32_t size) {
    saved[0] = 0;
    fat32_get_current_directory(saved, size);
    fat32_change_directory("/");
}

static void leave_root(const char *saved) {
    if (saved[0]) fat32_change_directory(saved);
}

static int parse_line(char *line, account_t *a) {
    char *field[5];
    int n = 0;
    field[n++] = line;
    for (char *p = line; *p && n < 5; p++)
        if (*p == ':') {
            *p = 0;
            field[n++] = p + 1;
        }
    if (n != 5 || !valid_name(field[0]) || strlen(field[4]) >= sizeof(a->user.home)) return -1;
    memset(a, 0, sizeof(*a));
    k_snprintf(a->user.name, sizeof(a->user.name), "%s", field[0]);
    a->user.uid = 0;
    for (char *p = field[1]; *p >= '0' && *p <= '9'; p++) a->user.uid = a->user.uid * 10 + (uint32_t)(*p - '0');
    if (hex_decode(a->salt, field[2], 16) != 0 || hex_decode(a->hash, field[3], 32) != 0) return -1;
    k_snprintf(a->user.home, sizeof(a->user.home), "%s", field[4]);
    return 0;
}

static void load_accounts(void) {
    account_count = 0;
    char saved[256];
    enter_root(saved, sizeof(saved));
    uint32_t size = fat32_file_exists(DB_FILE) ? fat32_get_file_size(DB_FILE) : 0;
    if (size && size != 0xFFFFFFFF && size < 64 * 1024) {
        char *data = kmalloc(size + 1);
        if (data && fat32_read_file(DB_FILE, (uint8_t *)data, size) >= 0) {
            data[size] = 0;
            char *line = data;
            while (*line && account_count < MAX_USERS) {
                char *end = line;
                while (*end && *end != '\n') end++;
                char next = *end;
                *end = 0;
                if (end > line && end[-1] == '\r') end[-1] = 0;
                if (line[0] && line[0] != '#' && parse_line(line, &accounts[account_count]) == 0)
                    account_count++;
                if (!next) break;
                line = end + 1;
            }
        }
        kfree(data);
    }
    leave_root(saved);
}

static int save_accounts(void) {
    char *data = kmalloc(MAX_USERS * 192 + 128);
    if (!data) return -1;
    int len = k_snprintf(data, 128, "# Terminal OS accounts: name:uid:salt:sha256 x%d:home\n", HASH_ROUNDS);
    for (int i = 0; i < account_count; i++) {
        char salt[33], hash[65];
        hex_encode(salt, accounts[i].salt, 16);
        hex_encode(hash, accounts[i].hash, 32);
        len += k_snprintf(data + len, 192, "%s:%u:%s:%s:%s\n", accounts[i].user.name, accounts[i].user.uid,
                          salt, hash, accounts[i].user.home);
    }
    char saved[256];
    privileged++;
    enter_root(saved, sizeof(saved));
    fat32_create_file(DB_FILE);
    int result = fat32_write_file(DB_FILE, (const uint8_t *)data, (uint32_t)len) < 0 ? -1 : 0;
    leave_root(saved);
    privileged--;
    kfree(data);
    return result;
}

/* Create `/name` and then `/name/sub` (either may be 0) if missing. */
static void make_dirs(const char *top, const char *sub) {
    char saved[256];
    privileged++;
    enter_root(saved, sizeof(saved));
    if (top && !fat32_file_exists(top)) fat32_mkdir(top);
    if (top && sub && fat32_change_directory(top) == 0 && !fat32_file_exists(sub)) fat32_mkdir(sub);
    fat32_change_directory("/");
    leave_root(saved);
    privileged--;
}

static account_t *add_account(const char *name, uint32_t uid, const char *password) {
    if (account_count >= MAX_USERS) return 0;
    account_t *a = &accounts[account_count++];
    memset(a, 0, sizeof(*a));
    k_snprintf(a->user.name, sizeof(a->user.name), "%s", name);
    a->user.uid = uid;
    if (uid == 0) {
        k_snprintf(a->user.home, sizeof(a->user.home), "/");
    } else {
        char upper[USER_NAME_MAX];
        k_snprintf(upper, sizeof(upper), "%s", name);
        to_upper(upper);
        k_snprintf(a->user.home, sizeof(a->user.home), "/HOME/%s", upper);
        make_dirs("HOME", upper);
    }
    set_password(a, password);
    return a;
}

/* ---- Permission check --------------------------------------------------- */

/* Absolute, upper-case path with "." and ".." resolved: "/HOME/ALICE/A.TXT". */
static void normalize_path(const char *path, char *out, size_t size) {
    char joined[512];
    if (path[0] == '/') {
        k_snprintf(joined, sizeof(joined), "%s", path);
    } else {
        char cwd[256] = "/";
        fat32_get_current_directory(cwd, sizeof(cwd));
        k_snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    }
    size_t len = 0;
    out[0] = 0;
    for (char *p = joined; *p; ) {
        while (*p == '/') p++;
        char *start = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - start);
        if (n == 0 || (n == 1 && start[0] == '.')) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
            out[len] = 0;
            continue;
        }
        if (len + n + 2 >= size) break;
        out[len++] = '/';
        for (size_t i = 0; i < n; i++) {
            char c = start[i];
            out[len++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        out[len] = 0;
    }
    if (len == 0) k_snprintf(out, size, "/");
}

static int path_inside(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && path[n] == '/';
}

int user_may_write(const char *path) {
    if (privileged || session_depth == 0) return 1;     // the kernel itself, or before login
    const user_t *u = user_current();
    if (u->uid == 0) return 1;
    char abs[256];
    normalize_path(path, abs, sizeof(abs));
    return path_inside(abs, u->home) || path_inside(abs, "/TMP");
}

static int write_guard(const char *path) {
    return user_may_write(path);
}

/* Like /etc/shadow: only root reads the password hashes (and the saved
 * Wi-Fi key). */
static int read_guard(const char *path) {
    if (privileged || session_depth == 0 || user_is_root()) return 1;
    char abs[256];
    normalize_path(path, abs, sizeof(abs));
    return strcmp(abs, "/" DB_FILE) != 0 && strcmp(abs, "/WIFI.CFG") != 0;
}

int users_read_line(char *buf, int size, int echo) {
    return read_input(buf, size, echo);
}

/* ---- Sessions ----------------------------------------------------------- */

const user_t *user_current(void) {
    static const user_t nobody = { "root", 0, "/" };
    task_t *t = task_current();
    if (t && t->is_user) return &t->user;      // a program runs as whoever started it
    return session_depth ? &sessions[session_depth - 1] : &nobody;
}

int user_is_root(void) {
    return user_current()->uid == 0;
}

static void push_session(const account_t *a) {
    if (session_depth < MAX_SESSIONS) sessions[session_depth++] = a->user;
}

int users_logout(void) {
    if (session_depth > 1) {
        session_depth--;
        return 1;
    }
    session_depth = 0;
    return 0;
}

static void first_start(void) {
    char password[64], name[USER_NAME_MAX + 8];
    print_str("\n");
    print_accent("  First start: ");
    print_str("there are no user accounts on this disk yet.\n");
    print_str("  Choose a password for root, the administrator account.\n\n");
    while (ask_new_password(password, sizeof(password)) != 0)
        print_str("Let's try that again.\n");
    add_account("root", 0, password);

    print_str("\n  Now create your own account for everyday use (empty name to skip).\n");
    while (1) {
        print_str("User name: ");
        if (read_input(name, sizeof(name), 1) <= 0) break;
        if (!valid_name(name) || strcmp(name, "root") == 0) {
            print_str("Use 1-8 lower-case letters or digits, starting with a letter.\n");
            continue;
        }
        if (ask_new_password(password, sizeof(password)) == 0) {
            add_account(name, FIRST_UID, password);
            break;
        }
    }
    make_dirs("TMP", 0);
    memset(password, 0, sizeof(password));

    if (save_accounts() == 0) {
        print_str("\n  Accounts saved to /PASSWD.");
        if (disk_selected() == DISK_RAM) print_str(" (This is the RAM disk: they last until reboot.)");
        print_str("\n");
    } else {
        print_str("\n  Could not save the accounts (no writable disk): they last until reboot.\n");
    }
}

void users_init(void) {
    fat32_set_write_guard(write_guard);
    fat32_set_read_guard(read_guard);
    session_depth = 0;
    load_accounts();
    if (!find_account("root")) first_start();
}

void users_login(void) {
    char name[32], password[64];
    session_depth = 0;
    print_str("\n  ");
    print_accent(OS_NAME " " OS_VERSION);
    print_str(" (" OS_HOSTNAME ")\n\n");
    while (1) {
        print_str(OS_HOSTNAME " login: ");
        if (read_input(name, sizeof(name), 1) <= 0) continue;
        print_str("Password: ");
        if (read_input(password, sizeof(password), 0) < 0) continue;
        account_t *a = find_account(name);
        int ok = a && check_password(a, password);
        memset(password, 0, sizeof(password));
        if (ok) {
            push_session(a);
            break;
        }
        sleep(1000);   // slow down guessing
        print_str("Login incorrect\n\n");
    }
    const user_t *u = user_current();
    if (fat32_change_directory(u->home) != 0) fat32_change_directory("/");
    kprintf("\nWelcome, %s. ", u->name);
    if (u->uid == 0)
        print_str("You are the administrator: be careful.\n");
    else
        kprintf("Your files go in %s.\n", u->home);
    print_str("Type ");
    print_accent("help");
    print_str(" for commands, ");
    print_accent("programs");
    print_str(" for programs, ");
    print_accent("sysinfo");
    print_str(" for this machine.\n\n");
}

/* ---- Commands ----------------------------------------------------------- */

static int require_root(const char *what) {
    if (user_is_root()) return 1;
    char msg[96];
    k_snprintf(msg, sizeof(msg), "%s: only root can do that (try `su`)", what);
    print_error(msg);
    return 0;
}

static void cmd_users(void) {
    kprintf("  %-10s %6s  %s\n", "NAME", "UID", "HOME");
    for (int i = 0; i < account_count; i++) {
        const user_t *u = &accounts[i].user;
        kprintf("  %-10s %6u  %s%s\n", u->name, u->uid, u->home,
                strcmp(u->name, user_current()->name) == 0 ? "   <- you" : "");
    }
}

static void cmd_useradd(const char *name) {
    if (!require_root("useradd")) return;
    if (!valid_name(name)) {
        print_str("Usage: useradd <name>  (1-8 lower-case letters or digits, starting with a letter)\n");
        return;
    }
    if (find_account(name)) {
        kprintf("useradd: %s already exists\n", name);
        return;
    }
    if (account_count >= MAX_USERS) {
        print_str("useradd: too many accounts\n");
        return;
    }
    uint32_t uid = FIRST_UID;
    for (int i = 0; i < account_count; i++)
        if (accounts[i].user.uid >= uid) uid = accounts[i].user.uid + 1;
    char password[64];
    if (ask_new_password(password, sizeof(password)) != 0) {
        print_str("useradd: cancelled\n");
        return;
    }
    account_t *a = add_account(name, uid, password);
    memset(password, 0, sizeof(password));
    if (save_accounts() != 0) print_str("useradd: warning: could not save /PASSWD\n");
    kprintf("Created user %s (uid %u), home %s\n", a->user.name, a->user.uid, a->user.home);
}

static void cmd_userdel(const char *name) {
    if (!require_root("userdel")) return;
    account_t *a = find_account(name);
    if (!a) {
        kprintf("userdel: no user %s\n", name[0] ? name : "(none given)");
        return;
    }
    if (a->user.uid == 0) {
        print_str("userdel: root cannot be deleted\n");
        return;
    }
    for (int i = 0; i < session_depth; i++)
        if (strcmp(sessions[i].name, name) == 0) {
            kprintf("userdel: %s is logged in\n", name);
            return;
        }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_at(i);
        if (t->is_user && t->state != TASK_FREE && t->state != TASK_ZOMBIE && strcmp(t->user.name, name) == 0) {
            kprintf("userdel: %s has programs running (pid %d)\n", name, t->pid);
            return;
        }
    }
    char home[48];
    k_snprintf(home, sizeof(home), "%s", a->user.home);
    *a = accounts[--account_count];
    if (save_accounts() != 0) print_str("userdel: warning: could not save /PASSWD\n");
    kprintf("Deleted user %s (the files in %s are kept)\n", name, home);
}

static void cmd_passwd(const char *name) {
    const user_t *me = user_current();
    if (!name[0]) name = me->name;
    if (strcmp(name, me->name) != 0 && !require_root("passwd")) return;
    account_t *a = find_account(name);
    if (!a) {
        kprintf("passwd: no user %s\n", name);
        return;
    }
    char password[64];
    if (me->uid != 0) {
        print_str("Current password: ");
        if (read_input(password, sizeof(password), 0) < 0) return;
        int ok = check_password(a, password);
        memset(password, 0, sizeof(password));
        if (!ok) {
            sleep(1000);
            print_str("passwd: wrong password\n");
            return;
        }
    }
    if (ask_new_password(password, sizeof(password)) != 0) {
        print_str("passwd: password unchanged\n");
        return;
    }
    set_password(a, password);
    memset(password, 0, sizeof(password));
    if (save_accounts() == 0)
        kprintf("passwd: password for %s updated\n", name);
    else
        print_str("passwd: could not save /PASSWD\n");
}

static void cmd_su(const char *name) {
    if (!name[0]) name = "root";
    account_t *a = find_account(name);
    if (!a) {
        kprintf("su: no user %s\n", name);
        return;
    }
    if (session_depth >= MAX_SESSIONS) {
        print_str("su: too many nested sessions (type exit)\n");
        return;
    }
    if (!user_is_root()) {
        char password[64];
        print_str("Password: ");
        if (read_input(password, sizeof(password), 0) < 0) return;
        int ok = check_password(a, password);
        memset(password, 0, sizeof(password));
        if (!ok) {
            sleep(1000);
            print_str("su: authentication failure\n");
            return;
        }
    }
    push_session(a);
    kprintf("Now %s. Type exit to go back.\n", name);
}

int users_authenticate(const char *name, user_t *out) {
    account_t *a = find_account(name);
    if (!a) return USERS_NO_SUCH_USER;
    const user_t *me = user_current();
    if (me->uid != 0 && strcmp(me->name, a->user.name) != 0) {
        char password[64];
        print_str("Password: ");
        if (read_input(password, sizeof(password), 0) < 0) return USERS_AUTH_FAILED;
        int ok = check_password(a, password);
        memset(password, 0, sizeof(password));
        if (!ok) {
            sleep(1000);
            return USERS_AUTH_FAILED;
        }
    }
    *out = a->user;
    return 0;
}

static const char *argument(const char *line, const char *cmd) {
    size_t n = strlen(cmd);
    if (strncmp(line, cmd, n) != 0 || (line[n] != 0 && line[n] != ' ')) return 0;
    line += n;
    while (*line == ' ') line++;
    return line;
}

int users_command(const char *line) {
    const char *arg;
    if (strcmp(line, "whoami") == 0) {
        kprintf("%s\n", user_current()->name);
    } else if (strcmp(line, "id") == 0) {
        const user_t *u = user_current();
        kprintf("uid=%u(%s) home=%s%s\n", u->uid, u->name, u->home, u->uid == 0 ? " (administrator)" : "");
    } else if (strcmp(line, "users") == 0) {
        cmd_users();
    } else if ((arg = argument(line, "useradd"))) {
        cmd_useradd(arg);
    } else if ((arg = argument(line, "userdel"))) {
        cmd_userdel(arg);
    } else if ((arg = argument(line, "passwd"))) {
        cmd_passwd(arg);
    } else if ((arg = argument(line, "su"))) {
        cmd_su(arg);
    } else {
        return 0;
    }
    return 1;
}
