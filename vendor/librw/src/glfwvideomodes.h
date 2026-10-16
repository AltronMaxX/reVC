// Render resolutions are independent of monitor modes. In particular, Wayland
// normally advertises only the compositor's current output mode.
#pragma once

namespace rw {
static const int glfwRenderResolutions[][2] = {
	{640, 480}, {720, 400}, {800, 600}, {960, 540}, {1024, 576},
	{1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800},
	{1280, 960}, {1280, 1024}, {1360, 768}, {1366, 768}, {1440, 900},
	{1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200},
	{2048, 1152}, {2560, 1080}, {2560, 1440}, {2560, 1600},
	{3440, 1440}, {3840, 1600}, {3840, 2160}
};
}
