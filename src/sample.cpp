#include "sample.h"

#include <algorithm>
#include <filesystem>

#include "miniaudio.h"

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
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
