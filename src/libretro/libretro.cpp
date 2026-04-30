#include "libretro.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "cpputil.h"
#include "towns.h"
#include "townsthread.h"

#if defined(_WIN32)
#define TSUGARU_RETRO_API extern "C" __declspec(dllexport)
#else
#define TSUGARU_RETRO_API extern "C" __attribute__((visibility("default")))
#endif

#ifndef RETRO_DEVICE_POINTER
#define RETRO_DEVICE_POINTER 6
#endif
#ifndef RETRO_DEVICE_ID_MOUSE_X
#define RETRO_DEVICE_ID_MOUSE_X 0
#endif
#ifndef RETRO_DEVICE_ID_MOUSE_Y
#define RETRO_DEVICE_ID_MOUSE_Y 1
#endif
#ifndef RETRO_DEVICE_ID_POINTER_X
#define RETRO_DEVICE_ID_POINTER_X 0
#endif
#ifndef RETRO_DEVICE_ID_POINTER_Y
#define RETRO_DEVICE_ID_POINTER_Y 1
#endif
#ifndef RETRO_DEVICE_ID_POINTER_PRESSED
#define RETRO_DEVICE_ID_POINTER_PRESSED 2
#endif
#ifndef RETRO_DEVICE_ID_MOUSE_LEFT
#define RETRO_DEVICE_ID_MOUSE_LEFT 2
#endif
#ifndef RETRO_DEVICE_ID_MOUSE_RIGHT
#define RETRO_DEVICE_ID_MOUSE_RIGHT 3
#endif

namespace
{
constexpr unsigned BASE_WIDTH = 640;
constexpr unsigned BASE_HEIGHT = 480;
constexpr unsigned MAX_WIDTH = 1024;
constexpr unsigned MAX_HEIGHT = 1024;
constexpr double FPS = 60.0;
constexpr double SAMPLE_RATE = 44100.0;
constexpr size_t AUDIO_FRAMES_PER_RUN = static_cast<size_t>(SAMPLE_RATE / FPS);
constexpr uintmax_t SMALL_BIN_FLOPPY_MAX = 1500u * 1024u;

retro_environment_t environ_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t audio_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t input_poll_cb = nullptr;
retro_input_state_t input_state_cb = nullptr;
retro_log_printf_t log_cb = nullptr;

std::array<uint32_t, BASE_WIDTH * BASE_HEIGHT> framebuffer{};
std::array<std::vector<uint32_t>, 3> video_buffers;
size_t video_buffer_index = 0;
std::string content_path;
std::string system_directory;
std::string save_directory;
uint64_t frame_counter = 0;
std::mutex savestate_lock;
std::vector<uint8_t> savestate_snapshot;
size_t savestate_snapshot_size = 0;
uint64_t savestate_snapshot_frame = ~0ULL;
bool savestate_snapshot_valid = false;
bool mouse_mode_integrated = false;
std::array<int, 2> mouse_pointer_prev_x{};
std::array<int, 2> mouse_pointer_prev_y{};
std::array<bool, 2> mouse_pointer_prev_valid{};

unsigned int port0_type = TOWNS_GAMEPORTEMU_PHYSICAL0;
unsigned int port1_type = TOWNS_GAMEPORTEMU_MOUSE;

// Keyboard mapping: RetroArch key code to FM Towns JIS key code
// Based on standalone keytrans.cpp mapping
unsigned char RetroKeyToTownsKey(unsigned keycode)
{
	switch (keycode)
	{
		case RETROK_BACKSPACE: return TOWNS_JISKEY_BACKSPACE;
		case RETROK_TAB: return TOWNS_JISKEY_TAB;
		case RETROK_RETURN: return TOWNS_JISKEY_RETURN;
		case RETROK_ESCAPE: return TOWNS_JISKEY_ESC;
		case RETROK_SPACE: return TOWNS_JISKEY_SPACE;
		
		case RETROK_QUOTE: return TOWNS_JISKEY_7;
		case RETROK_COMMA: return TOWNS_JISKEY_COMMA;
		case RETROK_MINUS: return TOWNS_JISKEY_MINUS;
		case RETROK_PERIOD: return TOWNS_JISKEY_DOT;
		case RETROK_SLASH: return TOWNS_JISKEY_SLASH;
		
		case RETROK_0: return TOWNS_JISKEY_0;
		case RETROK_1: return TOWNS_JISKEY_1;
		case RETROK_2: return TOWNS_JISKEY_2;
		case RETROK_3: return TOWNS_JISKEY_3;
		case RETROK_4: return TOWNS_JISKEY_4;
		case RETROK_5: return TOWNS_JISKEY_5;
		case RETROK_6: return TOWNS_JISKEY_6;
		case RETROK_7: return TOWNS_JISKEY_7;
		case RETROK_8: return TOWNS_JISKEY_8;
		case RETROK_9: return TOWNS_JISKEY_9;
		
		case RETROK_SEMICOLON: return TOWNS_JISKEY_SEMICOLON;
		case RETROK_EQUALS: return TOWNS_JISKEY_MINUS;
		
		case RETROK_LEFTBRACKET: return TOWNS_JISKEY_LEFT_SQ_BRACKET;
		case RETROK_BACKSLASH: return TOWNS_JISKEY_BACKSLASH;
		case RETROK_RIGHTBRACKET: return TOWNS_JISKEY_RIGHT_SQ_BRACKET;
		case RETROK_BACKQUOTE: return TOWNS_JISKEY_AT;
		
		case RETROK_a: return TOWNS_JISKEY_A;
		case RETROK_b: return TOWNS_JISKEY_B;
		case RETROK_c: return TOWNS_JISKEY_C;
		case RETROK_d: return TOWNS_JISKEY_D;
		case RETROK_e: return TOWNS_JISKEY_E;
		case RETROK_f: return TOWNS_JISKEY_F;
		case RETROK_g: return TOWNS_JISKEY_G;
		case RETROK_h: return TOWNS_JISKEY_H;
		case RETROK_i: return TOWNS_JISKEY_I;
		case RETROK_j: return TOWNS_JISKEY_J;
		case RETROK_k: return TOWNS_JISKEY_K;
		case RETROK_l: return TOWNS_JISKEY_L;
		case RETROK_m: return TOWNS_JISKEY_M;
		case RETROK_n: return TOWNS_JISKEY_N;
		case RETROK_o: return TOWNS_JISKEY_O;
		case RETROK_p: return TOWNS_JISKEY_P;
		case RETROK_q: return TOWNS_JISKEY_Q;
		case RETROK_r: return TOWNS_JISKEY_R;
		case RETROK_s: return TOWNS_JISKEY_S;
		case RETROK_t: return TOWNS_JISKEY_T;
		case RETROK_u: return TOWNS_JISKEY_U;
		case RETROK_v: return TOWNS_JISKEY_V;
		case RETROK_w: return TOWNS_JISKEY_W;
		case RETROK_x: return TOWNS_JISKEY_X;
		case RETROK_y: return TOWNS_JISKEY_Y;
		case RETROK_z: return TOWNS_JISKEY_Z;
		
		case RETROK_DELETE: return TOWNS_JISKEY_DELETE;
		
		case RETROK_KP0: return TOWNS_JISKEY_NUM_0;
		case RETROK_KP1: return TOWNS_JISKEY_NUM_1;
		case RETROK_KP2: return TOWNS_JISKEY_NUM_2;
		case RETROK_KP3: return TOWNS_JISKEY_NUM_3;
		case RETROK_KP4: return TOWNS_JISKEY_NUM_4;
		case RETROK_KP5: return TOWNS_JISKEY_NUM_5;
		case RETROK_KP6: return TOWNS_JISKEY_NUM_6;
		case RETROK_KP7: return TOWNS_JISKEY_NUM_7;
		case RETROK_KP8: return TOWNS_JISKEY_NUM_8;
		case RETROK_KP9: return TOWNS_JISKEY_NUM_9;
		case RETROK_KP_PERIOD: return TOWNS_JISKEY_NUM_DOT;
		case RETROK_KP_DIVIDE: return TOWNS_JISKEY_NUM_SLASH;
		case RETROK_KP_MULTIPLY: return TOWNS_JISKEY_NUM_STAR;
		case RETROK_KP_MINUS: return TOWNS_JISKEY_NUM_MINUS;
		case RETROK_KP_PLUS: return TOWNS_JISKEY_NUM_PLUS;
		case RETROK_KP_ENTER: return TOWNS_JISKEY_NUM_RETURN;
		case RETROK_KP_EQUALS: return TOWNS_JISKEY_NUM_EQUAL;
		
		case RETROK_UP: return TOWNS_JISKEY_UP;
		case RETROK_DOWN: return TOWNS_JISKEY_DOWN;
		case RETROK_RIGHT: return TOWNS_JISKEY_RIGHT;
		case RETROK_LEFT: return TOWNS_JISKEY_LEFT;
		case RETROK_INSERT: return TOWNS_JISKEY_INSERT;
		case RETROK_HOME: return TOWNS_JISKEY_HOME;
		case RETROK_END: return TOWNS_JISKEY_EXECUTE;
		case RETROK_PAGEUP: return TOWNS_JISKEY_PREV;
		case RETROK_PAGEDOWN: return TOWNS_JISKEY_NEXT;
		
		case RETROK_F1: return TOWNS_JISKEY_PF01;
		case RETROK_F2: return TOWNS_JISKEY_PF02;
		case RETROK_F3: return TOWNS_JISKEY_PF03;
		case RETROK_F4: return TOWNS_JISKEY_PF04;
		case RETROK_F5: return TOWNS_JISKEY_PF05;
		case RETROK_F6: return TOWNS_JISKEY_PF06;
		case RETROK_F7: return TOWNS_JISKEY_PF07;
		case RETROK_F8: return TOWNS_JISKEY_PF08;
		case RETROK_F9: return TOWNS_JISKEY_PF09;
		case RETROK_F10: return TOWNS_JISKEY_PF10;
		case RETROK_F11: return TOWNS_JISKEY_PF11;
		case RETROK_F12: return TOWNS_JISKEY_PF12;
		
		case RETROK_CAPSLOCK: return TOWNS_JISKEY_CAPS;
		case RETROK_SCROLLOCK: return TOWNS_JISKEY_COPY;
		case RETROK_RSHIFT: return TOWNS_JISKEY_SHIFT;
		case RETROK_LSHIFT: return TOWNS_JISKEY_SHIFT;
		case RETROK_RCTRL: return TOWNS_JISKEY_CTRL;
		case RETROK_LCTRL: return TOWNS_JISKEY_CTRL;
		case RETROK_RALT: return TOWNS_JISKEY_ALT;
		case RETROK_LALT: return TOWNS_JISKEY_ALT;
		case RETROK_PAUSE: return TOWNS_JISKEY_BREAK;
		
		default: return TOWNS_JISKEY_NULL;
	}
}

void retro_keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t key_modifiers);

