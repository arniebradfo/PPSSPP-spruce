#include "SDLOverlay.h"

#ifdef HAS_EMU_OVERLAY

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <GLES3/gl3.h>

#include "Core/Core.h"
#include "Core/Config.h"
#include "Core/SaveState.h"
#include "Core/System.h"

extern "C" {
#include "emu_overlay.h"
#include "emu_overlay_cfg.h"
#include "emu_overlay_sdl.h"
}

// ---------------------------------------------------------------------------
// Overlay state
// ---------------------------------------------------------------------------

static EmuOvl s_overlay;
static EmuOvlConfig s_overlayConfig;
static bool s_overlayInitialized = false;
static bool s_overlayConfigLoaded = false;
static bool s_overlayConfigFailed = false;
static char s_overlayJsonPath[512] = "";
static char s_overlayIniPath[512] = "";
static bool s_menuBtnPrev = false;
static SDL_Joystick *s_joy = nullptr;
static Uint8 s_prevHat = 0;
static Uint32 s_prevButtons = 0;
static SDL_Window *s_window = nullptr;
static SDL_GLContext s_glContext = nullptr;
static int s_screenW = 0;
static int s_screenH = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void overlay_init_paths()
{
	const char *json = getenv("EMU_OVERLAY_JSON");
	const char *ini  = getenv("EMU_OVERLAY_INI");
	if (json) strncpy(s_overlayJsonPath, json, sizeof(s_overlayJsonPath) - 1);
	if (ini)  strncpy(s_overlayIniPath,  ini,  sizeof(s_overlayIniPath) - 1);
}

static void overlay_swap_buffers()
{
	// PowerVR FBDEV compositor uses backbuffer alpha for transparency.
	// Force alpha channel to 1.0 (preserve RGB) so overlay is visible.
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

	SDL_GL_SwapWindow(s_window);
}

static void overlay_ensure_init()
{
	if (s_overlayInitialized || s_overlayConfigFailed)
		return;

	if (!s_overlayConfigLoaded) {
		overlay_init_paths();

		if (s_overlayJsonPath[0] == '\0')
			return;

		memset(&s_overlayConfig, 0, sizeof(s_overlayConfig));
		if (emu_ovl_cfg_load(&s_overlayConfig, s_overlayJsonPath) != 0) {
			fprintf(stderr, "[Overlay] Failed to load config: %s\n", s_overlayJsonPath);
			s_overlayConfigFailed = true;
			return;
		}

		if (s_overlayIniPath[0] != '\0') {
			emu_ovl_cfg_read_ini(&s_overlayConfig, s_overlayIniPath);
		}

		s_overlayConfigLoaded = true;
	}

	EmuOvlRenderBackend *render = overlay_sdl_get_backend();
	const char *gameName = getenv("EMU_OVERLAY_GAME");

	if (render->init(s_screenW, s_screenH) != 0) {
		fprintf(stderr, "[Overlay] Failed to init render backend\n");
		return;
	}

	emu_ovl_init(&s_overlay, &s_overlayConfig, render,
	             gameName ? gameName : "PSP", s_screenW, s_screenH);

	s_overlayInitialized = true;
}

