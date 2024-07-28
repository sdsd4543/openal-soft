/*
 * OpenAL LAF Playback Example
 *
 * Copyright (c) 2024 by Chris Robinson <chris.kcat@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* This file contains an example for playback of Limitless Audio Format files.
 *
 * Some current shortcomings:
 *
 * - There must be no space between the LIMITLESS and HEAD markers. Since the
 *   format doesn't specify the size with each section marker, it's not
 *   straight-forward to efficiently find the HEAD marker if there's extra
 *   data in between. It shouldn't be hard to fix, but it's on the back-burner
 *   for now.
 *
 * - Little-endian only. It shouldn't be too hard to fix with byteswap helpers.
 *
 * - 256 track limit. Could be made higher, but making it too flexible would
 *   necessitate more micro-allocations.
 *
 * - 24-bit samples are unsupported. Will need conversion to either 16-bit or
 *   float samples when buffering.
 *
 * - "Objects" mode only supports sample rates that are a multiple of 48. Since
 *   positions are specified as samples in extra channels/tracks, and 3*16
 *   samples are needed per track to specify the full set of positions, and
 *   each chunk is exactly one second long, other sample rates would result in
 *   the positions being split across chunks, causing the source playback
 *   offset to go out of sync with the offset used to look up the current
 *   spatial positions. Fixing this will require slightly more work to update
 *   and synchronize the spatial position arrays against the playback offset.
 *
 * - Updates are specified as fast as the app can detect and react to the
 *   reported source offset (that in turn depends on how often OpenAL renders).
 *   This can cause some positions to be a touch late and lose some granular
 *   temporal movement. In practice, this should probably be good enough for
 *   most use-cases. Fixing this would need either a new extension to queue
 *   position changes to apply when needed, or use a separate loopback device
 *   to render with and control the number of samples rendered between updates
 *   (with a second device to do the actual playback).
 *
 * - LFE channels are silenced. Since LFE signals can really contain anything,
 *   and may expect to be low-pass filtered for/by the subwoofer it's sent to,
 *   it's best to not play them raw. This can be fixed with AL_EXT_DEDICATED's
 *   AL_EFFECT_DEDICATED_LOW_FREQUENCY_EFFECT to silence the direct output and
 *   send the signal to the LFE output if it exists.
 *
 * - The LAF documentation doesn't prohibit object position tracks from being
 *   separated with audio tracks in between, or from being the first tracks
 *   followed by the audio tracks. It's not known if this is intended to be
 *   allowed, but it's not supported. Object position tracks must be last.
 *
 * Some remaining issues:
 *
 * - There are bursts of static on some channels. This doesn't appear to be a
 *   parsing error since the bursts last less than the chunk size, and it never
 *   loses sync with the remaining chunks. Might be an encoding error with the
 *   files tested.
 *
 * - Positions are specified in left-handed coordinates, despite the LAF
 *   documentation saying it's right-handed. Might be an encoding error with
 *   the files tested, or might be a misunderstanding about which is which. How
 *   to proceed may depend on how wide-spread this issue ends up being, but for
 *   now, they're treated as left-handed here.
 *
 * - The LAF documentation doesn't specify the range or direction for the
 *   channels' X and Y axis rotation in Channels mode. Presumably X rotation
 *   (elevation) goes from -pi/2...+pi/2 and Y rotation (azimuth) goes from
 *   either -pi...+pi or 0...pi*2, but the direction of movement isn't
 *   specified. Currently positive azimuth moves from center rightward and
 *   positive elevation moves from head-level upward.
 */

#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "AL/alc.h"
#include "AL/al.h"
#include "AL/alext.h"

#include "alassert.h"
#include "albit.h"
#include "alnumeric.h"
#include "alspan.h"
#include "alstring.h"
#include "common/alhelpers.h"

#include "win_main_utf8.h"