struct CachedInput
{
	bool up = false;
	bool down = false;
	bool left = false;
	bool right = false;
	bool a = false;
	bool b = false;
	bool run = false;
	bool pause = false;
};

std::mutex input_lock;
CachedInput cached_input;

std::string get_path_from_environment(unsigned cmd)
{
	if(nullptr == environ_cb)
	{
		return {};
	}
	const char *path = nullptr;
	if(environ_cb(cmd, &path) && nullptr != path)
	{
		return path;
	}
	return {};
}

std::string join_path(const std::string &base,const std::string &leaf)
{
	if(base.empty())
	{
		return leaf;
	}
	return (std::filesystem::path(base) / leaf).string();
}

std::string lower_extension(const std::string &path)
{
	auto ext = std::filesystem::path(path).extension().string();
	for(auto &c : ext)
	{
		c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	}
	if(false == ext.empty() && '.' == ext.front())
	{
		ext.erase(ext.begin());
	}
	return ext;
}

unsigned int StringToGamePortEmu(const std::string& str)
{
	if (str == "gamepad") return TOWNS_GAMEPORTEMU_PHYSICAL0;
	if (str == "physical0") return TOWNS_GAMEPORTEMU_PHYSICAL0;
	if (str == "physical1") return TOWNS_GAMEPORTEMU_PHYSICAL1;
	if (str == "mouse") return TOWNS_GAMEPORTEMU_MOUSE;
	if (str == "cyberstick") return TOWNS_GAMEPORTEMU_CYBERSTICK;
	if (str == "physical0_as_cyberstick") return TOWNS_GAMEPORTEMU_PHYSICAL0_AS_CYBERSTICK;
	if (str == "physical1_as_cyberstick") return TOWNS_GAMEPORTEMU_PHYSICAL1_AS_CYBERSTICK;
	if (str == "6button") return TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL0;
	if (str == "6btn") return TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL0;
	if (str == "capcom") return TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL0;
	if (str == "libblerabble") return TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG0;
	if (str == "libble-rabble") return TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG0;
	if (str == "martypad") return TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL0;
	if (str == "marty") return TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL0;
	if (str == "none") return TOWNS_GAMEPORTEMU_NONE;
	return TOWNS_GAMEPORTEMU_PHYSICAL0;
}

bool IsCyberStickPortType(unsigned int portType)
{
	switch(portType)
	{
	case TOWNS_GAMEPORTEMU_PHYSICAL0_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL1_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL2_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL3_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL4_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL5_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL6_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_PHYSICAL7_AS_CYBERSTICK:
	case TOWNS_GAMEPORTEMU_CYBERSTICK:
		return true;
	default:
		return false;
	}
}

bool IsLibbleRabblePortType(unsigned int portType)
{
	switch(portType)
	{
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG0:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG1:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG2:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG3:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG4:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG5:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG6:
	case TOWNS_GAMEPORTEMU_LIBBLE_RABBLE_PAD_BY_ANALOG7:
		return true;
	default:
		return false;
	}
}

bool IsCapcomPortType(unsigned int portType)
{
	switch(portType)
	{
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL0:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL1:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL2:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL3:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL4:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL5:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL6:
	case TOWNS_GAMEPORTEMU_CAPCOM_BY_PHYSICAL7:
		return true;
	default:
		return false;
	}
}

bool IsSixButtonPortType(unsigned int portType)
{
	switch(portType)
	{
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL0:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL1:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL2:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL3:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL4:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL5:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL6:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_PHYSICAL7:
	case TOWNS_GAMEPORTEMU_6BTNPAD_BY_KEY:
		return true;
	default:
		return false;
	}
}

bool IsMartyPadPortType(unsigned int portType)
{
	switch(portType)
	{
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL0:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL1:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL2:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL3:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL4:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL5:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL6:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_PHYSICAL7:
	case TOWNS_GAMEPORTEMU_MARTYPAD_BY_KEY:
		return true;
	default:
		return false;
	}
}

void AddDescriptor(std::vector<retro_input_descriptor> &descriptors,
	std::vector<std::string> &labels,
	unsigned port,
	unsigned device,
	unsigned index,
	unsigned id,
	const char *label)
{
	labels.emplace_back(label);
	descriptors.push_back({port, device, index, id, labels.back().c_str()});
}

void AppendStandardPadDescriptors(std::vector<retro_input_descriptor> &descriptors,
	std::vector<std::string> &labels,
	unsigned port)
{
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button A");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start (Run)");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select (Pause)");
}

void AppendCapcomDescriptors(std::vector<retro_input_descriptor> &descriptors,
	std::vector<std::string> &labels,
	unsigned port)
{
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button A");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button X");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button Y");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Button L");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button R");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start (Run)");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select (Pause)");
}

void AppendSixButtonDescriptors(std::vector<retro_input_descriptor> &descriptors,
	std::vector<std::string> &labels,
	unsigned port)
{
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button A");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button C");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button X");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Button Y");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Button Z");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start (Run)");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select (Pause)");
}

void AppendLibbleRabbleDescriptors(std::vector<retro_input_descriptor> &descriptors,
	std::vector<std::string> &labels,
	unsigned port)
{
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button A");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button B");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Left 2");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Right 2");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Up 2");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Down 2");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start (Run)");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select (Pause)");
}

