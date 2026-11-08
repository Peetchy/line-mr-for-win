// lineMR_audio.exe: audio work for voice message transcription, outside LINE and NVDA.
//
// A separate process so that a failure in the audio stack can never take LINE or NVDA
// with it. Output is UTF-8, one record per line ("<kind>\t<value>"), read by the add-on's
// appModules/line/transcribe.py.
//
//   lineMR_audio capture <pid> <out.wav> <max seconds> [<silence seconds> [<sound seconds>]]
//       Records what the process tree of <pid> plays (WASAPI process loopback, Windows 10 2004+)
//       as 16 kHz mono 16-bit PCM, written to disk as it arrives (no size limit in memory).
//       Prints "ready" once recording runs (the add-on then starts playback), "level\t<s>" every
//       second, and "done\t<seconds>\t<heard>" at the end. Stops after <max seconds>, after
//       <silence seconds> of silence once sound was heard for at least <sound seconds> (a voice
//       message of known length is not cut at a pause), or on a line "stop" on stdin. Silence after
//       the last sound is cut off (speech recognizers make up words in long silence).
//   lineMR_audio decode <in> <out.wav>
//       Decodes an audio file (LINE caches played voice messages as AAC .m4a) to the same WAV format
//       with Media Foundation; "done\t<seconds>".
//   lineMR_audio split <in.wav> <dir> <max seconds>
//       Cuts such a WAV file at its quietest moments into pieces of at most <max seconds> (Whisper
//       hears 30 s at a time); "chunk\t<file name>\t<start seconds>" per piece, "done\t<pieces>".
// Errors: "error\t<code>\t<detail>" and exit code 1. Codes: usage, com, process, activate,
// format, file.

#include <windows.h>
#include <io.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audioclientactivationparams.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/implements.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

constexpr DWORD kSampleRate = 16000;