namespace {

namespace fs = std::filesystem;
using namespace std::string_view_literals;


enum class Quality : std::uint8_t {
    s8, s16, f32, s24
};
enum class Mode : bool {
    Channels, Objects
};

auto GetQualityName(Quality quality) noexcept -> std::string_view
{
    switch(quality)
    {
    case Quality::s8: return "8-bit int"sv;
    case Quality::s16: return "16-bit int"sv;
    case Quality::f32: return "32-bit float"sv;
    case Quality::s24: return "24-bit int"sv;
    }
    return "<unknown>"sv;
}

auto GetModeName(Mode mode) noexcept -> std::string_view
{
    switch(mode)
    {
    case Mode::Channels: return "channels"sv;
    case Mode::Objects: return "objects"sv;
    }
    return "<unknown>"sv;
}

auto BytesFromQuality(Quality quality) noexcept -> size_t
{
    switch(quality)
    {
    case Quality::s8: return 1;
    case Quality::s16: return 2;
    case Quality::f32: return 4;
    case Quality::s24: return 3;
    }
    return 4;
}

auto FormatFromQuality(Quality quality) -> ALenum
{
    switch(quality)
    {
    case Quality::s8: return AL_FORMAT_MONO8;
    case Quality::s16: return AL_FORMAT_MONO16;
    case Quality::f32: return AL_FORMAT_MONO_FLOAT32;
    case Quality::s24: throw std::runtime_error{"24-bit samples not supported"};
    }
    return AL_NONE;
}


/* Each track with position data consists of a set of 3 samples per 16 audio
 * channels, resulting in a full set of positions being specified over 48
 * sample frames.
 */
constexpr auto FramesPerPos = 48_uz;

struct Channel {
    ALuint mSource{};
    std::array<ALuint,2> mBuffers{};
    float mAzimuth{};
    float mElevation{};
    bool mIsLfe{};

    Channel() = default;
    Channel(const Channel&) = delete;
    Channel(Channel&& rhs)
        : mSource{rhs.mSource}, mBuffers{rhs.mBuffers}, mAzimuth{rhs.mAzimuth}
        , mElevation{rhs.mElevation}, mIsLfe{rhs.mIsLfe}
    {
        rhs.mSource = 0;
        rhs.mBuffers.fill(0);
    }
    ~Channel()
    {
        if(mSource) alDeleteSources(1, &mSource);
        if(mBuffers[0]) alDeleteBuffers(ALsizei(mBuffers.size()), mBuffers.data());
    }

    auto operator=(const Channel&) -> Channel& = delete;
    auto operator=(Channel&& rhs) -> Channel&
    {
        std::swap(mSource, rhs.mSource);
        std::swap(mBuffers, rhs.mBuffers);
        std::swap(mAzimuth, rhs.mAzimuth);
        std::swap(mElevation, rhs.mElevation);
        std::swap(mIsLfe, rhs.mIsLfe);
        return *this;
    }
};

struct LafStream {
    std::ifstream mInFile;

    Quality mQuality{};
    Mode mMode{};
    uint32_t mSampleRate{};
    uint64_t mSampleCount{};
    uint32_t mNumTracks{};

    uint64_t mCurrentSample{};

    std::array<uint8_t,32> mEnabledTracks{};
    uint32_t mNumEnabled{};
    std::vector<char> mSampleChunk;
    al::span<char> mSampleLine;

    std::vector<Channel> mChannels;
    std::vector<std::vector<float>> mPosTracks;

    LafStream() = default;
    LafStream(const LafStream&) = delete;
    ~LafStream() = default;
    auto operator=(const LafStream&) -> LafStream& = delete;

    [[nodiscard]]
    auto readChunk() -> uint32_t;

    void convertPositions(const al::span<float> dst, const al::span<const char> src) const;

    template<typename T>
    void copySamples(char *dst, const char *src, size_t idx, size_t count) const;

    [[nodiscard]]
    auto prepareTrack(size_t trackidx, size_t count) -> al::span<char>;

