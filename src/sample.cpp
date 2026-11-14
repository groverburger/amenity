#include "sample.h"

#include <algorithm>
#include <filesystem>

#include "miniaudio.h"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

void finalizeSample(Sample& s) {
    s.frames = s.stereo.size() / 2;
    s.mono.resize(s.frames);
    for (size_t i = 0; i < s.frames; ++i) {
        s.mono[i] = 0.5f * (s.stereo[i * 2] + s.stereo[i * 2 + 1]);
    }

    const size_t blocks = (s.frames + Sample::kPeakBlock - 1) / Sample::kPeakBlock;
    s.peakMin.assign(blocks, 0.0f);
    s.peakMax.assign(blocks, 0.0f);
    for (size_t b = 0; b < blocks; ++b) {
        const size_t first = b * Sample::kPeakBlock;
        const size_t last = std::min(first + Sample::kPeakBlock, s.frames);
        const auto [lo, hi] = std::minmax_element(s.mono.begin() + first, s.mono.begin() + last);
        s.peakMin[b] = *lo;
        s.peakMax[b] = *hi;
    }
}

namespace {

// Handles WAV, MP3 and FLAC.
bool decodeWithMiniaudio(const std::string& path, uint32_t rate, std::vector<float>& out) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, rate);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) return false;

    constexpr ma_uint64 kChunk = 65536;
    out.clear();
    ma_result result = MA_SUCCESS;
    for (;;) {
        const size_t used = out.size();
        out.resize(used + kChunk * 2);
        ma_uint64 got = 0;
        result = ma_decoder_read_pcm_frames(&decoder, out.data() + used, kChunk, &got);
        out.resize(used + got * 2);
        if (got == 0 || result != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&decoder);
    return result == MA_SUCCESS || result == MA_AT_END;
}

#if defined(__APPLE__)
// Handles what miniaudio doesn't, such as M4A/AAC and AIFF.
bool decodeWithCoreAudio(const std::string& path, uint32_t rate, std::vector<float>& out) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()),
        static_cast<CFIndex>(path.size()), false);
    if (!url) return false;
    ExtAudioFileRef file = nullptr;
    const OSStatus opened = ExtAudioFileOpenURL(url, &file);
    CFRelease(url);
    if (opened != noErr) return false;

    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;

    bool ok = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                                      sizeof(format), &format) == noErr;
    constexpr UInt32 kChunk = 65536;
    out.clear();
    while (ok) {
        const size_t used = out.size();
        out.resize(used + kChunk * 2);
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = 2;
        list.mBuffers[0].mDataByteSize = kChunk * 8;
        list.mBuffers[0].mData = out.data() + used;
        UInt32 got = kChunk;
        ok = ExtAudioFileRead(file, &got, &list) == noErr;
        out.resize(used + (ok ? size_t{got} * 2 : 0));
        if (got == 0) break;
    }
    ExtAudioFileDispose(file);
    return ok;
}
#elif defined(_WIN32)
// Handles what miniaudio doesn't, such as M4A/AAC and WMA, with Media Foundation.
bool decodeWithMediaFoundation(const std::string& path, uint32_t rate, std::vector<float>& out) {
    // Paths are UTF-8 (the app's manifest sets that code page).
    const int wideLength = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wideLength <= 0) return false;
    std::wstring widePath(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.data(), wideLength);

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        if (SUCCEEDED(com)) CoUninitialize();
        return false;
    }

    // Decode to 32-bit float at the file's own rate and channel count, then convert below.
    std::vector<float> decoded;
    UINT32 channels = 0;
    UINT32 fileRate = 0;
    bool ok = false;
    IMFSourceReader* reader = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(widePath.c_str(), nullptr, &reader))) {
        const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(stream, TRUE);
        IMFMediaType* request = nullptr;
        if (SUCCEEDED(MFCreateMediaType(&request))) {
            request->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            request->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            ok = SUCCEEDED(reader->SetCurrentMediaType(stream, nullptr, request));
            request->Release();
        }
        IMFMediaType* actual = nullptr;
        if (ok && SUCCEEDED(reader->GetCurrentMediaType(stream, &actual))) {
            actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &fileRate);
            actual->Release();
        }
        ok = ok && channels > 0 && fileRate > 0;
        while (ok) {
            DWORD flags = 0;
            IMFSample* sample = nullptr;
            if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample))) {
                ok = false;
                break;
            }
            if (sample) {
                IMFMediaBuffer* buffer = nullptr;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer))) {
                    BYTE* data = nullptr;
                    DWORD bytes = 0;
                    if (SUCCEEDED(buffer->Lock(&data, nullptr, &bytes))) {
                        const auto* floats = reinterpret_cast<const float*>(data);
                        decoded.insert(decoded.end(), floats, floats + bytes / sizeof(float));
                        buffer->Unlock();
                    }
                    buffer->Release();
                }
                sample->Release();
            }
            if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
        }
        reader->Release();
    }
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    if (!ok || decoded.empty()) return false;

    const ma_uint64 inFrames = decoded.size() / channels;
    const ma_uint64 outFrames =
        ma_convert_frames(nullptr, 0, ma_format_f32, 2, rate, decoded.data(), inFrames, ma_format_f32, channels, fileRate);
    out.assign(static_cast<size_t>(outFrames) * 2, 0.0f);
    out.resize(static_cast<size_t>(ma_convert_frames(out.data(), outFrames, ma_format_f32, 2, rate, decoded.data(), inFrames,
                                                     ma_format_f32, channels, fileRate)) * 2);
    return !out.empty();
}
#endif

}  // namespace

std::shared_ptr<Sample> extractClip(const Sample& source, size_t start, size_t end) {
    end = std::min(end, source.frames);
    start = std::min(start, end);
    auto clip = std::make_shared<Sample>();
    clip->sampleRate = source.sampleRate;
    clip->stereo.assign(source.stereo.begin() + static_cast<std::ptrdiff_t>(start * 2),
                        source.stereo.begin() + static_cast<std::ptrdiff_t>(end * 2));
    finalizeSample(*clip);
    return clip;
}

bool writeWav(const Sample& sample, const std::string& path) {
    const ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, sample.sampleRate);
    ma_encoder encoder;
    if (ma_encoder_init_file(path.c_str(), &config, &encoder) != MA_SUCCESS) return false;
    ma_uint64 written = 0;
    const ma_result result =
        ma_encoder_write_pcm_frames(&encoder, sample.stereo.data(), sample.frames, &written);
    ma_encoder_uninit(&encoder);
    return result == MA_SUCCESS && written == sample.frames;
}

std::shared_ptr<Sample> loadSample(const std::string& path, uint32_t targetRate, std::string& error) {
    auto sample = std::make_shared<Sample>();
    bool ok = decodeWithMiniaudio(path, targetRate, sample->stereo);
#if defined(__APPLE__)
    if (!ok || sample->stereo.empty()) ok = decodeWithCoreAudio(path, targetRate, sample->stereo);
#elif defined(_WIN32)
    if (!ok || sample->stereo.empty()) ok = decodeWithMediaFoundation(path, targetRate, sample->stereo);
#endif
    if (!ok || sample->stereo.empty()) {
        error = "Couldn't decode " + std::filesystem::path(path).filename().string();
        return nullptr;
    }
    sample->path = path;
    sample->name = std::filesystem::path(path).filename().string();
    sample->sampleRate = targetRate;
    finalizeSample(*sample);
    return sample;
}