void AppendCyberStickDescriptors(std::vector<retro_input_descriptor> &descriptors,
	std::vector<std::string> &labels,
	unsigned port)
{
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Stick X");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Stick Y");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Throttle X");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Throttle Y");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Trigger 1");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Trigger 2");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Trigger 3");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Trigger 4");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Trigger 5");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Trigger 6");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Trigger 7");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Trigger 8");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Trigger 9");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Trigger 10");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Trigger 11");
	AddDescriptor(descriptors, labels, port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Trigger 12");
}

void RefreshInputDescriptors()
{
	if(nullptr == environ_cb)
	{
		return;
	}

	static std::vector<retro_input_descriptor> descriptors;
	static std::vector<std::string> labels;
	descriptors.clear();
	labels.clear();
	descriptors.reserve(64);
	labels.reserve(64);

	if(false == (TOWNS_GAMEPORTEMU_NONE == port0_type || TOWNS_GAMEPORTEMU_MOUSE == port0_type))
	{
		if(true == IsCyberStickPortType(port0_type))
		{
			AppendCyberStickDescriptors(descriptors, labels, 0);
		}
		else if(true == IsLibbleRabblePortType(port0_type))
		{
			AppendLibbleRabbleDescriptors(descriptors, labels, 0);
		}
		else if(true == IsCapcomPortType(port0_type))
		{
			AppendCapcomDescriptors(descriptors, labels, 0);
		}
		else if(true == IsSixButtonPortType(port0_type))
		{
			AppendSixButtonDescriptors(descriptors, labels, 0);
		}
		else if(true == IsMartyPadPortType(port0_type))
		{
			AppendStandardPadDescriptors(descriptors, labels, 0);
		}
		else
		{
			AppendStandardPadDescriptors(descriptors, labels, 0);
		}
	}

	if(false == (TOWNS_GAMEPORTEMU_NONE == port1_type || TOWNS_GAMEPORTEMU_MOUSE == port1_type))
	{
		if(true == IsCyberStickPortType(port1_type))
		{
			AppendCyberStickDescriptors(descriptors, labels, 1);
		}
		else if(true == IsLibbleRabblePortType(port1_type))
		{
			AppendLibbleRabbleDescriptors(descriptors, labels, 1);
		}
		else if(true == IsCapcomPortType(port1_type))
		{
			AppendCapcomDescriptors(descriptors, labels, 1);
		}
		else if(true == IsSixButtonPortType(port1_type))
		{
			AppendSixButtonDescriptors(descriptors, labels, 1);
		}
		else if(true == IsMartyPadPortType(port1_type))
		{
			AppendStandardPadDescriptors(descriptors, labels, 1);
		}
		else
		{
			AppendStandardPadDescriptors(descriptors, labels, 1);
		}
	}

	descriptors.push_back({0, 0, 0, 0, nullptr});
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, descriptors.data());
}

bool is_cd_extension(const std::string &ext)
{
	return "cue" == ext || "bin" == ext || "iso" == ext || "mds" == ext ||
	       "mdf" == ext || "ccd" == ext || "chd" == ext;
}

bool is_m3u_extension(const std::string &ext)
{
	return "m3u" == ext;
}

bool is_bin_extension(const std::string &ext)
{
	return "bin" == ext;
}

bool is_floppy_extension(const std::string &ext)
{
	return "d77" == ext || "d88" == ext || "rdd" == ext ||
	       "img" == ext || "fdi" == ext || "hdm" == ext;
}

bool is_harddisk_extension(const std::string &ext)
{
	return "h0" == ext;
}

enum class ContentKind
{
	Unknown,
	CD,
	Floppy,
	HardDisk
};

ContentKind classify_content_path(const std::string &path);
bool is_small_bin_floppy(const std::string &path);
void log(enum retro_log_level level, const char *message);
void logf(enum retro_log_level level, const char *fmt, ...);
void ResetMouseTracking();

struct MountedContentState
{
	bool haveFloppy = false;
	bool haveCD = false;
	bool haveHardDisk = false;
	unsigned floppyCount = 0;
};

bool mount_content_path(TownsStartParameters &argv, const std::string &path, MountedContentState &state)
{
	const auto kind = classify_content_path(path);
	switch(kind)
	{
	case ContentKind::Floppy:
		if(state.floppyCount < TownsStartParameters::NUM_FDDRIVES)
		{
			argv.fdImgFName[state.floppyCount] = path;
			state.haveFloppy = true;
			logf(RETRO_LOG_INFO, "Tsugaru libretro: mounting floppy image=\"%s\"\n", argv.fdImgFName[state.floppyCount].c_str());
			++state.floppyCount;
			return true;
		}
		logf(RETRO_LOG_WARN, "Tsugaru libretro: no floppy slot available for \"%s\"\n", path.c_str());
		return false;
	case ContentKind::CD:
		if(false == state.haveCD)
		{
			argv.cdImgFName = path;
			argv.townsType = TOWNSTYPE_2_MX;
			argv.memSizeInMB = 16;
			argv.useFPU = true;
			state.haveCD = true;
			logf(RETRO_LOG_INFO, "Tsugaru libretro: mounting CD image=\"%s\"\n", argv.cdImgFName.c_str());
			return true;
		}
		logf(RETRO_LOG_WARN, "Tsugaru libretro: additional CD entry ignored: \"%s\"\n", path.c_str());
		return true;
	case ContentKind::HardDisk:
		if(false == state.haveHardDisk)
		{
			argv.scsiImg[0].imageType = TownsStartParameters::SCSIIMAGE_HARDDISK;
			argv.scsiImg[0].imgFName = path;
			state.haveHardDisk = true;
			logf(RETRO_LOG_INFO, "Tsugaru libretro: mounting hard disk image=\"%s\"\n", argv.scsiImg[0].imgFName.c_str());
			return true;
		}
		logf(RETRO_LOG_WARN, "Tsugaru libretro: additional hard disk entry ignored: \"%s\"\n", path.c_str());
		return true;
	case ContentKind::Unknown:
	default:
		logf(RETRO_LOG_WARN, "Tsugaru libretro: unsupported media entry ignored: \"%s\"\n", path.c_str());
		return false;
	}
}

std::string trim_copy(const std::string &value)
{
	const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch)
	{
		return std::isspace(ch) != 0;
	});
	const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch)
	{
		return std::isspace(ch) != 0;
	}).base();
	if(first >= last)
	{
		return {};
	}
	return std::string(first, last);
}

std::vector<std::string> read_m3u_entries(const std::string &playlist)
{
	std::vector<std::string> entries;
	std::ifstream file(playlist);
	if(false == file.is_open())
	{
		return entries;
	}

	const auto base = std::filesystem::path(playlist).parent_path();
	std::string line;
	while(std::getline(file, line))
	{
		line = trim_copy(line);
		if(line.empty() || '#' == line[0])
		{
			continue;
		}

		const auto comment = line.find('#');
		if(std::string::npos != comment)
		{
			line = trim_copy(line.substr(0, comment));
			if(line.empty())
			{
				continue;
			}
		}

		if(line.find(':') != std::string::npos || (!line.empty() && ('\\' == line[0] || '/' == line[0])))
		{
			entries.push_back(line);
		}
		else
		{
			entries.push_back(base.empty() ? line : join_path(base.string(), line));
		}
	}

	return entries;
}

ContentKind classify_content_path(const std::string &path)
{
	const auto ext = lower_extension(path);
	if(true == is_small_bin_floppy(path) || true == is_floppy_extension(ext))
	{
		return ContentKind::Floppy;
	}
	if(true == is_cd_extension(ext))
	{
		return ContentKind::CD;
	}
	if(true == is_harddisk_extension(ext))
	{
		return ContentKind::HardDisk;
	}
	return ContentKind::Unknown;
}

const char *content_kind_label(ContentKind kind)
{
	switch(kind)
	{
	case ContentKind::CD:
		return "cd";
	case ContentKind::Floppy:
		return "floppy";
	case ContentKind::HardDisk:
		return "harddisk";
	case ContentKind::Unknown:
	default:
		return "unknown";
	}
}

