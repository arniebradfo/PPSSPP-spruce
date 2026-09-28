#include "SDLOverlay.h"

#ifdef HAS_EMU_OVERLAY

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#include <GLES3/gl3.h>

#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/System/NativeApp.h"
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

// Raw joystick button numbers the overlay reads. The defaults are TrimUI's
// (menu 8, d-pad on hat 0); $EMU_PAD overrides any of them in the N64 pak's
// format, e.g. for the MagicX Mini Zero 28:
//   EMU_PAD="a=0,b=1,l1=4,r1=5,menu=19,up=13,down=16,left=14,right=15"
// A d-pad entry of -1 means hat 0. Unknown keys are ignored.
struct OverlayPad {
	int a = 1, b = 0, l1 = 4, r1 = 5, menu = 8;
	int up = -1, down = -1, left = -1, right = -1;
};
static OverlayPad s_pad;
static bool s_openHostMenu = false;
// The overlay only takes the menu button when a pak configures it
// ($EMU_OVERLAY_JSON); otherwise PPSSPP behaves as if it weren't built in.
static bool s_enabled = false;

static void overlay_parse_pad()
{
	const char *env = getenv("EMU_PAD");
	if (!env || !env[0])
		return;
	std::string spec(env);
	size_t pos = 0;
	while (pos < spec.size()) {
		size_t end = spec.find(',', pos);
		if (end == std::string::npos)
			end = spec.size();
		std::string item = spec.substr(pos, end - pos);
		size_t eq = item.find('=');
		if (eq != std::string::npos) {
			std::string key = item.substr(0, eq);
			int value = atoi(item.c_str() + eq + 1);
			if (key == "a") s_pad.a = value;
			else if (key == "b") s_pad.b = value;
			else if (key == "l1") s_pad.l1 = value;
			else if (key == "r1") s_pad.r1 = value;
			else if (key == "menu") s_pad.menu = value;
			else if (key == "up") s_pad.up = value;
			else if (key == "down") s_pad.down = value;
			else if (key == "left") s_pad.left = value;
			else if (key == "right") s_pad.right = value;
		}
		pos = end + 1;
	}
	fprintf(stderr, "[Overlay] pad: a=%d b=%d l1=%d r1=%d menu=%d up=%d down=%d left=%d right=%d\n",
	        s_pad.a, s_pad.b, s_pad.l1, s_pad.r1, s_pad.menu, s_pad.up, s_pad.down, s_pad.left, s_pad.right);
}

static bool pad_button(int button)
{
	return button >= 0 && s_joy && SDL_JoystickGetButton(s_joy, button) != 0;
}

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

	// D-pad: buttons when $EMU_PAD names them, else hat 0. Edge detect: only
	// newly-pressed directions and buttons count.
	Uint8 hat = SDL_JoystickNumHats(s_joy) > 0 ? SDL_JoystickGetHat(s_joy, 0) : 0;
	if (s_pad.up >= 0 && pad_button(s_pad.up)) hat |= SDL_HAT_UP;
	if (s_pad.down >= 0 && pad_button(s_pad.down)) hat |= SDL_HAT_DOWN;
	if (s_pad.left >= 0 && pad_button(s_pad.left)) hat |= SDL_HAT_LEFT;
	if (s_pad.right >= 0 && pad_button(s_pad.right)) hat |= SDL_HAT_RIGHT;
	Uint8 hatPressed = hat & ~s_prevHat;
	s_prevHat = hat;

	if (hatPressed & SDL_HAT_UP)    input.up    = true;
	if (hatPressed & SDL_HAT_DOWN)  input.down  = true;
	if (hatPressed & SDL_HAT_LEFT)  input.left  = true;
	if (hatPressed & SDL_HAT_RIGHT) input.right = true;

	Uint32 curButtons = 0;
	if (pad_button(s_pad.a))    curButtons |= 1u << 0;
	if (pad_button(s_pad.b))    curButtons |= 1u << 1;
	if (pad_button(s_pad.l1))   curButtons |= 1u << 2;
	if (pad_button(s_pad.r1))   curButtons |= 1u << 3;
	if (pad_button(s_pad.menu)) curButtons |= 1u << 4;
	Uint32 btnPressed = curButtons & ~s_prevButtons;
	s_prevButtons = curButtons;

	if (btnPressed & (1u << 0)) input.a    = true;
	if (btnPressed & (1u << 1)) input.b    = true;
	if (btnPressed & (1u << 2)) input.l1   = true;
	if (btnPressed & (1u << 3)) input.r1   = true;
	if (btnPressed & (1u << 4)) input.menu = true;

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
	const char *json = getenv("EMU_OVERLAY_JSON");
	s_enabled = json && json[0];
	overlay_parse_pad();
	fprintf(stderr, "[Overlay] Init: %dx%d, glCtx=%p\n", screen_w, screen_h, (void*)s_glContext);
}

bool Overlay_CheckMenuButton()
{
	if (!s_enabled)
		return false;
	SDL_JoystickUpdate();

	if (!s_joy) {
		if (SDL_NumJoysticks() > 0)
			s_joy = SDL_JoystickOpen(0);
		if (!s_joy)
			return false;
	}

	bool pressed = pad_button(s_pad.menu);
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

	// Reset input edge detection state: whatever is held now (the menu press
	// itself) must be released before it counts
	s_prevHat = 0xFF;
	s_prevButtons = 0xFFFFFFFF;
	poll_overlay_input();
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

	if (action == EMU_OVL_ACTION_HOST_MENU) {
		// Open PPSSPP's own pause menu, as its back key does in-game. Sent
		// after Core_Resume below so EmuScreen handles it.
		s_openHostMenu = true;
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

	if (s_openHostMenu) {
		s_openHostMenu = false;
		KeyInput key{};
		key.deviceId = DEVICE_ID_PAD_0;
		key.keyCode = NKCODE_BACK;
		key.flags = KeyInputFlags::DOWN;
		NativeKey(key);
		key.flags = KeyInputFlags::UP;
		NativeKey(key);
	}

	return false;
}

int Overlay_MenuButton()
{
	return s_pad.menu;
}

bool Overlay_OwnsMenuButton()
{
	// Only in-game: in PPSSPP's own menus the button keeps its usual role
	// (back), so the "PPSSPP Menu" entry can be left again with it.
	return s_enabled && GetUIState() == UISTATE_INGAME;
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
int Overlay_MenuButton() { return -1; }
bool Overlay_OwnsMenuButton() { return false; }

#endif // HAS_EMU_OVERLAY
