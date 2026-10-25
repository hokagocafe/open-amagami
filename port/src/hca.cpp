#include "hca.hpp"

#include <memory>

extern "C" {
#include "third_party/clhca/clhca.h"
}

namespace amg {

bool decode_hca(const std::string& data, std::vector<int16_t>& pcm, int& rate, int& channels) {
    pcm.clear();
    if (data.size() < 8) return false;
    int header = clHCA_isOurFile(data.data(), unsigned(data.size()));
    if (header < 0 || size_t(header) > data.size()) return false;
    std::unique_ptr<clHCA, void (*)(clHCA*)> hca(clHCA_new(), clHCA_delete);
    if (!hca || clHCA_DecodeHeader(hca.get(), data.data(), unsigned(header)) < 0) return false;
    clHCA_stInfo info{};
    if (clHCA_getInfo(hca.get(), &info) < 0 || info.encryptionEnabled || info.channelCount == 0) return false;
    rate = int(info.samplingRate);
    channels = int(info.channelCount);
    std::vector<int16_t> block(size_t(info.samplesPerBlock) * info.channelCount);
    std::string frame(info.blockSize, '\0');
    pcm.reserve(size_t(info.blockCount) * block.size());
    for (unsigned b = 0; b < info.blockCount; ++b) {
        size_t at = size_t(info.headerSize) + size_t(b) * info.blockSize;
        if (at + info.blockSize > data.size()) break;
        frame.assign(data, at, info.blockSize);  // DecodeBlock may write into its buffer
        if (clHCA_DecodeBlock(hca.get(), frame.data(), info.blockSize) < 0) return false;
        clHCA_ReadSamples16(hca.get(), block.data());
        pcm.insert(pcm.end(), block.begin(), block.end());
    }
    // Drop the encoder's delay at the start and its padding at the end.
    size_t lead = size_t(info.encoderDelay) * info.channelCount;
    size_t total = size_t(info.sampleCount) * info.channelCount;
    if (lead >= pcm.size()) return false;
    pcm.erase(pcm.begin(), pcm.begin() + std::ptrdiff_t(lead));
    if (total && total < pcm.size()) pcm.resize(total);
    return !pcm.empty();
}

}  // namespace amg