bool is_small_bin_floppy(const std::string &path)
{
	if(false == is_bin_extension(lower_extension(path)))
	{
		return false;
	}

	std::error_code ec;
	const auto size = std::filesystem::file_size(path, ec);
	return !ec && 0 < size && size <= SMALL_BIN_FLOPPY_MAX;
}

std::string resolve_content_path(const std::string &path)
{
	if(path.empty() || std::filesystem::exists(path))
	{
		return path;
	}

	const auto requested = std::filesystem::path(path);
	const auto dir = requested.parent_path();
	const auto stem = requested.filename().string();
	if(stem.empty() || false == std::filesystem::is_directory(dir))
	{
		return path;
	}

	std::vector<std::filesystem::path> matches;
	for(const auto &entry : std::filesystem::directory_iterator(dir))
	{
		if(false == entry.is_regular_file())
		{
			continue;
		}
		const auto candidate = entry.path();
		const auto candidateStem = candidate.stem().string();
		const auto ext = lower_extension(candidate.string());
		if((true == is_cd_extension(ext) || true == is_floppy_extension(ext) || true == is_harddisk_extension(ext)) &&
		   (candidateStem == stem ||
		    (candidateStem.size() > stem.size() &&
		     0 == candidateStem.compare(0, stem.size(), stem) &&
		     ' ' == candidateStem[stem.size()])))
		{
			matches.push_back(candidate);
		}
	}

	if(1 == matches.size())
	{
		return matches.front().string();
	}
	return path;
}

unsigned int cmos_index_from_io(unsigned int ioport)
{
	return (ioport - TOWNSIO_CMOS_BASE) / 2;
}

void log(enum retro_log_level level, const char *message)
{
	if(nullptr != log_cb)
	{
		log_cb(level, "%s", message);
	}
}

void logf(enum retro_log_level level, const char *fmt, ...)
{
	if(nullptr != log_cb)
	{
		char text[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		log_cb(level, "%s", text);
	}
}

void set_environment_defaults()
{
	if(nullptr == environ_cb)
	{
		return;
	}

	auto pixelFormat = RETRO_PIXEL_FORMAT_XRGB8888;
	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &pixelFormat);

	bool supportNoGame = true;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &supportNoGame);
	RefreshInputDescriptors();

	static const retro_variable variables[] =
	{
		{ "tsugaru_model", "FM Towns Model; auto|MODEL2|2F|20F|UX|HR|MX|MARTY" },
		{ "tsugaru_ram_mb", "RAM Size; 6|4|8|10|12|16" },
		{ "tsugaru_mouse_mode", "Mouse Mode; relative|integrated" },
		{ "tsugaru_port0_type", "Port 0 Device; gamepad|mouse|cyberstick|libblerabble|martypad|6button|capcom|none" },
		{ "tsugaru_port1_type", "Port 1 Device; mouse|gamepad|cyberstick|libblerabble|martypad|6button|capcom|none" },
		{ nullptr, nullptr },
	};
	environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable *>(variables));
}

class LibretroWindow : public Outside_World::WindowInterface
{
public:
	std::mutex frameLock;
	std::vector<uint32_t> xrgb;
	unsigned width = BASE_WIDTH;
	unsigned height = BASE_HEIGHT;
	bool haveFrame = false;
	uint64_t frameSerial = 0;

	void Start(void) override {}
	void Stop(void) override {}
	void Interval(void) override
	{
		BaseInterval();
		ImportMostRecentImage();
	}
	void Render(bool) override
	{
		Interval();
	}
	void Communicate(Outside_World *) override {}
	void UpdateImage(TownsRender::ImageCopy &img) override
	{
		ImportImage(img);
	}

	void ImportMostRecentImage()
	{
		if(true == winThr.newImageRendered)
		{
			ImportImage(winThr.mostRecentImage);
			winThr.newImageRendered = false;
		}
	}

	void ImportImage(const TownsRender::ImageCopy &img)
	{
		if(0 == img.wid || 0 == img.hei || img.rgba.size() < img.wid * img.hei * 4)
		{
			return;
		}

		std::vector<uint32_t> converted;
		converted.resize(img.wid * img.hei);
		for(size_t i = 0; i < converted.size(); ++i)
		{
			const auto *p = img.rgba.data() + i * 4;
			converted[i] = (static_cast<uint32_t>(p[0]) << 16) |
			               (static_cast<uint32_t>(p[1]) << 8) |
			                static_cast<uint32_t>(p[2]);
		}

		std::lock_guard<std::mutex> lock(frameLock);
		xrgb.swap(converted);
		width = img.wid;
		height = img.hei;
		haveFrame = true;
		++frameSerial;
	}

	bool CopyFrame(std::vector<uint32_t> &out,unsigned &wid,unsigned &hei)
	{
		Interval();
		std::lock_guard<std::mutex> lock(frameLock);
		if(true != haveFrame || true == xrgb.empty())
		{
			return false;
		}
		out = xrgb;
		wid = width;
		hei = height;
		return true;
	}
};

class LibretroSound : public Outside_World::Sound
{
public:
	std::mutex audioLock;
	std::vector<int16_t> samples;
	bool fmPlaying = false;
	bool beepPlaying = false;
	bool cddaPlaying = false;
	DiscImage::MinSecFrm cddaPos;

	void Start(void) override {}
	void Stop(void) override
	{
		std::lock_guard<std::mutex> lock(audioLock);
		samples.clear();
		fmPlaying = false;
		beepPlaying = false;
		cddaPlaying = false;
	}
	void Polling(void) override {}

	void CDDAPlay(const DiscImage &,DiscImage::MinSecFrm from,DiscImage::MinSecFrm, bool, unsigned int, unsigned int) override
	{
		cddaPos = from;
		cddaPlaying = true;
	}
	void CDDASetVolume(float,float) override {}
	void CDDAStop(void) override { cddaPlaying = false; }
	void CDDAPause(void) override { cddaPlaying = false; }
	void CDDAResume(void) override { cddaPlaying = true; }
	bool CDDAIsPlaying(void) override { return cddaPlaying; }
	DiscImage::MinSecFrm CDDACurrentPosition(void) override { return cddaPos; }

	void FMPCMPlay(std::vector<unsigned char> &wave) override
	{
		AppendStereo16LE(wave);
		fmPlaying = true;
	}
	void FMPCMPlayStop(void) override { fmPlaying = false; }
	bool FMPCMChannelPlaying(void) override
	{
		std::lock_guard<std::mutex> lock(audioLock);
		return samples.size() >= AUDIO_FRAMES_PER_RUN * 2;
	}

	void BeepPlay(int,std::vector<unsigned char> &wave) override
	{
		AppendStereo16LE(wave);
		beepPlaying = true;
	}
	void BeepPlayStop(void) override { beepPlaying = false; }
	bool BeepChannelPlaying(void) const override
	{
		return beepPlaying;
	}

	void AppendStereo16LE(const std::vector<unsigned char> &wave)
	{
		std::lock_guard<std::mutex> lock(audioLock);
		const size_t n = wave.size() / 2;
		const size_t limit = 44100 * 2;
		for(size_t i = 0; i < n; ++i)
		{
			const auto lo = static_cast<uint16_t>(wave[i * 2]);
			const auto hi = static_cast<uint16_t>(wave[i * 2 + 1]);
			samples.push_back(static_cast<int16_t>(lo | (hi << 8)));
		}
		if(samples.size() > limit)
		{
			samples.erase(samples.begin(), samples.end() - limit);
		}
	}

	size_t PopFrames(int16_t *dst,size_t frames)
	{
		std::lock_guard<std::mutex> lock(audioLock);
		const size_t availableFrames = samples.size() / 2;
		const size_t n = std::min(frames, availableFrames);
		if(0 < n)
		{
			std::memcpy(dst, samples.data(), n * 2 * sizeof(int16_t));
			samples.erase(samples.begin(), samples.begin() + n * 2);
		}
		if(0 == samples.size())
		{
			fmPlaying = false;
			beepPlaying = false;
		}
		return n;
	}
};

class LibretroOutsideWorld : public Outside_World
{
public:
	LibretroWindow *window = nullptr;
	LibretroSound *sound = nullptr;

