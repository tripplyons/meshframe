#include <SDL3/SDL.h>

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    SDL_Window* window = SDL_CreateWindow("meshframe", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) running = false;
        SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
