#!/usr/bin/env python3
"""Map backbuffer viewport/scissor rects correctly for every display rotation.

Applied after patches/a30/display-rotation.py, which converts a backbuffer rect
for a 90/270 rotation by swapping x/y and w/h. That matches ComputeOrthoMatrix's
ortho * rot_matrix only for ROTATE_90 (DISPLAY_ROTATION=270, the Miyoo A30).
For ROTATE_270 (DISPLAY_ROTATION=90, the MagicX Mini Zero 28) the swap puts a
rect mirrored through the screen centre, so full-screen and centred rects (the
game image) look right but UI scissors and sub-viewports land elsewhere and
clip the menus. ROTATE_180 fell through to the unrotated y-flip.

With the logical (top-left origin) backbuffer lw x lh, a logical rect
(x, y, w, h) lands in GL window coordinates at:
  ROTATE_0    (x,           lh - y - h,  w, h)
  ROTATE_90   (y,           x,           h, w)
  ROTATE_180  (lw - x - w,  y,           w, h)
  ROTATE_270  (lh - y - h,  lw - x - w,  h, w)

Files modified:
  Common/GPU/OpenGL/GLQueueRunner.cpp
"""
import sys

TARGET = 'Common/GPU/OpenGL/GLQueueRunner.cpp'


def replace_once(content, old, new, what):
    if content.count(old) != 1:
        print(f"ERROR: Could not find {what} in {TARGET} (is display-rotation.py applied?)")
        sys.exit(1)
    return content.replace(old, new, 1)


def patch(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    content = replace_once(
        content,
        'static constexpr int TEXCACHE_NAME_CACHE_SIZE = 16;',
        'static constexpr int TEXCACHE_NAME_CACHE_SIZE = 16;\n'
        '\n'
        '// Maps a rect in the logical (top-left origin) backbuffer, lw x lh, to GL\n'
        '// window coordinates on a rotated display. Must agree with\n'
        '// ComputeOrthoMatrix\'s ortho * rot_matrix.\n'
        'template <class T>\n'
        'static void RotateBackbufferRect(T &x, T &y, T &w, T &h, T lw, T lh) {\n'
        '\tconst T ox = x, oy = y, ow = w, oh = h;\n'
        '\tswitch (g_display.rotation) {\n'
        '\tcase DisplayRotation::ROTATE_90:\n'
        '\t\tx = oy; y = ox; w = oh; h = ow;\n'
        '\t\tbreak;\n'
        '\tcase DisplayRotation::ROTATE_180:\n'
        '\t\tx = lw - ox - ow; y = oy;\n'
        '\t\tbreak;\n'
        '\tcase DisplayRotation::ROTATE_270:\n'
        '\t\tx = lh - oy - oh; y = lw - ox - ow; w = oh; h = ow;\n'
        '\t\tbreak;\n'
        '\tdefault:\n'
        '\t\ty = lh - oy - oh;\n'
        '\t\tbreak;\n'
        '\t}\n'
        '}',
        'TEXCACHE_NAME_CACHE_SIZE declaration')

    content = replace_once(
        content,
        '\t\t\tif (!curFB_) {\n'
        '\t\t\t\tif (g_display.rotation == DisplayRotation::ROTATE_90 || g_display.rotation == DisplayRotation::ROTATE_270) {\n'
        '\t\t\t\t\tstd::swap(vp_x, vp_y);\n'
        '\t\t\t\t\tstd::swap(vp_w, vp_h);\n'
        '\t\t\t\t} else {\n'
        '\t\t\t\t\tvp_y = curFBHeight_ - vp_y - vp_h;\n'
        '\t\t\t\t}\n'
        '\t\t\t}',
        '\t\t\tif (!curFB_)\n'
        '\t\t\t\tRotateBackbufferRect<float>(vp_x, vp_y, vp_w, vp_h, (float)curFBWidth_, (float)curFBHeight_);',
        'rotated VIEWPORT block')

    content = replace_once(
        content,
        '\t\t\tif (!curFB_) {\n'
        '\t\t\t\tif (g_display.rotation == DisplayRotation::ROTATE_90 || g_display.rotation == DisplayRotation::ROTATE_270) {\n'
        '\t\t\t\t\tstd::swap(sc_x, sc_y);\n'
        '\t\t\t\t\tstd::swap(sc_w, sc_h);\n'
        '\t\t\t\t} else {\n'
        '\t\t\t\t\tsc_y = curFBHeight_ - sc_y - sc_h;\n'
        '\t\t\t\t}\n'
        '\t\t\t}',
        '\t\t\tif (!curFB_)\n'
        '\t\t\t\tRotateBackbufferRect<int>(sc_x, sc_y, sc_w, sc_h, curFBWidth_, curFBHeight_);',
        'rotated SCISSOR block')

    with open(filepath, 'w') as f:
        f.write(content)
    print(f"Patched {filepath}: 3 modifications")


if __name__ == '__main__':
    patch(TARGET)