	std::string GetProgramResourceDirectory(void) const override
	{
		return system_directory;
	}
	void Start(void) override {}
	void Stop(void) override {}
	void DevicePolling(FMTownsCommon &towns) override
	{
		if (!input_poll_cb || !input_state_cb) return;
		input_poll_cb();

		auto read_button = [&](unsigned port, unsigned id) -> bool
		{
			return 0 != input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id);
		};
		auto read_axis = [&](unsigned port, unsigned index, unsigned id) -> int
		{
			return static_cast<int>(input_state_cb(port, RETRO_DEVICE_ANALOG, index, id));
		};
		auto read_mouse_axis = [&](unsigned port, unsigned id) -> int
		{
			return static_cast<int>(input_state_cb(port, RETRO_DEVICE_MOUSE, 0, id));
		};
		auto read_pointer_axis = [&](unsigned port, unsigned id) -> int
		{
			return static_cast<int>(input_state_cb(port, RETRO_DEVICE_POINTER, 0, id));
		};
		auto read_mouse_button = [&](unsigned port, unsigned id) -> bool
		{
			return 0 != input_state_cb(port, RETRO_DEVICE_MOUSE, 0, id);
		};
		auto read_pointer_pressed = [&](unsigned port) -> bool
		{
			return 0 != input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
		};
		auto read_axis_with_fallback = [&](unsigned port, unsigned index, unsigned id, unsigned negative_button, unsigned positive_button) -> int
		{
			const int analog = read_axis(port, index, id);
			if(0 != analog)
			{
				return analog / 256;
			}
			if(true == read_button(port, positive_button))
			{
				return 127;
			}
			if(true == read_button(port, negative_button))
			{
				return -128;
			}
			return 0;
		};

		unsigned mousePort = 2;
		for(unsigned port = 0; port < 2; ++port)
		{
			const unsigned int portType = (port == 0) ? port0_type : port1_type;
			if(TOWNS_GAMEPORTEMU_MOUSE == portType)
			{
				mousePort = port;
				break;
			}
		}
		if(mousePort < 2)
		{
			const int mouseDx = read_mouse_axis(mousePort, RETRO_DEVICE_ID_MOUSE_X);
			const int mouseDy = read_mouse_axis(mousePort, RETRO_DEVICE_ID_MOUSE_Y);
			const int pointerX = read_pointer_axis(mousePort, RETRO_DEVICE_ID_POINTER_X);
			const int pointerY = read_pointer_axis(mousePort, RETRO_DEVICE_ID_POINTER_Y);
			const bool pointerPressed = read_pointer_pressed(mousePort);
			bool leftButton = read_mouse_button(mousePort, RETRO_DEVICE_ID_MOUSE_LEFT);
			bool rightButton = read_mouse_button(mousePort, RETRO_DEVICE_ID_MOUSE_RIGHT);

			if(true == mouse_mode_integrated &&
			   (0 != pointerX || 0 != pointerY || true == pointerPressed || true == mouse_pointer_prev_valid[mousePort]))
			{
				if(false == mouse_pointer_prev_valid[mousePort])
				{
					mouse_pointer_prev_x[mousePort] = pointerX;
					mouse_pointer_prev_y[mousePort] = pointerY;
					mouse_pointer_prev_valid[mousePort] = true;
				}
				const int dx = pointerX - mouse_pointer_prev_x[mousePort];
				const int dy = pointerY - mouse_pointer_prev_y[mousePort];
				mouse_pointer_prev_x[mousePort] = pointerX;
				mouse_pointer_prev_y[mousePort] = pointerY;
				leftButton = leftButton || pointerPressed;
				towns.SetMouseButtonState(leftButton, rightButton);
				if(0 != dx || 0 != dy)
				{
					towns.ControlMouseByDiffDirect(dx, dy);
				}
			}
			else
			{
				towns.SetMouseButtonState(leftButton, rightButton);
				if(0 != mouseDx || 0 != mouseDy)
				{
					towns.ControlMouseByDiffDirect(mouseDx, mouseDy);
				}
			}
		}
		
		for (unsigned port = 0; port < 2; ++port)
		{
			unsigned int portType = (port == 0) ? port0_type : port1_type;
			
			if (portType == TOWNS_GAMEPORTEMU_NONE || portType == TOWNS_GAMEPORTEMU_MOUSE)
				continue;

			if(true == IsCyberStickPortType(portType))
			{
				const bool left = read_button(port, RETRO_DEVICE_ID_JOYPAD_LEFT) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT) < 0;
				const bool right = read_button(port, RETRO_DEVICE_ID_JOYPAD_RIGHT) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT) > 0;
				const bool up = read_button(port, RETRO_DEVICE_ID_JOYPAD_UP) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN) < 0;
				const bool down = read_button(port, RETRO_DEVICE_ID_JOYPAD_DOWN) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN) > 0;
				unsigned int trig = 0;
				auto setTrig = [&](unsigned bit, unsigned id)
				{
					if(true == read_button(port, id))
					{
						trig |= (1u << bit);
					}
				};

				const int x = read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT);
				const int y = read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN);
				const int z = read_axis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) / 256;
				const int w = read_axis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) / 256;

				setTrig(11, RETRO_DEVICE_ID_JOYPAD_A);
				setTrig(10, RETRO_DEVICE_ID_JOYPAD_B);
				setTrig(5, RETRO_DEVICE_ID_JOYPAD_X);
				setTrig(4, RETRO_DEVICE_ID_JOYPAD_Y);
				setTrig(9, RETRO_DEVICE_ID_JOYPAD_L);
				setTrig(8, RETRO_DEVICE_ID_JOYPAD_R);
				setTrig(7, RETRO_DEVICE_ID_JOYPAD_L2);
				setTrig(6, RETRO_DEVICE_ID_JOYPAD_R2);
				setTrig(0, RETRO_DEVICE_ID_JOYPAD_SELECT);
				setTrig(1, RETRO_DEVICE_ID_JOYPAD_START);
				setTrig(2, RETRO_DEVICE_ID_JOYPAD_L3);
				setTrig(3, RETRO_DEVICE_ID_JOYPAD_R3);

				towns.SetCyberStickState(port, x, y, z, w, trig);
			}
			else if(true == IsLibbleRabblePortType(portType))
			{
				const bool left = read_button(port, RETRO_DEVICE_ID_JOYPAD_LEFT) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT) < 0;
				const bool right = read_button(port, RETRO_DEVICE_ID_JOYPAD_RIGHT) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT) > 0;
				const bool up = read_button(port, RETRO_DEVICE_ID_JOYPAD_UP) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN) < 0;
				const bool down = read_button(port, RETRO_DEVICE_ID_JOYPAD_DOWN) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN) > 0;
				const bool a = read_button(port, RETRO_DEVICE_ID_JOYPAD_A);
				const bool b = read_button(port, RETRO_DEVICE_ID_JOYPAD_B);
				const bool run = read_button(port, RETRO_DEVICE_ID_JOYPAD_START);
				const bool pause = read_button(port, RETRO_DEVICE_ID_JOYPAD_SELECT);
				const bool left2 = read_button(port, RETRO_DEVICE_ID_JOYPAD_X) || read_axis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) < -32;
				const bool right2 = read_button(port, RETRO_DEVICE_ID_JOYPAD_Y) || read_axis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) > 32;
				const bool up2 = read_button(port, RETRO_DEVICE_ID_JOYPAD_L) || read_axis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) < -32;
				const bool down2 = read_button(port, RETRO_DEVICE_ID_JOYPAD_R) || read_axis(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) > 32;

				towns.SetLibbleRabblePadState(
					a,
					b,
					left,
					right,
					up,
					down,
					left2,
					right2,
					up2,
					down2,
					run,
					pause,
					towns.state.townsTime
				);
			}
			else if(true == IsCapcomPortType(portType) || true == IsSixButtonPortType(portType))
			{
				const bool left = read_button(port, RETRO_DEVICE_ID_JOYPAD_LEFT);
				const bool right = read_button(port, RETRO_DEVICE_ID_JOYPAD_RIGHT);
				const bool up = read_button(port, RETRO_DEVICE_ID_JOYPAD_UP);
				const bool down = read_button(port, RETRO_DEVICE_ID_JOYPAD_DOWN);
				const bool a = read_button(port, RETRO_DEVICE_ID_JOYPAD_A);
				const bool b = read_button(port, RETRO_DEVICE_ID_JOYPAD_B);
				const bool x = read_button(port, RETRO_DEVICE_ID_JOYPAD_X);
				const bool y = read_button(port, RETRO_DEVICE_ID_JOYPAD_Y);
				const bool l = read_button(port, RETRO_DEVICE_ID_JOYPAD_L);
				const bool r = read_button(port, RETRO_DEVICE_ID_JOYPAD_R);
				const bool run = read_button(port, RETRO_DEVICE_ID_JOYPAD_START);
				const bool pause = read_button(port, RETRO_DEVICE_ID_JOYPAD_SELECT);

				towns.gameport.state.ports[port].SetCAPCOMCPSFState(
					left, right, up, down, a, b, x, y, l, r, run, pause,
					towns.state.townsTime
				);
			}
			else
			{
				const bool left = read_button(port, RETRO_DEVICE_ID_JOYPAD_LEFT) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT) < 0;
				const bool right = read_button(port, RETRO_DEVICE_ID_JOYPAD_RIGHT) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT) > 0;
				const bool up = read_button(port, RETRO_DEVICE_ID_JOYPAD_UP) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN) < 0;
				const bool down = read_button(port, RETRO_DEVICE_ID_JOYPAD_DOWN) || read_axis_with_fallback(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN) > 0;
				const bool a = read_button(port, RETRO_DEVICE_ID_JOYPAD_A);
				const bool b = read_button(port, RETRO_DEVICE_ID_JOYPAD_B);
				const bool run = read_button(port, RETRO_DEVICE_ID_JOYPAD_START);
				const bool pause = read_button(port, RETRO_DEVICE_ID_JOYPAD_SELECT);
				towns.SetGamePadState(port, a, b, left, right, up, down, run, pause, false);
			}
		}
	}
	bool ImageNeedsFlip(void) override
	{
		return false;
	}
	void SetKeyboardLayout(unsigned int) override {}
	void CacheGamePadIndicesThatNeedUpdates(void) override
	{
		gamePadsNeedUpdate.clear();
		UseGamePad(0);
	}
	WindowInterface *CreateWindowInterface(void) const override
	{
		auto *created = new LibretroWindow;
		const_cast<LibretroOutsideWorld *>(this)->window = created;
		return created;
	}
	void DeleteWindowInterface(WindowInterface *ptr) const override
	{
		if(ptr == window)
		{
			const_cast<LibretroOutsideWorld *>(this)->window = nullptr;
		}
		delete static_cast<LibretroWindow *>(ptr);
	}
	Sound *CreateSound(void) const override
	{
		auto *created = new LibretroSound;
		const_cast<LibretroOutsideWorld *>(this)->sound = created;
		return created;
	}
	void DeleteSound(Sound *ptr) const override
	{
		if(ptr == sound)
		{
			const_cast<LibretroOutsideWorld *>(this)->sound = nullptr;
		}
		delete static_cast<LibretroSound *>(ptr);
	}
};