static EmuOvlInput poll_overlay_input()
{
	EmuOvlInput input;
	memset(&input, 0, sizeof(input));

	SDL_JoystickUpdate();
	if (!s_joy) return input;

	// D-pad (hat) — edge detect: only trigger on newly-pressed directions
	Uint8 hat = SDL_JoystickGetHat(s_joy, 0);
	Uint8 hatPressed = hat & ~s_prevHat;
	s_prevHat = hat;

	if (hatPressed & SDL_HAT_UP)    input.up    = true;
	if (hatPressed & SDL_HAT_DOWN)  input.down  = true;
	if (hatPressed & SDL_HAT_LEFT)  input.left  = true;
	if (hatPressed & SDL_HAT_RIGHT) input.right = true;

	// Buttons — edge detect: only trigger on newly-pressed buttons
	// SDL button indices: 0=A(hw), 1=B(hw), 2=X(hw), 3=Y(hw), 4=L1, 5=R1, 8=Menu
	static const int btnMap[] = {0, 1, 4, 5, 8};
	Uint32 curButtons = 0;
	for (int i = 0; i < 5; i++) {
		if (SDL_JoystickGetButton(s_joy, btnMap[i]))
			curButtons |= (1u << btnMap[i]);
	}
	Uint32 btnPressed = curButtons & ~s_prevButtons;
	s_prevButtons = curButtons;

	if (btnPressed & (1u << 0)) input.b    = true;
	if (btnPressed & (1u << 1)) input.a    = true;
	if (btnPressed & (1u << 4)) input.l1   = true;
	if (btnPressed & (1u << 5)) input.r1   = true;
	if (btnPressed & (1u << 8)) input.menu = true;

	return input;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Overlay_Init(SDL_Window *window, int screen_w, int screen_h)
{
	s_window = window;
	s_glContext = SDL_GL_GetCurrentContext();
	s_screenW = screen_w;
	s_screenH = screen_h;
	fprintf(stderr, "[Overlay] Init: %dx%d, glCtx=%p\n", screen_w, screen_h, (void*)s_glContext);
}

bool Overlay_CheckMenuButton()
{
	SDL_JoystickUpdate();

	if (!s_joy) {
		if (SDL_NumJoysticks() > 0)
			s_joy = SDL_JoystickOpen(0);
		if (!s_joy)
			return false;
	}

	bool pressed = SDL_JoystickGetButton(s_joy, 8) != 0;
	bool justPressed = pressed && !s_menuBtnPrev;
	s_menuBtnPrev = pressed;
	return justPressed;
}

bool Overlay_RunMenu(SDL_Window *window)
{
	// Only open overlay when in-game
	if (GetUIState() != UISTATE_INGAME)
		return false;

	// Ensure GL context is current on this thread before any GL calls
	if (s_glContext)
		SDL_GL_MakeCurrent(s_window, s_glContext);

	overlay_ensure_init();
	if (!s_overlayInitialized) {
		fprintf(stderr, "[Overlay] RunMenu: init failed, returning\n");
		return false;
	}

	fprintf(stderr, "[Overlay] RunMenu: pausing emulation\n");
	// Pause emulation
	Core_Break(BreakReason::UIPause);

	// Wait for emu thread to actually pause (up to 500ms)
	for (int i = 0; i < 50 && !Core_IsStepping(); i++) {
		SDL_Delay(10);
	}

	// Pause audio
	SDL_PauseAudio(1);

	fprintf(stderr, "[Overlay] RunMenu: stepping=%d, opening overlay\n", Core_IsStepping());
	// Open overlay (captures current frame)
	emu_ovl_open(&s_overlay);

	// Reset input edge detection state
	s_prevHat = s_joy ? SDL_JoystickGetHat(s_joy, 0) : 0;
	s_prevButtons = 0;
	if (s_joy) {
		static const int btnMap[] = {0, 1, 4, 5, 8};
		for (int i = 0; i < 5; i++) {
			if (SDL_JoystickGetButton(s_joy, btnMap[i]))
				s_prevButtons |= (1u << btnMap[i]);
		}
	}
	// Drain pending SDL events
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {}
	s_menuBtnPrev = true; // prevent re-trigger

	fprintf(stderr, "[Overlay] RunMenu: entering menu loop\n");
	// Overlay menu loop
	while (emu_ovl_is_active(&s_overlay)) {
		EmuOvlInput input = poll_overlay_input();
		emu_ovl_update(&s_overlay, &input);
		emu_ovl_render(&s_overlay);
		overlay_swap_buffers();
		SDL_Delay(16);
	}

	// Drain any SDL events generated during the overlay loop
	// (especially button presses used to close the overlay — if PPSSPP
	// processes them, it may trigger its own pause/menu screen)
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {}
	}

	// Resume audio
	SDL_PauseAudio(0);

	EmuOvlAction action = emu_ovl_get_action(&s_overlay);

	// Handle config changes
	if (emu_ovl_cfg_has_changes(&s_overlayConfig)) {
		if (s_overlayIniPath[0] != '\0') {
			emu_ovl_cfg_write_ini(&s_overlayConfig, s_overlayIniPath);
		}
		emu_ovl_cfg_apply_staged(&s_overlayConfig);
		// Reload PPSSPP config to pick up changes
		g_Config.Reload();
	}

	if (action == EMU_OVL_ACTION_QUIT) {
		Core_Stop();
		return true;
	}

	if (action == EMU_OVL_ACTION_SAVE_STATE) {
		int slot = emu_ovl_get_action_param(&s_overlay);
		std::string gamePrefix = SaveState::GetGamePrefix(g_paramSFO);
		SaveState::SaveSlot(gamePrefix, slot, nullptr);
		emu_ovl_save_slot_screenshot(&s_overlay, slot);
	}

	if (action == EMU_OVL_ACTION_LOAD_STATE) {
		int slot = emu_ovl_get_action_param(&s_overlay);
		std::string gamePrefix = SaveState::GetGamePrefix(g_paramSFO);
		SaveState::LoadSlot(gamePrefix, slot, nullptr);
	}

	// Render the captured game frame to cover any transition flash
	// before PPSSPP's ThreadFrame() takes over rendering
	EmuOvlRenderBackend *render = overlay_sdl_get_backend();
	if (render && render->begin_frame && render->draw_captured_frame && render->end_frame) {
		render->begin_frame();
		render->draw_captured_frame(0.0f);
		render->end_frame();
		overlay_swap_buffers();
	}

	// Resume emulation
	fprintf(stderr, "[Overlay] RunMenu: resuming emulation\n");
	Core_Resume();

	return false;
}

void Overlay_Shutdown()
{
	if (s_joy) {
		SDL_JoystickClose(s_joy);
		s_joy = nullptr;
	}
	s_overlayInitialized = false;
}

#else // !HAS_EMU_OVERLAY — stub implementations

void Overlay_Init(SDL_Window *, int, int) {}
bool Overlay_CheckMenuButton() { return false; }
bool Overlay_RunMenu(SDL_Window *) { return false; }
void Overlay_Shutdown() {}

#endif // HAS_EMU_OVERLAY
