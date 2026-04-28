#include "libretro.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define TSUGARU_RETRO_API extern "C" __declspec(dllexport)
#else
#define TSUGARU_RETRO_API extern "C" __attribute__((visibility("default")))
#endif

namespace
{
constexpr unsigned BASE_WIDTH = 640;
constexpr unsigned BASE_HEIGHT = 480;
constexpr unsigned MAX_WIDTH = 1024;
constexpr unsigned MAX_HEIGHT = 1024;
constexpr double FPS = 60.0;
constexpr double SAMPLE_RATE = 44100.0;
constexpr size_t AUDIO_FRAMES_PER_RUN = 735;

retro_environment_t environ_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t audio_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t input_poll_cb = nullptr;
retro_input_state_t input_state_cb = nullptr;
retro_log_printf_t log_cb = nullptr;

std::array<uint32_t, BASE_WIDTH * BASE_HEIGHT> framebuffer{};
std::array<int16_t, AUDIO_FRAMES_PER_RUN * 2> silence{};
std::string content_path;
uint64_t frame_counter = 0;

void log(enum retro_log_level level, const char *message)
{
	if(nullptr != log_cb)
	{
		log_cb(level, "%s", message);
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

	static const retro_variable variables[] =
	{
		{ "tsugaru_model", "FM Towns Model; auto|MODEL2|2F|20F|UX|HR|MX|MARTY" },
		{ "tsugaru_ram_mb", "RAM Size; 6|4|8|10|12|16" },
		{ "tsugaru_mouse_mode", "Mouse Mode; relative|integrated" },
		{ nullptr, nullptr },
	};
	environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable *>(variables));
}

void draw_placeholder_frame()
{
	const uint32_t background = content_path.empty() ? 0x00101820u : 0x00102018u;
	framebuffer.fill(background);

	const unsigned markerX = static_cast<unsigned>((frame_counter * 3) % BASE_WIDTH);
	for(unsigned y = 0; y < BASE_HEIGHT; ++y)
	{
		framebuffer[(y * BASE_WIDTH) + markerX] = 0x00f0f0f0u;
	}

	const unsigned markerY = static_cast<unsigned>((frame_counter * 2) % BASE_HEIGHT);
	for(unsigned x = 0; x < BASE_WIDTH; ++x)
	{
		framebuffer[(markerY * BASE_WIDTH) + x] = 0x003080c0u;
	}
}

void push_video()
{
	if(nullptr != video_cb)
	{
		video_cb(framebuffer.data(), BASE_WIDTH, BASE_HEIGHT, BASE_WIDTH * sizeof(uint32_t));
	}
}

void push_audio()
{
	if(nullptr != audio_batch_cb)
	{
		audio_batch_cb(silence.data(), AUDIO_FRAMES_PER_RUN);
	}
	else if(nullptr != audio_cb)
	{
		for(size_t i = 0; i < AUDIO_FRAMES_PER_RUN; ++i)
		{
			audio_cb(0, 0);
		}
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
	info->library_version = "libretro phase1";
	info->valid_extensions = "cue|bin|iso|mds|mdf|ccd|chd|d77|d88|rdd|img|fdi";
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
	log(RETRO_LOG_INFO, "Tsugaru libretro phase 1 initialized.\n");
}

TSUGARU_RETRO_API void retro_deinit(void)
{
	content_path.clear();
	frame_counter = 0;
}

TSUGARU_RETRO_API void retro_reset(void)
{
	frame_counter = 0;
}

TSUGARU_RETRO_API bool retro_load_game(const retro_game_info *game)
{
	content_path = (nullptr != game && nullptr != game->path) ? game->path : "";
	frame_counter = 0;
	return true;
}

TSUGARU_RETRO_API bool retro_load_game_special(unsigned, const retro_game_info *, size_t)
{
	return false;
}

TSUGARU_RETRO_API void retro_unload_game(void)
{
	content_path.clear();
	frame_counter = 0;
}

TSUGARU_RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

TSUGARU_RETRO_API void retro_run(void)
{
	if(nullptr != input_poll_cb)
	{
		input_poll_cb();
	}
	(void)input_state_cb;

	draw_placeholder_frame();
	push_video();
	push_audio();
	++frame_counter;
}

TSUGARU_RETRO_API size_t retro_serialize_size(void)
{
	return 0;
}

TSUGARU_RETRO_API bool retro_serialize(void *, size_t)
{
	return false;
}

TSUGARU_RETRO_API bool retro_unserialize(const void *, size_t)
{
	return false;
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
