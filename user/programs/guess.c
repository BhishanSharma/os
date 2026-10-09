// guess - guess the number between 1 and 100
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

int main(void) {
    srand((unsigned)uptime_ms());
    int secret = rand() % 100 + 1;
    char line[32];

    printf("I'm thinking of a number between 1 and 100.\n");
    for (int tries = 1; ; tries++) {
        printf("Your guess: ");
        readline(line, sizeof(line));
        if (!line[0]) { tries--; continue; }
        int guess = atoi(line);
        if (guess < secret) {
            printf("Higher!\n");
        } else if (guess > secret) {
            printf("Lower!\n");
        } else {
            set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            printf("Correct! You got it in %d %s.\n", tries, tries == 1 ? "try" : "tries");
            reset_color();
            return 0;
        }
    }
}