    [[nodiscard]]
    auto isAtEnd() const noexcept -> bool { return mCurrentSample >= mSampleCount; }
};

auto LafStream::readChunk() -> uint32_t
{
    mEnabledTracks.fill(0);
    mInFile.read(reinterpret_cast<char*>(mEnabledTracks.data()), (mNumTracks+7_z)>>3);
    mNumEnabled = std::accumulate(mEnabledTracks.cbegin(), mEnabledTracks.cend(), 0u,
        [](const unsigned int val, const uint8_t in)
        { return val + unsigned(al::popcount(unsigned(in))); });

    alassert(mEnabledTracks[((mNumTracks+7_uz)>>3) - 1] < (1u<<(mNumTracks&7)));

    /* Each chunk is exactly one second long, with samples interleaved for each
     * enabled track.
     */
    const auto toread = std::streamsize(mSampleRate * BytesFromQuality(mQuality) * mNumEnabled);
    mInFile.read(mSampleChunk.data(), toread);

    const auto numsamples = std::min(uint64_t{mSampleRate}, mSampleCount-mCurrentSample);
    mCurrentSample += numsamples;
    return static_cast<uint32_t>(numsamples);
}

void LafStream::convertPositions(const al::span<float> dst, const al::span<const char> src) const
{
    switch(mQuality)
    {
    case Quality::s8:
        std::transform(src.begin(), src.end(), dst.begin(),
            [](const int8_t in) { return float(in) / 127.0f; });
        break;
    case Quality::s16:
        {
            auto i16src = al::span{reinterpret_cast<const int16_t*>(src.data()),
                src.size()/sizeof(int16_t)};
            std::transform(i16src.begin(), i16src.end(), dst.begin(),
                [](const int16_t in) { return float(in) / 32767.0f; });
        }
        break;
    case Quality::f32:
        {
            auto f32src = al::span{reinterpret_cast<const float*>(src.data()),
                src.size()/sizeof(float)};
            std::copy(f32src.begin(), f32src.end(), dst.begin());
        }
        break;
    case Quality::s24:
        break;
    }
}

template<typename T>
void LafStream::copySamples(char *dst, const char *src, const size_t idx, const size_t count) const
{
    const auto step = mNumEnabled;
    assert(idx < step);

    auto input = al::span{reinterpret_cast<const T*>(src), count*step};
    auto output = al::span{reinterpret_cast<T*>(dst), count};

    auto inptr = input.begin();
    std::generate_n(output.begin(), output.size(), [&inptr,idx,step]
    {
        auto ret = inptr[idx];
        inptr += step;
        return ret;
    });
}

auto LafStream::prepareTrack(const size_t trackidx, const size_t count) -> al::span<char>
{
    const auto todo = std::min(size_t{mSampleRate}, count);
    if((mEnabledTracks[trackidx>>3] & (1_uz<<(trackidx&7))))
    {
        /* If the track is enabled, get the real index (skipping disabled
         * tracks), and deinterlace it into the mono line.
         */
        const auto idx = [this,trackidx]() -> unsigned int
        {
            const auto bits = al::span{mEnabledTracks}.first(trackidx>>3);
            const auto res = std::accumulate(bits.begin(), bits.end(), 0u,
                [](const unsigned int val, const uint8_t in)
                { return val + unsigned(al::popcount(unsigned(in))); });
            return unsigned(al::popcount(mEnabledTracks[trackidx>>3] & ((1_uz<<(trackidx&7))-1)))
                + res;
        }();

        switch(mQuality)
        {
        case Quality::s8:
            copySamples<int8_t>(mSampleLine.data(), mSampleChunk.data(), idx, todo);
            break;
        case Quality::s16:
            copySamples<int16_t>(mSampleLine.data(), mSampleChunk.data(), idx, todo);
            break;
        case Quality::f32:
            copySamples<float>(mSampleLine.data(), mSampleChunk.data(), idx, todo);
            break;
        case Quality::s24:
            throw std::runtime_error{"24-bit samples not supported"};
        }
    }
    else
    {
        /* If the track is disabled, provide silence. */
        std::fill_n(mSampleLine.begin(), mSampleLine.size(),
            (mQuality==Quality::s8) ? char(0x80) : char{});
    }

    return mSampleLine.first(todo * BytesFromQuality(mQuality));
}


auto LoadLAF(const fs::path &fname) -> std::unique_ptr<LafStream>
{
    auto laf = std::make_unique<LafStream>();
    laf->mInFile = std::ifstream{fname, std::ios_base::binary};

    auto marker = std::array<char,9>{};
    alassert(laf->mInFile.read(marker.data(), marker.size()));
    alassert((std::string_view{marker.data(), marker.size()} == "LIMITLESS"sv));

    auto header = std::array<char,10>{};
    alassert(laf->mInFile.read(header.data(), header.size()));
    alassert((std::string_view{header.data(), 4} == "HEAD"sv));

    laf->mQuality = [stype=int{header[4]}] {
        if(stype == 0) return Quality::s8;
        if(stype == 1) return Quality::s16;
        if(stype == 2) return Quality::f32;
        if(stype == 3) return Quality::s24;
        throw std::runtime_error{"Invalid quality type: "+std::to_string(stype)};
    }();

    laf->mMode = [mode=int{header[5]}] {
        if(mode == 0) return Mode::Channels;
        if(mode == 1) return Mode::Objects;
        throw std::runtime_error{"Invalid mode: "+std::to_string(mode)};
    }();

    laf->mNumTracks = [input=al::span{header}.subspan<6,4>()] {
        auto data = std::array<char,4>{};
        std::copy_n(input.begin(), input.size(), data.begin());
        return al::bit_cast<uint32_t>(data);
    }();

    std::cout<< "Filename: "<<fname<<'\n';
    std::cout<< " quality: "<<GetQualityName(laf->mQuality)<<'\n';
    std::cout<< " mode: "<<GetModeName(laf->mMode)<<'\n';
    std::cout<< " track count: "<<laf->mNumTracks<<'\n';

    if(laf->mNumTracks > 256)
        throw std::runtime_error{"Too many tracks: "+std::to_string(laf->mNumTracks)};

    auto chandata = std::vector<char>(laf->mNumTracks*9_uz);
    assert(laf->mInFile.read(chandata.data(), std::streamsize(chandata.size())));

    laf->mChannels.reserve(laf->mNumTracks);
    for(uint32_t i{0};i < laf->mNumTracks;++i)
    {
        static constexpr auto read_float = [](al::span<char,4> input)
        {
            auto data = std::array<char,4>{};
            std::copy_n(input.begin(), input.size(), data.begin());
            return al::bit_cast<float>(data);
        };

        auto chan = al::span{chandata}.subspan(i*9_uz, 9);
        auto x_axis = read_float(chan.first<4>());
        auto y_axis = read_float(chan.subspan<4,4>());
        auto lfe_flag = int{chan[8]};

        std::cout<< "Track "<<i<<": E="<<x_axis<<", A="<<y_axis<<" (LFE: "<<lfe_flag<<")\n";

        if(x_axis != x_axis && y_axis == 0.0)
        {
            alassert(laf->mMode == Mode::Objects);
            alassert(i != 0);
            laf->mPosTracks.emplace_back();
        }
        else
        {
            alassert(laf->mPosTracks.empty());
            alassert(std::isfinite(x_axis) && std::isfinite(y_axis));
            auto &channel = laf->mChannels.emplace_back();
            channel.mAzimuth = y_axis;
            channel.mElevation = x_axis;
            channel.mIsLfe = lfe_flag != 0;
        }
    }
    std::cout<< "Channels: "<<laf->mChannels.size()<<'\n';

    /* For "objects" mode, ensure there's enough tracks with position data to
     * handle the audio channels.
     */
    if(laf->mMode == Mode::Objects)
        alassert(((laf->mChannels.size()-1)>>4) == laf->mPosTracks.size()-1);

    auto footer = std::array<char,12>{};
    alassert(laf->mInFile.read(footer.data(), footer.size()));

    laf->mSampleRate = [input=al::span{footer}.first<4>()] {
        auto data = std::array<char,4>{};
        std::copy_n(input.begin(), input.size(), data.begin());
        return al::bit_cast<uint32_t>(data);
    }();
    laf->mSampleCount = [input=al::span{footer}.last<8>()] {
        auto data = std::array<char,8>{};
        std::copy_n(input.begin(), input.size(), data.begin());
        return al::bit_cast<uint64_t>(data);
    }();
    std::cout<< "Sample rate: "<<laf->mSampleRate<<'\n';
    std::cout<< "Length: "<<laf->mSampleCount<<" samples ("
        <<(static_cast<double>(laf->mSampleCount)/static_cast<double>(laf->mSampleRate))<<" sec)\n";

    /* Position vectors get split across the PCM chunks if the sample rate
     * isn't a multiple of 48. Each PCM chunk is exactly one second (the sample
     * rate in sample frames). Each track with position data consists of a set
     * of 3 samples for 16 audio channels, resuling in 48 sample frames for a
     * full set of positions. Extra logic will be needed to manage the position
     * frame offset separate from each chunk.
     */
    alassert(laf->mMode == Mode::Channels || (laf->mSampleRate%FramesPerPos) == 0);

    for(size_t i{0};i < laf->mPosTracks.size();++i)
        laf->mPosTracks[i].resize(laf->mSampleRate*2_uz, 0.0f);

    laf->mSampleChunk.resize(laf->mSampleRate * BytesFromQuality(laf->mQuality)
        * (laf->mNumTracks+1));
    laf->mSampleLine = al::span{laf->mSampleChunk}.last(laf->mSampleRate
        * BytesFromQuality(laf->mQuality));

    return laf;
}

void PlayLAF(std::string_view fname)
{
    auto laf = LoadLAF(fs::u8path(fname));

    auto alloc_channel = [](Channel &channel)
    {
        alGenSources(1, &channel.mSource);
        alGenBuffers(ALsizei(channel.mBuffers.size()), channel.mBuffers.data());

        /* FIXME: Is the Y rotation/azimuth clockwise or counter-clockwise?
         * Does +azimuth move the sound right or left?
         */
        const auto x = std::sin(channel.mAzimuth) * std::cos(channel.mElevation);
        const auto y = std::sin(channel.mElevation);
        const auto z = -std::cos(channel.mAzimuth) * std::cos(channel.mElevation);
        alSource3f(channel.mSource, AL_POSITION, x, y, z);
        alSourcef(channel.mSource, AL_ROLLOFF_FACTOR, 0.0f);
        alSourcei(channel.mSource, AL_SOURCE_RELATIVE, AL_TRUE);

        /* Silence LFE channels since they may not be appropriate to play
         * normally. AL_EFFECT_DEDICATED_LOW_FREQUENCY_EFFECT could be used to
         * send them to the proper output.
         */
        if(channel.mIsLfe)
            alSourcef(channel.mSource, AL_GAIN, 0.0f);

        if(auto err=alGetError())
            throw std::runtime_error{std::string{"OpenAL error: "} + alGetString(err)};
    };
    std::for_each(laf->mChannels.begin(), laf->mChannels.end(), alloc_channel);

    while(!laf->isAtEnd())
    {
        auto state = ALenum{};
        auto offset = ALint{};
        auto processed = ALint{};
        /* All sources are played in sync, so they'll all be at the same offset
         * with the same state and number of processed buffers. Query the back
         * source just in case the previous update ran really late and missed
         * updating only some sources on time (in which case, the latter ones
         * will underrun, which this will detect and restart them all as
         * needed).
         */
        alGetSourcei(laf->mChannels.back().mSource, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(laf->mChannels.back().mSource, AL_SAMPLE_OFFSET, &offset);
        alGetSourcei(laf->mChannels.back().mSource, AL_SOURCE_STATE, &state);

        if(state == AL_PLAYING || state == AL_PAUSED)
        {
            if(!laf->mPosTracks.empty())
            {
                alcSuspendContext(alcGetCurrentContext());
                for(size_t i{0};i < laf->mChannels.size();++i)
                {
                    const auto trackidx = i>>4;

                    const auto posoffset = unsigned(offset)/FramesPerPos*16_uz + (i&15);
                    const auto x = laf->mPosTracks[trackidx][posoffset*3 + 0];
                    const auto y = laf->mPosTracks[trackidx][posoffset*3 + 1];
                    const auto z = laf->mPosTracks[trackidx][posoffset*3 + 2];

                    /* Contrary to the docs, the position is left-handed and
                     * needs to be converted to right-handed.
                     */
                    alSource3f(laf->mChannels[i].mSource, AL_POSITION, x, y, -z);
                }
                alcProcessContext(alcGetCurrentContext());
            }

            if(processed > 0)
            {
                const auto numsamples = laf->readChunk();
                for(size_t i{0};i < laf->mChannels.size();++i)
                {
                    const auto samples = laf->prepareTrack(i, numsamples);
                    auto bufid = ALuint{};
                    alSourceUnqueueBuffers(laf->mChannels[i].mSource, 1, &bufid);
                    alBufferData(bufid, FormatFromQuality(laf->mQuality), samples.data(),
                        ALsizei(samples.size()), ALsizei(laf->mSampleRate));
                    alSourceQueueBuffers(laf->mChannels[i].mSource, 1, &bufid);
                }
                for(size_t i{0};i < laf->mPosTracks.size();++i)
                {
                    std::copy(laf->mPosTracks[i].begin() + laf->mSampleRate,
                        laf->mPosTracks[i].end(), laf->mPosTracks[i].begin());

                    const auto positions = laf->prepareTrack(laf->mChannels.size()+i, numsamples);
                    laf->convertPositions(al::span{laf->mPosTracks[i]}.last(laf->mSampleRate),
                        positions);
                }
            }
            else
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        else if(state == AL_STOPPED)
        {
            auto sources = std::array<ALuint,256>{};
            for(size_t i{0};i < laf->mChannels.size();++i)
                sources[i] = laf->mChannels[i].mSource;
            alSourcePlayv(ALsizei(laf->mChannels.size()), sources.data());
        }
        else if(state == AL_INITIAL)
        {
            auto sources = std::array<ALuint,256>{};
            auto numsamples = laf->readChunk();
            for(size_t i{0};i < laf->mChannels.size();++i)
            {
                const auto samples = laf->prepareTrack(i, numsamples);
                alBufferData(laf->mChannels[i].mBuffers[0], FormatFromQuality(laf->mQuality),
                    samples.data(), ALsizei(samples.size()), ALsizei(laf->mSampleRate));
            }
            for(size_t i{0};i < laf->mPosTracks.size();++i)
            {
                const auto positions = laf->prepareTrack(laf->mChannels.size()+i, numsamples);
                laf->convertPositions(al::span{laf->mPosTracks[i]}.first(laf->mSampleRate),
                    positions);
            }

            numsamples = laf->readChunk();
            for(size_t i{0};i < laf->mChannels.size();++i)
            {
                const auto samples = laf->prepareTrack(i, numsamples);
                alBufferData(laf->mChannels[i].mBuffers[1], FormatFromQuality(laf->mQuality),
                    samples.data(), ALsizei(samples.size()), ALsizei(laf->mSampleRate));
                alSourceQueueBuffers(laf->mChannels[i].mSource,
                    ALsizei(laf->mChannels[i].mBuffers.size()), laf->mChannels[i].mBuffers.data());
                sources[i] = laf->mChannels[i].mSource;
            }
            for(size_t i{0};i < laf->mPosTracks.size();++i)
            {
                const auto positions = laf->prepareTrack(laf->mChannels.size()+i, numsamples);
                laf->convertPositions(al::span{laf->mPosTracks[i]}.last(laf->mSampleRate),
                    positions);
            }

            if(!laf->mPosTracks.empty())
            {
                for(size_t i{0};i < laf->mChannels.size();++i)
                {
                    const auto trackidx = i>>4;

                    const auto x = laf->mPosTracks[trackidx][(i&15)*3 + 0];
                    const auto y = laf->mPosTracks[trackidx][(i&15)*3 + 1];
                    const auto z = laf->mPosTracks[trackidx][(i&15)*3 + 2];

                    alSource3f(laf->mChannels[i].mSource, AL_POSITION, x, y, -z);
                }
            }

            alSourcePlayv(ALsizei(laf->mChannels.size()), sources.data());
        }
        else
            break;
    }

    auto state = ALenum{};
    auto offset = ALint{};
    alGetSourcei(laf->mChannels.back().mSource, AL_SAMPLE_OFFSET, &offset);
    alGetSourcei(laf->mChannels.back().mSource, AL_SOURCE_STATE, &state);
    while(alGetError() == AL_NO_ERROR && state == AL_PLAYING)
    {
        if(!laf->mPosTracks.empty())
        {
            alcSuspendContext(alcGetCurrentContext());
            for(size_t i{0};i < laf->mChannels.size();++i)
            {
                const auto trackidx = i>>4;

                const auto posoffset = unsigned(offset)/FramesPerPos*16_uz + (i&15);
                const auto x = laf->mPosTracks[trackidx][posoffset*3 + 0];
                const auto y = laf->mPosTracks[trackidx][posoffset*3 + 1];
                const auto z = laf->mPosTracks[trackidx][posoffset*3 + 2];

                alSource3f(laf->mChannels[i].mSource, AL_POSITION, x, y, -z);
            }
            alcProcessContext(alcGetCurrentContext());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        alGetSourcei(laf->mChannels.back().mSource, AL_SAMPLE_OFFSET, &offset);
        alGetSourcei(laf->mChannels.back().mSource, AL_SOURCE_STATE, &state);
    }
}

auto main(al::span<std::string_view> args) -> int
{
    /* Print out usage if no arguments were specified */
    if(args.size() < 2)
    {
        fprintf(stderr, "Usage: %.*s [-device <name>] <filenames...>\n", al::sizei(args[0]),
            args[0].data());
        return 1;
    }
    args = args.subspan(1);

    /* A simple RAII container for OpenAL startup and shutdown. */
    struct AudioManager {
        AudioManager(al::span<std::string_view> &args_)
        {
            if(InitAL(args_) != 0)
                throw std::runtime_error{"Failed to initialize OpenAL"};
        }
        ~AudioManager() { CloseAL(); }
    };
    AudioManager almgr{args};

    std::for_each(args.begin(), args.end(), PlayLAF);

    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    alassert(argc >= 0);
    auto args = std::vector<std::string_view>(static_cast<unsigned int>(argc));
    std::copy_n(argv, args.size(), args.begin());
    return main(al::span{args});
}
