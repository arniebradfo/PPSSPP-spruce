#!/usr/bin/env python3
"""Build in the NextUI/MinUI-style emulator overlay (overlay/, from nx-redux).

Pressing the menu button in-game opens the overlay instead of PPSSPP's pause
screen: Continue, Save State, Load State, Options, an optional entry that
hands over to PPSSPP's own menu, and Quit. Adapted from nx-redux v1.1.1's
workspace/tg5040/other/ppsspp/ppsspp.patch for PPSSPP v1.20.3, and only
active when configured with -DEMU_OVERLAY=ON -DEMU_OVERLAY_DIR=<overlay dir>.

Run from the PPSSPP source root after copying overlay/SDLOverlay.{cpp,h} into
SDL/.

Files modified:
  CMakeLists.txt      option, sources, SDL2_ttf/SDL2_image
  SDL/SDLMain.cpp     init, open the overlay from the main-thread render loop
  SDL/SDLJoystick.cpp keep the menu button from PPSSPP while in-game
"""
import sys


def edit(path, pairs):
    with open(path) as f:
        content = f.read()
    for old, new, what in pairs:
        if content.count(old) != 1:
            print(f"ERROR: Could not find {what} in {path}")
            sys.exit(1)
        content = content.replace(old, new, 1)
    with open(path, 'w') as f:
        f.write(content)
    print(f"Patched {path}: {len(pairs)} modifications")


edit('CMakeLists.txt', [
    ('option(USE_CCACHE "Use ccache if detected" ON)',
     'option(EMU_OVERLAY "Build with the NextUI/MinUI emulator overlay menu" OFF)\n'
     'set(EMU_OVERLAY_DIR "" CACHE PATH "Path to the emulator overlay sources")\n'
     'option(USE_CCACHE "Use ccache if detected" ON)',
     'USE_CCACHE option'),
    ('\t\tSDL/SDLMain.cpp\n'
     '\t\tSDL/SDLGLGraphicsContext.cpp\n'
     '\t)\n',
     '\t\tSDL/SDLMain.cpp\n'
     '\t\tSDL/SDLGLGraphicsContext.cpp\n'
     '\t\tSDL/SDLOverlay.h\n'
     '\t\tSDL/SDLOverlay.cpp\n'
     '\t)\n'
     '\tif(EMU_OVERLAY AND EMU_OVERLAY_DIR)\n'
     '\t\tadd_compile_definitions(HAS_EMU_OVERLAY)\n'
     '\t\tlist(APPEND nativeExtra\n'
     '\t\t\t${EMU_OVERLAY_DIR}/emu_overlay.c\n'
     '\t\t\t${EMU_OVERLAY_DIR}/emu_overlay_cfg.c\n'
     '\t\t\t${EMU_OVERLAY_DIR}/emu_overlay_sdl.c\n'
     '\t\t\t${EMU_OVERLAY_DIR}/cjson/cJSON.c\n'
     '\t\t)\n'
     '\t\tset_source_files_properties(\n'
     '\t\t\t${EMU_OVERLAY_DIR}/emu_overlay.c\n'
     '\t\t\t${EMU_OVERLAY_DIR}/emu_overlay_cfg.c\n'
     '\t\t\t${EMU_OVERLAY_DIR}/emu_overlay_sdl.c\n'
     '\t\t\t${EMU_OVERLAY_DIR}/cjson/cJSON.c\n'
     '\t\t\tPROPERTIES COMPILE_FLAGS "-std=gnu99 -I${EMU_OVERLAY_DIR}"\n'
     '\t\t)\n'
     '\t\tset_source_files_properties(SDL/SDLOverlay.cpp\n'
     '\t\t\tPROPERTIES COMPILE_FLAGS "-I${EMU_OVERLAY_DIR}"\n'
     '\t\t)\n'
     '\t\tlist(APPEND nativeExtraLibs SDL2_ttf SDL2_image)\n'
     '\tendif()\n',
     'SDL source list'),
])

