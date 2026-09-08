#include "../src/remote_microphone_buffer.h"

#include <cstddef>
#include <iostream>
#include <vector>

int main()
{
    RemoteMicrophoneBuffer buffer;
    constexpr std::uint32_t sampleRate = 10;
    std::vector<float> samples(31 * sampleRate, 0.25F);
    buffer.append(0, sampleRate, samples);

    if (buffer.bufferedFrames() != 30 * sampleRate
        || buffer.trimmedFrames() != sampleRate
        || buffer.latestFrameIndex() != samples.size()) {
        std::cerr << "Remote microphone buffer did not enforce its 30-second limit\n";
        return 1;
    }

    const RemoteAudioSnapshot snapshot = buffer.snapshotFrom(0);
    if (snapshot.firstFrameIndex != sampleRate
        || snapshot.samples.size() != 30 * sampleRate) {
        std::cerr << "Remote microphone snapshot did not follow the trimmed window\n";
        return 1;
    }

    std::cout << "Remote microphone buffer limit test passed\n";
    return 0;
}
