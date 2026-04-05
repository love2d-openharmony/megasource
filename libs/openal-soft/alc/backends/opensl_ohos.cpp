#include "config.h"

#include "opensl.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif
#include <string>
#include <string_view>

#include "alnumeric.h"
#include "core/device.h"
#include "core/helpers.h"
#include "core/logging.h"
#include "dynload.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_OpenHarmony.h>
#include <SLES/OpenSLES_Platform.h>

namespace {

using namespace std::string_view_literals;

[[nodiscard]] constexpr auto GetDeviceName() noexcept { return "OpenSL"sv; }

#if HAVE_DYNLOAD
#define SLES_SYMBOLS(MAGIC)          \
	MAGIC(slCreateEngine);           \
	MAGIC(SL_IID_ENGINE);            \
	MAGIC(SL_IID_PLAY);              \
	MAGIC(SL_IID_OH_BUFFERQUEUE);

static void *sles_handle = nullptr;

#define MAKE_SYMBOL(f) decltype(f) *p##f
SLES_SYMBOLS(MAKE_SYMBOL)
#undef MAKE_SYMBOL

#ifndef IN_IDE_PARSER
#define slCreateEngine (*pslCreateEngine)
#define SL_IID_ENGINE (*pSL_IID_ENGINE)
#define SL_IID_PLAY (*pSL_IID_PLAY)
#define SL_IID_OH_BUFFERQUEUE (*pSL_IID_OH_BUFFERQUEUE)
#endif
#endif

struct OpenSLPlayback final : public BackendBase {
	explicit OpenSLPlayback(DeviceBase *device) noexcept : BackendBase{device} { }
	~OpenSLPlayback() final;

	void open(std::string_view name) final;
	auto reset() -> bool final;
	void start() final;
	void stop() final;

private:
	static void BufferQueueCallback(SLOHBufferQueueItf bufferQueueItf, void *pContext, SLuint32 size);
	void writeBuffer(SLOHBufferQueueItf bufferQueueItf, SLuint32 size);
	void destroyObjects();

	SLObjectItf mEngineObject{nullptr};
	SLEngineItf mEngineEngine{nullptr};
	SLObjectItf mOutputMixObject{nullptr};
	SLObjectItf mPlayerObject{nullptr};
	SLPlayItf mPlayItf{nullptr};
	SLOHBufferQueueItf mBufferQueueItf{nullptr};

