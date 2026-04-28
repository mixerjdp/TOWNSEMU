#include "libretro.h"

#include <array>
#include <cctype>
#include <cstdint>
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

namespace
{
constexpr unsigned BASE_WIDTH = 640;
constexpr unsigned BASE_HEIGHT = 480;
constexpr unsigned MAX_WIDTH = 1024;
constexpr unsigned MAX_HEIGHT = 1024;
constexpr double FPS = 60.0;
constexpr double SAMPLE_RATE = 44100.0;
constexpr size_t AUDIO_FRAMES_PER_RUN = static_cast<size_t>(SAMPLE_RATE / FPS);

retro_environment_t environ_cb = nullptr;
retro_video_refresh_t video_cb = nullptr;
retro_audio_sample_t audio_cb = nullptr;
retro_audio_sample_batch_t audio_batch_cb = nullptr;
retro_input_poll_t input_poll_cb = nullptr;
retro_input_state_t input_state_cb = nullptr;
retro_log_printf_t log_cb = nullptr;

std::array<uint32_t, BASE_WIDTH * BASE_HEIGHT> framebuffer{};
std::string content_path;
std::string system_directory;
std::string save_directory;
uint64_t frame_counter = 0;

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

bool is_cd_extension(const std::string &ext)
{
	return "cue" == ext || "bin" == ext || "iso" == ext || "mds" == ext ||
	       "mdf" == ext || "ccd" == ext || "chd" == ext;
}

bool is_floppy_extension(const std::string &ext)
{
	return "d77" == ext || "d88" == ext || "rdd" == ext ||
	       "img" == ext || "fdi" == ext;
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

class LibretroWindow : public Outside_World::WindowInterface
{
public:
	std::mutex frameLock;
	std::vector<uint32_t> xrgb;
	unsigned width = BASE_WIDTH;
	unsigned height = BASE_HEIGHT;
	bool haveFrame = false;

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
		CachedInput input;
		{
			std::lock_guard<std::mutex> lock(input_lock);
			input = cached_input;
		}
		towns.SetGamePadState(0, input.a, input.b, input.left, input.right, input.up, input.down, input.run, input.pause, false);
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
	std::thread vmThread;
	bool loaded = false;
	bool contentIsCD = false;
	bool contentIsFD = false;

	~Runtime()
	{
		unload();
	}

	bool load(const retro_game_info *game)
	{
		unload();
		content_path = (nullptr != game && nullptr != game->path) ? game->path : "";

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
		argv.gamePort[0] = TOWNS_GAMEPORTEMU_PHYSICAL0;
		argv.gamePort[1] = TOWNS_GAMEPORTEMU_MOUSE;
		argv.specialPath.push_back({"${system}", system_directory});
		argv.specialPath.push_back({"${save}", PreferredSavePath()});

		const auto ext = lower_extension(content_path);
		if(true == is_cd_extension(ext))
		{
			argv.cdImgFName = content_path;
			argv.bootKeyComb = BOOT_KEYCOMB_CD;
			argv.townsType = TOWNSTYPE_2_MX;
			argv.memSizeInMB = 16;
			argv.useFPU = true;
			contentIsCD = true;
		}
		else if(true == is_floppy_extension(ext) || true != content_path.empty())
		{
			argv.fdImgFName[0] = content_path;
			argv.bootKeyComb = BOOT_KEYCOMB_F0;
			contentIsFD = true;
		}

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
		window->Start();
		loaded = true;
		vmThread = std::thread([this]()
		{
			townsThread->VMStart(towns.get(), outside.get(), uiThread.get());
			townsThread->VMMainLoop(towns.get(), outside.get(), sound, window, uiThread.get());
			townsThread->VMEnd(towns.get(), outside.get(), uiThread.get());
		});
		return true;
	}

	void unload()
	{
		if(nullptr != townsThread)
		{
			townsThread->SetRunMode(TownsThread::RUNMODE_EXIT);
		}
		if(vmThread.joinable())
		{
			vmThread.join();
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
		contentIsCD = false;
		contentIsFD = false;
	}

	bool CopyFrame(std::vector<uint32_t> &out,unsigned &wid,unsigned &hei)
	{
		if(nullptr != outside && nullptr != outside->window)
		{
			return outside->window->CopyFrame(out, wid, hei);
		}
		return false;
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
		if(true == contentIsCD)
		{
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_TYPE)] = 8;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_UNIT)] = 0;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_BOOT_DEV)] = 0x80;
		}
		else if(true == contentIsFD)
		{
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_TYPE)] = 2;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_DEF_BOOT_DEV_UNIT)] = 0;
			towns->physMem.state.CMOSRAM[cmos_index_from_io(TOWNSIO_CMOS_BOOT_DEV)] = 0x20;
		}
	}
};

Runtime runtime;

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
		video_cb(frame.data(), wid, hei, wid * sizeof(uint32_t));
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
	if(nullptr != input_poll_cb)
	{
		input_poll_cb();
	}
	if(nullptr != input_state_cb)
	{
		CachedInput next;
		next.up = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
		next.down = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
		next.left = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
		next.right = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
		next.a = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
		next.b = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
		next.run = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
		next.pause = 0 != input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
		std::lock_guard<std::mutex> lock(input_lock);
		cached_input = next;
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
	log(RETRO_LOG_INFO, "Tsugaru libretro phase 3 initialized.\n");
}

TSUGARU_RETRO_API void retro_deinit(void)
{
	runtime.unload();
}

TSUGARU_RETRO_API void retro_reset(void)
{
	runtime.unload();
	retro_game_info game{};
	game.path = content_path.empty() ? nullptr : content_path.c_str();
	runtime.load(&game);
}

TSUGARU_RETRO_API bool retro_load_game(const retro_game_info *game)
{
	frame_counter = 0;
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
}

TSUGARU_RETRO_API unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

TSUGARU_RETRO_API void retro_run(void)
{
	poll_input();
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