class LibretroUIThread : public TownsUIThread
{
private:
	void Main(TownsThread &,FMTownsCommon &,const TownsARGV &,Outside_World &) override {}
public:
	void ExecCommandQueue(TownsThread &,FMTownsCommon &,Outside_World *outside_world,Outside_World::Sound *) override
	{
		while(nullptr != outside_world && true != outside_world->commandQueue.empty())
		{
			outside_world->commandQueue.pop();
		}
	}
};

class Runtime
{
public:
	std::unique_ptr<FMTownsTemplate<i486DXDefaultFidelity>> towns;
	std::unique_ptr<TownsThread> townsThread;
	std::unique_ptr<LibretroUIThread> uiThread;
	std::unique_ptr<LibretroOutsideWorld> outside;
	Outside_World::WindowInterface *window = nullptr;
	Outside_World::Sound *sound = nullptr;
	bool loaded = false;
	unsigned int bootKeyComb = BOOT_KEYCOMB_NONE;
	bool contentIsCD = false;
	bool contentIsFD = false;

	~Runtime()
	{
		unload();
	}

	bool load(const retro_game_info *game)
	{
		unload();
		const auto requestedPath = (nullptr != game && nullptr != game->path) ? std::string(game->path) : std::string();
		content_path = resolve_content_path(requestedPath);
		logf(RETRO_LOG_INFO, "Tsugaru libretro: retro_load_game path=\"%s\"\n", requestedPath.c_str());
		if(content_path != requestedPath)
		{
			logf(RETRO_LOG_WARN, "Tsugaru libretro: resolved content path=\"%s\"\n", content_path.c_str());
		}

		TownsStartParameters argv;
		contentIsCD = false;
		contentIsFD = false;
		argv.ROMPath = PreferredRomPath();
		argv.CMOSFName = join_path(PreferredSavePath(), "tsugaru_cmos.bin");
		argv.autoSaveCMOS = true;
		argv.autoStart = true;
		argv.noWait = true;
		argv.catchUpRealTime = false;
		argv.interactive = false;
		argv.townsType = TOWNSTYPE_1F_2F;
		argv.memSizeInMB = 6;
		argv.keyboardMode = TOWNS_KEYBOARD_MODE_DIRECT;

		mouse_mode_integrated = false;
		if(environ_cb)
		{
			retro_variable var;
			var.key = "tsugaru_mouse_mode";
			if(environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
			{
				mouse_mode_integrated = (0 == std::strcmp(var.value, "integrated"));
				logf(RETRO_LOG_INFO, "Tsugaru libretro: Mouse mode=\"%s\"\n", var.value);
			}
		}
		ResetMouseTracking();
		
		// Read port type options
		retro_variable var;
		var.key = "tsugaru_port0_type";
		if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			port0_type = StringToGamePortEmu(var.value);
			argv.gamePort[0] = port0_type;
			logf(RETRO_LOG_INFO, "Tsugaru libretro: Port 0 type=\"%s\" (%u)\n", var.value, port0_type);
		}
		else
		{
			port0_type = TOWNS_GAMEPORTEMU_PHYSICAL0;
			argv.gamePort[0] = port0_type;
		}
		
		var.key = "tsugaru_port1_type";
		if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		{
			port1_type = StringToGamePortEmu(var.value);
			argv.gamePort[1] = port1_type;
			logf(RETRO_LOG_INFO, "Tsugaru libretro: Port 1 type=\"%s\" (%u)\n", var.value, port1_type);
		}
		else
		{
			port1_type = TOWNS_GAMEPORTEMU_MOUSE;
			argv.gamePort[1] = port1_type;
		}

		RefreshInputDescriptors();
		
		argv.specialPath.push_back({"${system}", system_directory});
		argv.specialPath.push_back({"${save}", PreferredSavePath()});

		const auto ext = lower_extension(content_path);
		const auto kind = classify_content_path(content_path);
		logf(RETRO_LOG_INFO, "Tsugaru libretro: content extension=\"%s\"\n", ext.c_str());
		logf(RETRO_LOG_INFO, "Tsugaru libretro: content classification candidate=%s\n",
			true == is_m3u_extension(ext) ? "playlist" : content_kind_label(kind));

		MountedContentState mounted;
		if(true == is_m3u_extension(ext))
		{
			const auto mediaPaths = read_m3u_entries(content_path);
			if(mediaPaths.empty())
			{
				log(RETRO_LOG_ERROR, "Tsugaru libretro: M3U playlist is empty or unreadable.\n");
				return false;
			}
			logf(RETRO_LOG_INFO, "Tsugaru libretro: playlist entries=%zu\n", mediaPaths.size());
			for(const auto &mediaPath : mediaPaths)
			{
				mount_content_path(argv, mediaPath, mounted);
			}
		}
		else
		{
			mount_content_path(argv, content_path, mounted);
		}

		if(true == mounted.haveFloppy)
		{
			bootKeyComb = BOOT_KEYCOMB_F0;
		}
		else if(true == mounted.haveCD)
		{
			bootKeyComb = BOOT_KEYCOMB_CD;
		}
		else if(true == mounted.haveHardDisk)
		{
			bootKeyComb = BOOT_KEYCOMB_H0;
		}
		else
		{
			logf(RETRO_LOG_WARN, "Tsugaru libretro: no image mounted from content path\n");
		}
		if(true == is_m3u_extension(ext) && false == mounted.haveFloppy && false == mounted.haveCD && false == mounted.haveHardDisk)
		{
			log(RETRO_LOG_ERROR, "Tsugaru libretro: M3U playlist contained no supported media.\n");
			return false;
		}
		argv.bootKeyComb = bootKeyComb;
		logf(RETRO_LOG_INFO, "Tsugaru libretro: boot key combination=\"%s\"\n", TownsKeyCombToStr(bootKeyComb).c_str());

		outside = std::make_unique<LibretroOutsideWorld>();
		sound = outside->CreateSound();
		window = outside->CreateWindowInterface();
		towns = std::make_unique<FMTownsTemplate<i486DXDefaultFidelity>>();
		if(true != FMTownsCommon::Setup(*towns, outside.get(), window, argv))
		{
			outside->DeleteSound(sound);
			outside->DeleteWindowInterface(window);
			sound = nullptr;
			window = nullptr;
			towns.reset();
			outside.reset();
			log(RETRO_LOG_ERROR, "Tsugaru libretro: failed to set up FM Towns VM.\n");
			return false;
		}
		ConfigureBootDevice();

		townsThread = std::make_unique<TownsThread>();
		townsThread->SetRunMode(TownsThread::RUNMODE_RUN);
		uiThread = std::make_unique<LibretroUIThread>();
		townsThread->VMStart(towns.get(), outside.get(), uiThread.get());
		sound->Start();
		window->Start();
		loaded = true;
		return true;
	}