	uint mNumChannels{2};
	uint mFrameSize{0};
	std::atomic<bool> mRunning{false};
};

OpenSLPlayback::~OpenSLPlayback() { destroyObjects(); }

void OpenSLPlayback::destroyObjects()
{
	mRunning.store(false, std::memory_order_release);

	if (mPlayerObject)
	{
		(*mPlayerObject)->Destroy(mPlayerObject);
		mPlayerObject = nullptr;
	}
	mPlayItf = nullptr;
	mBufferQueueItf = nullptr;

	if (mOutputMixObject)
	{
		(*mOutputMixObject)->Destroy(mOutputMixObject);
		mOutputMixObject = nullptr;
	}

	if (mEngineObject)
	{
		(*mEngineObject)->Destroy(mEngineObject);
		mEngineObject = nullptr;
	}
	mEngineEngine = nullptr;
}

void OpenSLPlayback::open(std::string_view name)
{
	if (!name.empty() && name != GetDeviceName())
		throw al::backend_exception{al::backend_error::NoDevice, "No device named {}", name};

	SLresult result = slCreateEngine(&mEngineObject, 0, nullptr, 0, nullptr, nullptr);
	if (result != SL_RESULT_SUCCESS || !mEngineObject)
		throw al::backend_exception{al::backend_error::DeviceError, "slCreateEngine failed ({})", int(result)};

	result = (*mEngineObject)->Realize(mEngineObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		throw al::backend_exception{al::backend_error::DeviceError, "Engine Realize failed ({})", int(result)};

	result = (*mEngineObject)->GetInterface(mEngineObject, SL_IID_ENGINE, &mEngineEngine);
	if (result != SL_RESULT_SUCCESS || !mEngineEngine)
		throw al::backend_exception{al::backend_error::DeviceError, "Engine GetInterface failed ({})", int(result)};

	mDeviceName = std::string{GetDeviceName()};
}

auto OpenSLPlayback::reset() -> bool
{
	if (!mEngineEngine)
		return false;

	if (mPlayerObject)
	{
		(*mPlayerObject)->Destroy(mPlayerObject);
		mPlayerObject = nullptr;
	}
	mPlayItf = nullptr;
	mBufferQueueItf = nullptr;

	if (mOutputMixObject)
	{
		(*mOutputMixObject)->Destroy(mOutputMixObject);
		mOutputMixObject = nullptr;
	}

	mDevice->FmtType = DevFmtShort;
	if (mDevice->FmtChans != DevFmtMono && mDevice->FmtChans != DevFmtStereo)
		mDevice->FmtChans = DevFmtStereo;

	mDevice->mAmbiOrder = 0;
	mNumChannels = (mDevice->FmtChans == DevFmtMono) ? 1u : 2u;
	mFrameSize = mDevice->bytesFromFmt() * mNumChannels;
	setDefaultChannelOrder();

	SLresult result = (*mEngineEngine)->CreateOutputMix(mEngineEngine, &mOutputMixObject, 0, nullptr, nullptr);
	if (result != SL_RESULT_SUCCESS || !mOutputMixObject)
		throw al::backend_exception{al::backend_error::DeviceError, "CreateOutputMix failed ({})", int(result)};

	result = (*mOutputMixObject)->Realize(mOutputMixObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		throw al::backend_exception{al::backend_error::DeviceError, "OutputMix Realize failed ({})", int(result)};

	SLDataLocator_OutputMix outmixloc = {SL_DATALOCATOR_OUTPUTMIX, mOutputMixObject};
	SLDataSink sink = {&outmixloc, nullptr};

	SLDataLocator_BufferQueue bufferQueue = {SL_DATALOCATOR_BUFFERQUEUE, 2};

	const SLuint32 sampleRate = static_cast<SLuint32>(mDevice->mSampleRate * 1000u);
	SLuint32 channelMask = (mNumChannels == 1) ? SL_SPEAKER_FRONT_CENTER
		: (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
	SLDataFormat_PCM pcm = {
		SL_DATAFORMAT_PCM,
		static_cast<SLuint32>(mNumChannels),
		sampleRate,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		SL_PCMSAMPLEFORMAT_FIXED_16,
		channelMask,
		SL_BYTEORDER_LITTLEENDIAN,
	};

	SLDataSource source = {&bufferQueue, &pcm};

	const SLInterfaceID ids[] = {SL_IID_OH_BUFFERQUEUE, SL_IID_PLAY};
	const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
	result = (*mEngineEngine)->CreateAudioPlayer(mEngineEngine, &mPlayerObject, &source, &sink,
		2, ids, req);
	if (result != SL_RESULT_SUCCESS || !mPlayerObject)
		throw al::backend_exception{al::backend_error::DeviceError, "CreateAudioPlayer failed ({})", int(result)};

	result = (*mPlayerObject)->Realize(mPlayerObject, SL_BOOLEAN_FALSE);
	if (result != SL_RESULT_SUCCESS)
		throw al::backend_exception{al::backend_error::DeviceError, "Player Realize failed ({})", int(result)};

	result = (*mPlayerObject)->GetInterface(mPlayerObject, SL_IID_PLAY, &mPlayItf);
	if (result != SL_RESULT_SUCCESS || !mPlayItf)
		throw al::backend_exception{al::backend_error::DeviceError, "Player GetInterface(PLAY) failed ({})", int(result)};

	result = (*mPlayerObject)->GetInterface(mPlayerObject, SL_IID_OH_BUFFERQUEUE, &mBufferQueueItf);
	if (result != SL_RESULT_SUCCESS || !mBufferQueueItf)
		throw al::backend_exception{al::backend_error::DeviceError, "Player GetInterface(OH_BUFFERQUEUE) failed ({})", int(result)};

	result = (*mBufferQueueItf)->RegisterCallback(mBufferQueueItf, &OpenSLPlayback::BufferQueueCallback, this);
	if (result != SL_RESULT_SUCCESS)
		throw al::backend_exception{al::backend_error::DeviceError, "BufferQueue RegisterCallback failed ({})", int(result)};

	if (mDevice->mUpdateSize < 256)
		mDevice->mUpdateSize = 1024;
	if (mDevice->mBufferSize < mDevice->mUpdateSize * 2)
		mDevice->mBufferSize = mDevice->mUpdateSize * 2;

	return true;
}

void OpenSLPlayback::BufferQueueCallback(SLOHBufferQueueItf bufferQueueItf, void *pContext, SLuint32 size)
{
	auto *self = static_cast<OpenSLPlayback *>(pContext);
	if (!self)
		return;
	self->writeBuffer(bufferQueueItf, size);
}

void OpenSLPlayback::writeBuffer(SLOHBufferQueueItf bufferQueueItf, SLuint32 /*size*/)
{
	if (!mRunning.load(std::memory_order_acquire) || !bufferQueueItf)
		return;

	SLuint8 *buffer = nullptr;
	SLuint32 bufferSize = 0;
	SLresult result = (*bufferQueueItf)->GetBuffer(bufferQueueItf, &buffer, &bufferSize);
	if (result != SL_RESULT_SUCCESS || !buffer || bufferSize == 0)
		return;

	const uint frames = static_cast<uint>(bufferSize / mFrameSize);
	mDevice->renderSamples(buffer, frames, mNumChannels);

	(*bufferQueueItf)->Enqueue(bufferQueueItf, buffer, bufferSize);
}

void OpenSLPlayback::start()
{
	if (!mPlayItf || !mBufferQueueItf)
		return;

	mRunning.store(true, std::memory_order_release);

	(*mPlayItf)->SetPlayState(mPlayItf, SL_PLAYSTATE_PLAYING);

	for (int i = 0; i < 2; i++)
		writeBuffer(mBufferQueueItf, 0);
}

void OpenSLPlayback::stop()
{
	mRunning.store(false, std::memory_order_release);
	if (mPlayItf)
		(*mPlayItf)->SetPlayState(mPlayItf, SL_PLAYSTATE_STOPPED);
	if (mBufferQueueItf)
		(*mBufferQueueItf)->Clear(mBufferQueueItf);
}

} // namespace

bool OSLBackendFactory::init()
{
#if HAVE_DYNLOAD
	if(!sles_handle)
	{
#if defined(HAVE_DLFCN_H)
		static constexpr const char *libs[] = {"libOpenSLES.so", "libOpenSLES.z.so"};
		for (auto *lib : libs)
		{
			dlerror();
			void *h = dlopen(lib, RTLD_NOW);
			const char *err = dlerror();
			if (h)
			{
				sles_handle = h;
				std::fprintf(stderr, "[ALSOFT][OpenSL] dlopen %s ok\n", lib);
				break;
			}
			std::fprintf(stderr, "[ALSOFT][OpenSL] dlopen %s failed: %s\n", lib, err ? err : "(null)");
		}
#else
		sles_handle = LoadLib("libOpenSLES.so");
#endif
		if(!sles_handle)
			return false;

		std::string missing_syms;
#define LOAD_SYMBOL(f) do {                                               \
	p##f = reinterpret_cast<decltype(p##f)>(GetSymbol(sles_handle, #f));   \
	if(p##f == nullptr) missing_syms += "\n" #f;                           \
} while(0)
		SLES_SYMBOLS(LOAD_SYMBOL)
#undef LOAD_SYMBOL

		if(!missing_syms.empty())
		{
			std::fprintf(stderr, "[ALSOFT][OpenSL] missing expected symbols:%s\n", missing_syms.c_str());
			CloseLib(sles_handle);
			sles_handle = nullptr;
			return false;
		}

		std::fprintf(stderr, "[ALSOFT][OpenSL] init ok\n");
	}
#endif
	return true;
}

bool OSLBackendFactory::querySupport(BackendType type) { return type == BackendType::Playback; }

auto OSLBackendFactory::enumerate(BackendType type) -> std::vector<std::string>
{
	if (type == BackendType::Playback)
		return std::vector{std::string{GetDeviceName()}};
	return {};
}

BackendPtr OSLBackendFactory::createBackend(DeviceBase *device, BackendType type)
{
	if (type == BackendType::Playback)
		return BackendPtr{new OpenSLPlayback{device}};
	return nullptr;
}

BackendFactory &OSLBackendFactory::getFactory()
{
	static OSLBackendFactory factory{};
	return factory;
}
