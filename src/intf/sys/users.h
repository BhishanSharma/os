// users.h - user accounts, login sessions and file permissions
#ifndef USERS_H
#define USERS_H

#include <stdint.h>

#define USER_NAME_MAX 9      /* 8 characters: home folders are FAT short names */

typedef struct {
    char name[USER_NAME_MAX];
    uint32_t uid;            /* 0 = root */
    char home[48];           /* "/" for root, "/HOME/ALICE" for the others */
} user_t;

/* Load the account database (PASSWD in the disk's root). With no database,
 * ask for a root password first (first start). Call once the disk is mounted. */
void users_init(void);

/* Ask for a user name and password until they match an account, then start
 * that user's session (current directory = home). */
void users_login(void);

/* `logout`/`exit`: end the current session. Returns 1 if a `su` session ended
 * and the previous user is current again, 0 if the login session ended. */
int users_logout(void);

const user_t *user_current(void);
int user_is_root(void);

/* 1 if the current user may create, change or delete `path` (relative to the
 * current directory): root anywhere, others only under their home and /TMP. */
int user_may_write(const char *path);

/* Shell commands: whoami, id, users, useradd, userdel, passwd, su.
 * Returns 1 if `line` was one of them. */
int users_command(const char *line);

#endif