	void unload()
	{
		if(loaded && nullptr != townsThread && nullptr != towns && nullptr != outside && nullptr != uiThread)
		{
			if(nullptr != sound)
			{
				sound->Stop();
			}
			if(nullptr != window)
			{
				window->NotifyVMClosed();
			}
			townsThread->VMEnd(towns.get(), outside.get(), uiThread.get());
		}
		if(nullptr != window)
		{
			window->Stop();
		}
		if(nullptr != outside)
		{
			if(nullptr != sound)
			{
				outside->DeleteSound(sound);
			}
			if(nullptr != window)
			{
				outside->DeleteWindowInterface(window);
			}
		}
		sound = nullptr;
		window = nullptr;
		uiThread.reset();
		townsThread.reset();
		towns.reset();
		outside.reset();
		loaded = false;
		bootKeyComb = BOOT_KEYCOMB_NONE;
		contentIsCD = false;
		contentIsFD = false;
		ResetMouseTracking();
	}

	void reset()
	{
		if(false == loaded || nullptr == towns || nullptr == townsThread || nullptr == outside || nullptr == uiThread)
		{
			return;
		}

		towns->Reset(bootKeyComb);
		ResetMouseTracking();
		frame_counter = 0;
	}

	bool CopyFrame(std::vector<uint32_t> &out,unsigned &wid,unsigned &hei)
	{
		if(nullptr != outside && nullptr != outside->window)
		{
			return outside->window->CopyFrame(out, wid, hei);
		}
		return false;
	}

	bool Advance()
	{
		if(false == loaded || nullptr == townsThread || nullptr == towns || nullptr == outside || nullptr == window || nullptr == sound || nullptr == uiThread)
		{
			return false;
		}

		auto *libWindow = static_cast<LibretroWindow *>(window);
		const uint64_t startSerial = libWindow->frameSerial;
		const auto startWall = std::chrono::steady_clock::now();
		const auto wallBudget = (true == libWindow->haveFrame ? std::chrono::milliseconds(15) : std::chrono::milliseconds(45));
		const auto startTownsTime = towns->state.townsTime;
		const auto targetTownsTime = startTownsTime + 1000000000ULL / static_cast<unsigned long long>(FPS);
		const unsigned maxSlices = (TownsThread::RUNMODE_RUN == townsThread->GetRunMode() ? 24u : 1u);
		bool terminate = false;
		for(unsigned slice = 0; slice < maxSlices && false == terminate && libWindow->frameSerial == startSerial; ++slice)
		{
			terminate = townsThread->VMRunSlice(towns.get(), outside.get(), sound, window, uiThread.get(), false, false);
			window->Interval();
			if(targetTownsTime <= towns->state.townsTime)
			{
				break;
			}
			if(wallBudget <= std::chrono::steady_clock::now() - startWall)
			{
				break;
			}
		}

		if(true == terminate)
		{
			unload();
			return false;
		}

		return true;
	}

	size_t PopAudio(int16_t *dst,size_t frames)
	{
		if(nullptr != outside && nullptr != outside->sound)
		{
			return outside->sound->PopFrames(dst, frames);
		}
		return 0;
	}

	std::string PreferredRomPath() const
	{
		auto subdir = join_path(system_directory, "fmtowns");
		if(std::filesystem::is_directory(subdir))
		{
			return subdir;
		}
		return system_directory;
	}

	std::string PreferredSavePath() const
	{
		auto base = save_directory.empty() ? system_directory : save_directory;
		auto path = join_path(base, "Tsugaru");
		std::error_code ec;
		std::filesystem::create_directories(path, ec);
		return path;
	}

	void ConfigureBootDevice()
	{
		if(nullptr == towns)
		{
			return;
		}
		switch(bootKeyComb)
		{
		case BOOT_KEYCOMB_CD:
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_TYPE)] = 8;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_UNIT)] = 0;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_BOOT_DEV)] = 0x80;
			break;
		case BOOT_KEYCOMB_F0:
		case BOOT_KEYCOMB_F1:
		case BOOT_KEYCOMB_F2:
		case BOOT_KEYCOMB_F3:
		{
			const unsigned unit = bootKeyComb - BOOT_KEYCOMB_F0;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_TYPE)] = 2;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_UNIT)] = unit;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_BOOT_DEV)] = static_cast<unsigned char>(0x20 + unit);
		}
			break;
		case BOOT_KEYCOMB_H0:
		case BOOT_KEYCOMB_H1:
		case BOOT_KEYCOMB_H2:
		case BOOT_KEYCOMB_H3:
		case BOOT_KEYCOMB_H4:
		{
			const unsigned unit = bootKeyComb - BOOT_KEYCOMB_H0;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_TYPE)] = 1;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_UNIT)] = unit;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_BOOT_DEV)] = static_cast<unsigned char>(0x10 + unit);
		}
			break;
		default:
			break;
		}
	}
};

Runtime runtime;

void retro_keyboard_event(bool down, unsigned keycode, uint32_t, uint16_t)
{
	const unsigned char townsKey = RetroKeyToTownsKey(keycode);
	if(TOWNS_JISKEY_NULL == townsKey)
	{
		return;
	}
	if(nullptr != runtime.towns && true == runtime.loaded)
	{
		unsigned char keyCodeBuf[2];
		keyCodeBuf[0] = TOWNS_KEYFLAG_TYPE_JIS | TOWNS_KEYFLAG_TYPE_FIRSTBYTE;
		keyCodeBuf[0] |= down ? TOWNS_KEYFLAG_PRESS : TOWNS_KEYFLAG_RELEASE;
		keyCodeBuf[1] = townsKey;
		runtime.towns->keyboard.TypeToFifo(keyCodeBuf);
	}
}

void draw_placeholder_frame()
{
	const uint32_t background = runtime.loaded ? 0x00182010u : (content_path.empty() ? 0x00101820u : 0x00102018u);
	framebuffer.fill(background);
	const unsigned markerX = static_cast<unsigned>((frame_counter * 3) % BASE_WIDTH);
	for(unsigned y = 0; y < BASE_HEIGHT; ++y)
	{
		framebuffer[(y * BASE_WIDTH) + markerX] = 0x00f0f0f0u;
	}
}