std::string utf8(const std::wstring &s)
{
	if (s.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
	std::string out(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
	return out;
}

void emit(const std::string &line)
{
	fwrite(line.data(), 1, line.size(), stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

int fail(const char *code, const std::string &detail)
{
	std::string clean = detail;
	for (char &c : clean) {
		if (c == '\n' || c == '\r' || c == '\t')
			c = ' ';
	}
	emit(std::string("error\t") + code + "\t" + clean);
	return 1;
}

std::string hr(const char *what, HRESULT h)
{
	char buf[96];
	sprintf_s(buf, "%s failed (0x%08lX)", what, static_cast<unsigned long>(h));
	return buf;
}

// --- capture ---------------------------------------------------------------------------------

class ActivationHandler : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler>
{
public:
	ActivationHandler() { done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
	~ActivationHandler() override { CloseHandle(done); }
	STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override
	{
		HRESULT activated = E_FAIL;
		ComPtr<IUnknown> unknown;
		result = op->GetActivateResult(&activated, &unknown);
		if (SUCCEEDED(result))
			result = activated;
		if (SUCCEEDED(result))
			result = unknown.As(&client);
		SetEvent(done);
		return S_OK;
	}
	HANDLE done;
	HRESULT result = E_FAIL;
	ComPtr<IAudioClient> client;
};

// Streams 16-bit PCM to a WAV file; the sizes in the header are written at the end.
class WavWriter
{
public:
	bool open(const std::wstring &path)
	{
		if (_wfopen_s(&m_file, path.c_str(), L"wb") != 0 || !m_file)
			return false;
		writeHeader(0);
		return true;
	}
	void write(const int16_t *samples, size_t count)
	{
		fwrite(samples, sizeof(int16_t), count, m_file);
		m_bytes += uint32_t(count * sizeof(int16_t));
	}
	// Keeps the first <samples> samples (all if fewer were written).
	void close(uint64_t samples = UINT64_MAX)
	{
		if (!m_file)
			return;
		if (samples < m_bytes / sizeof(int16_t)) {
			m_bytes = uint32_t(samples * sizeof(int16_t));
			fflush(m_file);
			_chsize_s(_fileno(m_file), 44 + int64_t(m_bytes));
		}
		fseek(m_file, 0, SEEK_SET);
		writeHeader(m_bytes);
		fclose(m_file);
		m_file = nullptr;
	}
	~WavWriter() { close(); }

private:
	void writeHeader(uint32_t data)
	{
		auto u32 = [this](uint32_t v) { fwrite(&v, 4, 1, m_file); };
		auto u16 = [this](uint16_t v) { fwrite(&v, 2, 1, m_file); };
		fwrite("RIFF", 1, 4, m_file);
		u32(36 + data);
		fwrite("WAVEfmt ", 1, 8, m_file);
		u32(16);
		u16(1); // PCM
		u16(1); // mono
		u32(kSampleRate);
		u32(kSampleRate * 2);
		u16(2);
		u16(16);
		fwrite("data", 1, 4, m_file);
		u32(data);
	}
	FILE *m_file = nullptr;
	uint32_t m_bytes = 0;
};

int capture(DWORD pid, const std::wstring &out, double maxSeconds, double silenceSeconds, double soundSeconds)
{
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!process)
		return fail("process", "no such process");
	CloseHandle(process);

	AUDIOCLIENT_ACTIVATION_PARAMS params = {};
	params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	params.ProcessLoopbackParams.TargetProcessId = pid;
	params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
	PROPVARIANT prop = {};
	prop.vt = VT_BLOB;
	prop.blob.cbSize = sizeof(params);
	prop.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

	ComPtr<ActivationHandler> handler = Make<ActivationHandler>();
	ComPtr<IActivateAudioInterfaceAsyncOperation> op;
	HRESULT h = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &prop,
	                                        handler.Get(), &op);
	if (FAILED(h))
		return fail("activate", hr("ActivateAudioInterfaceAsync", h));
	if (WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0)
		return fail("activate", "activation timed out");
	if (FAILED(handler->result))
		return fail("activate", hr("process loopback activation", handler->result));
	IAudioClient *client = handler->client.Get();

	// The process loopback client takes the format it is given (it has no mix format) and
	// converts to it.
	WAVEFORMATEX format = {};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 1;
	format.nSamplesPerSec = kSampleRate;
	format.wBitsPerSample = 16;
	format.nBlockAlign = 2;
	format.nAvgBytesPerSec = kSampleRate * 2;
	h = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
	                       AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
	                           | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
	                       200 * 10000, 0, &format, nullptr);
	if (FAILED(h))
		return fail("format", hr("IAudioClient::Initialize", h));
	HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	client->SetEventHandle(ready);
	ComPtr<IAudioCaptureClient> captureClient;
	h = client->GetService(IID_PPV_ARGS(&captureClient));
	if (FAILED(h))
		return fail("activate", hr("GetService", h));

	WavWriter wav;
	if (!wav.open(out))
		return fail("file", "cannot write the recording");

	// A line "stop" on stdin ends the recording early (an empty or closed stdin does not).
	std::atomic<bool> stop{ false };
	std::thread([&stop] {
		char line[64];
		while (fgets(line, sizeof line, stdin)) {
			if (strncmp(line, "stop", 4) == 0) {
				stop = true;
				break;
			}
		}
	}).detach();

	h = client->Start();
	if (FAILED(h))
		return fail("activate", hr("IAudioClient::Start", h));
	emit("ready");

	const uint64_t maxSamples = uint64_t(maxSeconds * kSampleRate);
	const uint64_t silenceSamples = uint64_t(silenceSeconds * kSampleRate);
	uint64_t total = 0;
	uint64_t firstSound = 0;
	uint64_t lastSound = 0;
	bool heard = false;
	uint64_t nextLevel = kSampleRate;
	std::vector<int16_t> zeros;
	ULONGLONG lastPacket = GetTickCount64();
	while (!stop && total < maxSamples) {
		WaitForSingleObject(ready, 200);
		UINT32 packet = 0;
		while (SUCCEEDED(captureClient->GetNextPacketSize(&packet)) && packet > 0) {
			BYTE *data = nullptr;
			UINT32 frames = 0;
			DWORD flags = 0;
			if (FAILED(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
				break;
			const int16_t *samples = reinterpret_cast<const int16_t *>(data);
			if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
				zeros.assign(frames, 0);
				samples = zeros.data();
			}
			wav.write(samples, frames);
			for (UINT32 i = 0; i < frames; ++i) {
				if (std::abs(int(samples[i])) > 300) { // about -40 dBFS
					if (!heard)
						firstSound = total + i;
					heard = true;
					lastSound = total + i;
				}
			}
			total += frames;
			captureClient->ReleaseBuffer(frames);
			lastPacket = GetTickCount64();
		}
		// While nothing plays, the loopback client delivers no packets: time counts anyway, as
		// silence (the recording keeps the real time line).
		const ULONGLONG now = GetTickCount64();
		if (now - lastPacket > 200) { // > 200 ms without any packet
			const uint64_t gap = (now - lastPacket) * kSampleRate / 1000;
			lastPacket = now;
			zeros.assign(size_t(gap), 0);
			wav.write(zeros.data(), zeros.size());
			total += gap;
		}
		if (total >= nextLevel) {
			emit("level\t" + std::to_string(total / kSampleRate));
			nextLevel += kSampleRate;
		}
		if (heard && silenceSamples && total - lastSound > silenceSamples
		    && lastSound - firstSound >= uint64_t(soundSeconds * kSampleRate))
			break;
	}
	client->Stop();
	// Up to half a second after the last sound.
	wav.close(heard ? lastSound + kSampleRate / 2 : total);
	char buf[64];
	sprintf_s(buf, "done\t%.1f\t%d", double(total) / kSampleRate, heard ? 1 : 0);
	emit(buf);
	return 0;
}

// --- decode ------------------------------------------------------------------------------------

// Decodes an audio file Windows can read (LINE's voice messages are AAC in M4A) to 16 kHz mono
// 16-bit PCM WAV with Media Foundation's source reader (it converts and resamples). Prints
// "done\t<seconds>".
int decode(const std::wstring &in, const std::wstring &out)
{
	HRESULT h = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(h))
		return fail("com", hr("MFStartup", h));
	struct Shutdown {
		~Shutdown() { MFShutdown(); }
	} shutdown;
	ComPtr<IMFSourceReader> reader;
	h = MFCreateSourceReaderFromURL(in.c_str(), nullptr, &reader);
	if (FAILED(h))
		return fail("format", hr("MFCreateSourceReaderFromURL", h));
	reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
	reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
	ComPtr<IMFMediaType> pcm;
	MFCreateMediaType(&pcm);
	pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
	pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
	pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2);
	pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSampleRate * 2);
	h = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm.Get());
	if (FAILED(h))
		return fail("format", hr("SetCurrentMediaType", h));
	WavWriter wav;
	if (!wav.open(out))
		return fail("file", "cannot write the recording");
	uint64_t total = 0;
	for (;;) {
		DWORD flags = 0;
		ComPtr<IMFSample> sample;
		h = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample);
		if (FAILED(h))
			return fail("format", hr("ReadSample", h));
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
			break;
		if (!sample)
			continue;
		ComPtr<IMFMediaBuffer> buffer;
		if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
			continue;
		BYTE *data = nullptr;
		DWORD length = 0;
		if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
			wav.write(reinterpret_cast<const int16_t *>(data), length / 2);
			total += length / 2;
			buffer->Unlock();
		}
	}
	wav.close();
	char buf[64];
	sprintf_s(buf, "done\t%.1f", double(total) / kSampleRate);
	emit(buf);
	return 0;
}

