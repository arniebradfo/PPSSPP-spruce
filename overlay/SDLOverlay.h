#pragma once

#include <SDL2/SDL.h>

// Initialize overlay paths from environment variables.
// Must be called after SDL and PPSSPP initialization.
void Overlay_Init(SDL_Window *window, int screen_w, int screen_h);

// Check if menu button was just pressed (edge detect).
// Call once per frame from the main thread.
bool Overlay_CheckMenuButton();

// Run the blocking overlay menu loop.
// Pauses emulation, runs overlay, resumes on exit.
// Returns true if the user chose to quit the emulator.
bool Overlay_RunMenu(SDL_Window *window);

// Raw joystick button the overlay opens on ($EMU_PAD menu=, default 8), or -1
// when the overlay isn't built in.
int Overlay_MenuButton();

// Whether menu button events belong to the overlay right now (in-game) rather
// than to PPSSPP.
bool Overlay_OwnsMenuButton();

// Cleanup overlay resources.
void Overlay_Shutdown();