void push_video()
{
	if(nullptr == video_cb)
	{
		return;
	}
	std::vector<uint32_t> frame;
	unsigned wid = 0, hei = 0;
	if(true == runtime.CopyFrame(frame, wid, hei) && 0 < wid && 0 < hei)
	{
		video_buffer_index = (video_buffer_index + 1) % video_buffers.size();
		auto &buffer = video_buffers[video_buffer_index];
		buffer = std::move(frame);
		video_cb(buffer.data(), wid, hei, wid * sizeof(uint32_t));
	}
	else
	{
		draw_placeholder_frame();
		video_cb(framebuffer.data(), BASE_WIDTH, BASE_HEIGHT, BASE_WIDTH * sizeof(uint32_t));
	}
}

void push_audio()
{
	std::array<int16_t, AUDIO_FRAMES_PER_RUN * 2> audio{};
	const auto got = runtime.PopAudio(audio.data(), AUDIO_FRAMES_PER_RUN);
	if(got < AUDIO_FRAMES_PER_RUN)
	{
		std::memset(audio.data() + got * 2, 0, (AUDIO_FRAMES_PER_RUN - got) * 2 * sizeof(int16_t));
	}
	if(nullptr != audio_batch_cb)
	{
		audio_batch_cb(audio.data(), AUDIO_FRAMES_PER_RUN);
	}
	else if(nullptr != audio_cb)
	{
		for(size_t i = 0; i < AUDIO_FRAMES_PER_RUN; ++i)
		{
			audio_cb(audio[i * 2], audio[i * 2 + 1]);
		}
	}
}

void poll_input()
{
	// Input polling is now done directly in DevicePolling
}

void ResetMouseTracking()
{
	mouse_pointer_prev_x = {0, 0};
	mouse_pointer_prev_y = {0, 0};
	mouse_pointer_prev_valid = {false, false};
}

void InvalidateSavestateSnapshot()
{
	std::lock_guard<std::mutex> lock(savestate_lock);
	savestate_snapshot.clear();
	savestate_snapshot_size = 0;
	savestate_snapshot_frame = ~0ULL;
	savestate_snapshot_valid = false;
}

bool CaptureSavestateSnapshot()
{
	if(nullptr == runtime.towns || false == runtime.loaded)
	{
		InvalidateSavestateSnapshot();
		return false;
	}

	try
	{
		auto state = runtime.towns->SaveStateMem();
		std::lock_guard<std::mutex> lock(savestate_lock);
		savestate_snapshot = std::move(state);
		savestate_snapshot_size = savestate_snapshot.size();
		savestate_snapshot_frame = frame_counter;
		savestate_snapshot_valid = true;
		return true;
	}
	catch(...)
	{
		InvalidateSavestateSnapshot();
		log(RETRO_LOG_WARN, "Tsugaru libretro: savestate capture failed.\n");
		return false;
	}
}
}

TSUGARU_RETRO_API void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;
	if(nullptr != environ_cb)
	{
		retro_log_callback logging{};
		if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
		{
			log_cb = logging.log;
		}
		system_directory = get_path_from_environment(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
		save_directory = get_path_from_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
	}
	set_environment_defaults();
}

TSUGARU_RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

TSUGARU_RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
	audio_cb = cb;
}

TSUGARU_RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_batch_cb = cb;
}

TSUGARU_RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
	input_poll_cb = cb;
}

TSUGARU_RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
	input_state_cb = cb;
}

TSUGARU_RETRO_API void retro_set_controller_port_device(unsigned, unsigned)
{
}

TSUGARU_RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

TSUGARU_RETRO_API void retro_get_system_info(retro_system_info *info)
{
	if(nullptr == info)
	{
		return;
	}

	std::memset(info, 0, sizeof(*info));
	info->library_name = "Tsugaru";
	info->library_version = "libretro phase3";
	info->valid_extensions = "cue|bin|iso|mds|mdf|ccd|chd|d77|d88|rdd|img|fdi|hdm|h0|m3u";
	info->need_fullpath = true;
	info->block_extract = true;
}

TSUGARU_RETRO_API void retro_get_system_av_info(retro_system_av_info *info)
{
	if(nullptr == info)
	{
		return;
	}

	std::memset(info, 0, sizeof(*info));
	info->geometry.base_width = BASE_WIDTH;
	info->geometry.base_height = BASE_HEIGHT;
	info->geometry.max_width = MAX_WIDTH;
	info->geometry.max_height = MAX_HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = FPS;
	info->timing.sample_rate = SAMPLE_RATE;
}

TSUGARU_RETRO_API void retro_init(void)
{
	frame_counter = 0;
	InvalidateSavestateSnapshot();
	
	// Register keyboard callback
	if (environ_cb)
	{
		retro_keyboard_callback kb_callback;
		kb_callback.callback = retro_keyboard_event;
		environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kb_callback);
	}
	
	log(RETRO_LOG_INFO, "Tsugaru libretro phase 3 initialized.\n");
}

TSUGARU_RETRO_API void retro_deinit(void)
{
	runtime.unload();
	InvalidateSavestateSnapshot();
}

TSUGARU_RETRO_API void retro_reset(void)
{
	runtime.reset();
	InvalidateSavestateSnapshot();
}

TSUGARU_RETRO_API bool retro_load_game(const retro_game_info *game)
{
	frame_counter = 0;
	InvalidateSavestateSnapshot();
	return runtime.load(game);
}

TSUGARU_RETRO_API bool retro_load_game_special(unsigned, const retro_game_info *, size_t)
{
	return false;
}

TSUGARU_RETRO_API void retro_unload_game(void)
{
	runtime.unload();
	frame_counter = 0;
	InvalidateSavestateSnapshot();
}

TSUGARU_RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

TSUGARU_RETRO_API void retro_run(void)
{
	poll_input();
	if(true == runtime.loaded)
	{
		runtime.Advance();
	}
	else
	{
		draw_placeholder_frame();
	}
	push_video();
	push_audio();
	++frame_counter;
}

TSUGARU_RETRO_API size_t retro_serialize_size(void)
{
	if(false == runtime.loaded || nullptr == runtime.towns)
	{
		InvalidateSavestateSnapshot();
		return 0;
	}

	{
		std::lock_guard<std::mutex> lock(savestate_lock);
		if(true == savestate_snapshot_valid && savestate_snapshot_frame == frame_counter)
		{
			return savestate_snapshot_size;
		}
	}

	if(false == CaptureSavestateSnapshot())
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(savestate_lock);
	return savestate_snapshot_size;
}

TSUGARU_RETRO_API bool retro_serialize(void *data, size_t size)
{
	if(false == runtime.loaded || nullptr == runtime.towns || nullptr == data)
	{
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(savestate_lock);
		if(true == savestate_snapshot_valid && savestate_snapshot_frame == frame_counter)
		{
			if(savestate_snapshot_size > size)
			{
				return false;
			}
			std::memcpy(data, savestate_snapshot.data(), savestate_snapshot_size);
			return true;
		}
	}

	if(false == CaptureSavestateSnapshot())
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(savestate_lock);
	if(savestate_snapshot_size > size)
	{
		return false;
	}
	std::memcpy(data, savestate_snapshot.data(), savestate_snapshot_size);
	return true;
}

TSUGARU_RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	if(false == runtime.loaded || nullptr == runtime.towns || nullptr == data)
	{
		return false;
	}

	bool ok = false;
	try
	{
		const auto *bytes = static_cast<const uint8_t *>(data);
		std::vector<uint8_t> state(bytes, bytes + size);
		ok = runtime.towns->LoadStateMem(state);
	}
	catch(...)
	{
		ok = false;
	}
	if(true == ok)
	{
		InvalidateSavestateSnapshot();
	}
	else
	{
		log(RETRO_LOG_WARN, "Tsugaru libretro: savestate load failed.\n");
	}
	return ok;
}

TSUGARU_RETRO_API void retro_cheat_reset(void)
{
}

TSUGARU_RETRO_API void retro_cheat_set(unsigned, bool, const char *)
{
}

TSUGARU_RETRO_API void *retro_get_memory_data(unsigned)
{
	return nullptr;
}

TSUGARU_RETRO_API size_t retro_get_memory_size(unsigned)
{
	return 0;
}