edit('SDL/SDLMain.cpp', [
    ('#include "SDL/SDLJoystick.h"\n',
     '#include "SDL/SDLJoystick.h"\n'
     '#include "SDL/SDLOverlay.h"\n',
     'SDLJoystick include'),
    ('\tEnableFZ();\n'
     '\n'
     '\tEmuThreadStart(graphicsContext);\n',
     '\tEnableFZ();\n'
     '\n'
     '\tOverlay_Init(window, w, h);\n'
     '\n'
     '\tEmuThreadStart(graphicsContext);\n',
     'EmuThreadStart after EnableFZ'),
    ('\t} else while (true) {\n'
     '\t\t{\n'
     '\t\t\tSDL_Event event;\n'
     '\t\t\twhile (SDL_PollEvent(&event)) {\n'
     '\t\t\t\tProcessSDLEvent(window, event, &inputTracker);\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tif (g_QuitRequested || g_RestartRequested)\n'
     '\t\t\tbreak;\n',
     '\t} else while (true) {\n'
     '\t\t{\n'
     '\t\t\tSDL_Event event;\n'
     '\t\t\twhile (SDL_PollEvent(&event)) {\n'
     '\t\t\t\t// The overlay reads its menu button itself while in-game\n'
     '\t\t\t\tif ((event.type == SDL_JOYBUTTONDOWN || event.type == SDL_JOYBUTTONUP) &&\n'
     '\t\t\t\t\tevent.jbutton.button == Overlay_MenuButton() && Overlay_OwnsMenuButton())\n'
     '\t\t\t\t\tcontinue;\n'
     '\t\t\t\tProcessSDLEvent(window, event, &inputTracker);\n'
     '\t\t\t}\n'
     '\t\t}\n'
     '\t\tif (g_QuitRequested || g_RestartRequested)\n'
     '\t\t\tbreak;\n'
     '\n'
     '\t\t// Overlay: the menu button opens it in-game (render thread: GL is current)\n'
     '\t\tif (Overlay_CheckMenuButton()) {\n'
     '\t\t\tif (Overlay_RunMenu(window)) {\n'
     '\t\t\t\tg_QuitRequested = true;\n'
     '\t\t\t\tbreak;\n'
     '\t\t\t}\n'
     '\t\t}\n',
     'main-thread render loop'),
    ('\tEmuThreadJoin();\n'
     '\n'
     '\tdelete joystick;\n',
     '\tEmuThreadJoin();\n'
     '\n'
     '\tOverlay_Shutdown();\n'
     '\n'
     '\tdelete joystick;\n',
     'final EmuThreadJoin'),
])

edit('SDL/SDLJoystick.cpp', [
    ('void SDLJoystick::ProcessInput(const SDL_Event &event){\n'
     '\tswitch (event.type) {\n'
     '\tcase SDL_CONTROLLERBUTTONDOWN:\n',
     '// Whether a controller button event is the overlay\'s menu button while the\n'
     '// overlay owns it (in-game). The raw button number comes from $EMU_PAD, so\n'
     '// look it up through the controller mapping.\n'
     'static bool isOverlayMenuButton(const SDL_ControllerButtonEvent &cbutton) {\n'
     '\tint menu = Overlay_MenuButton();\n'
     '\tif (menu < 0 || !Overlay_OwnsMenuButton())\n'
     '\t\treturn false;\n'
     '\tSDL_GameController *gc = SDL_GameControllerFromInstanceID(cbutton.which);\n'
     '\tif (!gc)\n'
     '\t\treturn false;\n'
     '\tSDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(\n'
     '\t\tgc, (SDL_GameControllerButton)cbutton.button);\n'
     '\treturn bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && bind.value.button == menu;\n'
     '}\n'
     '\n'
     'void SDLJoystick::ProcessInput(const SDL_Event &event){\n'
     '\tswitch (event.type) {\n'
     '\tcase SDL_CONTROLLERBUTTONDOWN:\n'
     '\t\tif (isOverlayMenuButton(event.cbutton))\n'
     '\t\t\tbreak;\n',
     'ProcessInput button down'),
    ('\tcase SDL_CONTROLLERBUTTONUP:\n'
     '\t\tif (event.cbutton.state == SDL_RELEASED) {\n',
     '\tcase SDL_CONTROLLERBUTTONUP:\n'
     '\t\tif (isOverlayMenuButton(event.cbutton))\n'
     '\t\t\tbreak;\n'
     '\t\tif (event.cbutton.state == SDL_RELEASED) {\n',
     'ProcessInput button up'),
    ('#include "SDL/SDLJoystick.h"\n',
     '#include "SDL/SDLJoystick.h"\n'
     '#include "SDL/SDLOverlay.h"\n',
     'SDLJoystick.h include'),
])