// --- split -------------------------------------------------------------------------------------

// Cuts a recording of this tool's format (16 kHz mono 16-bit WAV) into pieces of at most <max seconds>
// for Whisper, which hears 30 s at a time and cut words at its own window ends (repeated or lost
// words there). Each cut is at the quietest 300 ms in the last 10 s of the piece: a pause, or the
// gap between two words when nobody pauses. Writes <dir>\chunk-0001.wav ...; prints
// "chunk\t<file name>\t<start seconds>" per piece and "done\t<pieces>".
int split(const std::wstring &in, const std::wstring &dir, double maxSeconds)
{
	FILE *file = nullptr;
	if (_wfopen_s(&file, in.c_str(), L"rb") != 0 || !file)
		return fail("file", "cannot read the recording");
	std::vector<int16_t> samples;
	char id[4];
	uint32_t size = 0;
	bool data = false;
	if (fread(id, 1, 4, file) == 4 && memcmp(id, "RIFF", 4) == 0 && fread(&size, 4, 1, file) == 1
	    && fread(id, 1, 4, file) == 4 && memcmp(id, "WAVE", 4) == 0) {
		while (fread(id, 1, 4, file) == 4 && fread(&size, 4, 1, file) == 1) {
			if (memcmp(id, "data", 4) == 0) {
				samples.resize(size / 2);
				samples.resize(fread(samples.data(), 2, samples.size(), file));
				data = true;
				break;
			}
			fseek(file, long(size + (size & 1)), SEEK_CUR);
		}
	}
	fclose(file);
	if (!data)
		return fail("format", "not a WAV recording");

	constexpr size_t kFrame = kSampleRate / 50; // 20 ms
	constexpr size_t kQuiet = 15;              // frames: 300 ms
	const size_t frames = samples.size() / kFrame;
	std::vector<double> energy(frames + 1, 0.0); // running sums of the frames' energy
	for (size_t f = 0; f < frames; ++f) {
		double e = 0;
		for (size_t i = f * kFrame; i < (f + 1) * kFrame; ++i)
			e += double(samples[i]) * samples[i];
		energy[f + 1] = energy[f] + e;
	}
	const size_t maxFrames = size_t(std::max(5.0, maxSeconds) * 50);
	const size_t minFrames = maxFrames > 500 + kQuiet ? maxFrames - 500 : maxFrames / 2;
	std::vector<size_t> cuts; // frame numbers
	size_t start = 0;
	while (frames - start > maxFrames) {
		size_t best = start + maxFrames - kQuiet;
		double bestEnergy = -1;
		for (size_t f = start + minFrames; f + kQuiet <= start + maxFrames; ++f) {
			const double e = energy[f + kQuiet] - energy[f];
			if (bestEnergy < 0 || e < bestEnergy) {
				bestEnergy = e;
				best = f;
			}
		}
		start = best + kQuiet / 2;
		cuts.push_back(start);
	}
	cuts.push_back(frames + 1); // the rest, with the samples after the last whole frame

	size_t from = 0;
	int count = 0;
	for (size_t cut : cuts) {
		const size_t first = from * kFrame;
		const size_t last = std::min(samples.size(), cut * kFrame);
		wchar_t name[32];
		swprintf_s(name, L"chunk-%04d.wav", ++count);
		WavWriter wav;
		if (!wav.open(dir + L"\\" + name))
			return fail("file", "cannot write a piece of the recording");
		wav.write(samples.data() + first, last - first);
		wav.close();
		char buf[96];
		sprintf_s(buf, "chunk\t%s\t%.2f", utf8(name).c_str(), double(first) / kSampleRate);
		emit(buf);
		from = cut;
	}
	char buf[32];
	sprintf_s(buf, "done\t%d", count);
	emit(buf);
	return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	// Phrases are passed as UTF-8 bytes; keep the C runtime from translating them.
	setvbuf(stdout, nullptr, _IOFBF, 1 << 14);
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return fail("com", "CoInitializeEx failed");
	const std::wstring command = argc > 1 ? argv[1] : L"";
	int code = 0;
	if (command == L"decode" && argc >= 4)
		code = decode(argv[2], argv[3]);
	else if (command == L"split" && argc >= 5)
		code = split(argv[2], argv[3], _wtof(argv[4]));
	else if (command == L"capture" && argc >= 5)
		code = capture(DWORD(_wtoi(argv[2])), argv[3], _wtof(argv[4]), argc >= 6 ? _wtof(argv[5]) : 0,
		               argc >= 7 ? _wtof(argv[6]) : 0);
	else
		code = fail("usage", "lineMR_audio capture <pid> <out.wav> <max s> [<silence s> [<sound s>]] | decode <in> <out.wav> | split <in.wav> <dir> <max s>");
	CoUninitialize();
	return code;
}
