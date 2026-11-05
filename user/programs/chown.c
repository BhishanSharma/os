// chown <user>[:<group>] <file> - give a file or folder to another user (root only)
//
// Users and groups by name or number; everyone's group has their own name.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <args.h>

/* "alice" or "1000" -> the uid; -1 if there is no such user. */
static int lookup(const char *name) {
    if (*name >= '0' && *name <= '9') {
        int n = 0;
        for (; *name >= '0' && *name <= '9'; name++) n = n * 10 + (*name - '0');
        return *name ? -1 : n;
    }
    if (!strcmp(name, "root")) return 0;
    struct os_account a[16];
    int n = (int)os_syscall(SYS_SYSTEM, SYSTEM_USERS, (long)a, 16);
    for (int i = 0; i < n; i++)
        if (!strcmp(a[i].name, name)) return (int)a[i].uid;
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: chown <user>[:<group>] <file>\n");
        return 2;
    }
    char who[40], name[256];
    snprintf(who, sizeof(who), "%s", argv[1]);
    args_join(name, sizeof(name), argc, argv, 2);
    char *colon = strchr(who, ':');
    if (colon) *colon++ = 0;
    int uid = who[0] ? lookup(who) : -1, gid = colon ? lookup(colon) : -1;
    if ((who[0] && uid < 0) || (colon && *colon && gid < 0)) {
        printf("chown: no such user: %s\n", who[0] && uid < 0 ? who : colon);
        return 1;
    }
    int r = chown(name, uid, gid);
    if (r < 0) {
        printf("chown: %s: %s%s\n", name, os_strerror(r), r == SYSERR_PERM ? " (only root gives files away)" : "");
        return 1;
    }
    return 0;
}
