#include <SDL.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: dbtest <dbfile>\n"); return 2; }
    // emulate the launcher: newline-joined content via env
    char buf[8192] = {0};
    FILE *f = fopen(argv[1], "rb");
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    setenv("SDL_GAMECONTROLLERCONFIG", buf, 1);
    SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    SDL_JoystickGUID g = SDL_JoystickGetGUIDFromString("0300f186ff1100000839000001010000");
    char *m = SDL_GameControllerMappingForGUID(g);
    printf("pad mapping: %s\n", m ? m : "(default/none)");
    SDL_JoystickGUID g2 = SDL_JoystickGetGUIDFromString("1900e97c010000000100000000010000");
    char *m2 = SDL_GameControllerMappingForGUID(g2);
    printf("keypad mapping: %s\n", m2 ? m2 : "(default/none)");
    return 0;
}
