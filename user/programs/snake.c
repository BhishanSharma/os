// snake - arrow keys (or WASD) to steer, Q to quit
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

#define MAX_LEN 2048
#define BLOCK   ((char)0xDB)   /* CP437 full block */
#define DOT     ((char)0x04)   /* CP437 diamond */

static int cols, rows;          // play field inside the border
static int xs[MAX_LEN], ys[MAX_LEN], len;
static int food_x, food_y;

static void put_at(int x, int y, char c, int color) {
    gotoxy(x, y);
    set_color(color, COLOR_BLACK);
    putchar(c);
}

static void draw_border(void) {
    set_color(COLOR_DARK_GRAY, COLOR_BLACK);
    for (int x = 0; x <= cols + 1; x++) {
        gotoxy(x, 1); putchar(BLOCK);
        gotoxy(x, rows + 2); putchar(BLOCK);
    }
    for (int y = 1; y <= rows + 2; y++) {
        gotoxy(0, y); putchar(BLOCK);
        gotoxy(cols + 1, y); putchar(BLOCK);
    }
}

static int on_snake(int x, int y) {
    for (int i = 0; i < len; i++)
        if (xs[i] == x && ys[i] == y) return 1;
    return 0;
}

static void place_food(void) {
    do {
        food_x = rand() % cols + 1;
        food_y = rand() % rows + 2;
    } while (on_snake(food_x, food_y));
    put_at(food_x, food_y, DOT, COLOR_LIGHT_RED);
}

static void show_score(int score, int best) {
    gotoxy(0, 0);
    set_color(COLOR_YELLOW, COLOR_BLACK);
    printf(" SNAKE   score %-5d best %-5d   arrows/WASD steer, P pause, Q quit ", score, best);
}

int main(void) {
    int screen_cols, screen_rows;
    console_size(&screen_cols, &screen_rows);
    cols = screen_cols - 3;
    rows = screen_rows - 4;
    if (cols > 60) cols = 60;
    if (rows > 24) rows = 24;
    srand((unsigned)uptime_ms());
    int best = 0;

    for (;;) {
        clear_screen();
        show_cursor(0);
        draw_border();
        len = 4;
        for (int i = 0; i < len; i++) {
            xs[i] = cols / 2 - i;
            ys[i] = rows / 2 + 2;
            put_at(xs[i], ys[i], BLOCK, i ? COLOR_GREEN : COLOR_LIGHT_GREEN);
        }
        int dx = 1, dy = 0, score = 0, paused = 0;
        place_food();
        show_score(score, best);

        for (;;) {
            int key, quit = 0;
            while ((key = getkey()) != 0) {
                if ((key == KEYCODE_UP || key == 'w') && dy == 0)         { dx = 0; dy = -1; }
                else if ((key == KEYCODE_DOWN || key == 's') && dy == 0)  { dx = 0; dy = 1; }
                else if ((key == KEYCODE_LEFT || key == 'a') && dx == 0)  { dx = -1; dy = 0; }
                else if ((key == KEYCODE_RIGHT || key == 'd') && dx == 0) { dx = 1; dy = 0; }
                else if (key == 'p') paused = !paused;
                else if (key == 'q') quit = 1;
            }
            if (quit) goto done;
            if (paused) { sleep_ms(50); continue; }

            int nx = xs[0] + dx, ny = ys[0] + dy;
            if (nx < 1 || nx > cols || ny < 2 || ny > rows + 1 || on_snake(nx, ny)) break;

            int ate = nx == food_x && ny == food_y;
            if (!ate) put_at(xs[len - 1], ys[len - 1], ' ', COLOR_BLACK);
            else if (len < MAX_LEN) len++;
            for (int i = len - 1; i > 0; i--) { xs[i] = xs[i - 1]; ys[i] = ys[i - 1]; }
            xs[0] = nx;
            ys[0] = ny;
            put_at(xs[1], ys[1], BLOCK, COLOR_GREEN);
            put_at(nx, ny, BLOCK, COLOR_LIGHT_GREEN);
            if (ate) {
                score += 10;
                if (score > best) best = score;
                show_score(score, best);
                place_food();
            }
            int delay = 110 - len * 2;
            sleep_ms(delay < 45 ? 45 : delay);
        }

        gotoxy(cols / 2 - 12, rows / 2 + 2);
        set_color(COLOR_WHITE, COLOR_RED);
        printf("  GAME OVER - score %d  ", score);
        gotoxy(cols / 2 - 12, rows / 2 + 3);
        printf("  R: play again  Q: quit ");
        for (;;) {
            int key = getkey();
            if (key == 'r') break;
            if (key == 'q') goto done;
            sleep_ms(30);
        }
    }

done:
    reset_color();
    clear_screen();
    show_cursor(1);
    printf("Thanks for playing! Best score: %d\n", best);
    return 0;
}
