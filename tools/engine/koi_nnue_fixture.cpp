#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

#include "koi/nnue.hpp"

namespace {

constexpr std::string_view kUsage =
    "usage: koi-nnue-fixture --arch v4|v5 --output <path>\n";
constexpr std::uint32_t kGpuV5HiddenUnits = 1'536;
constexpr std::uint32_t kGpuV5L1Units = 32;

struct Arguments {
    std::string_view arch;
    std::filesystem::path output;
};

koi::NnueNetwork gpu_compatible_v5_fixture() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v5();
    // Keep the seed weights while value-initializing the added dimensions.
    // The same synthetic network then satisfies the GPU kernel's fixed shape.
    network.manifest.layer_sizes[1] = kGpuV5HiddenUnits;
    network.manifest.layer_sizes[3] = kGpuV5L1Units;

    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.l1_weights.resize(l1_units * hidden_units);
    network.l1_bias.resize(l1_units);
    network.bottleneck_weights.resize(output_buckets * l1_units);
    return network;
}

std::optional<Arguments> parse_arguments(int argc, char** argv) {
    std::optional<std::string_view> arch;
    std::optional<std::filesystem::path> output;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--arch") {
            if (arch.has_value() || index + 1 >= argc || argv[index + 1] == nullptr) {
                return std::nullopt;
            }
            arch = argv[++index];
        } else if (argument == "--output") {
            if (output.has_value() || index + 1 >= argc || argv[index + 1] == nullptr) {
                return std::nullopt;
            }
            output = std::filesystem::path(argv[++index]);
        } else {
            return std::nullopt;
        }
    }

    if (!arch.has_value() || !output.has_value() || output->empty() ||
        (*arch != "v4" && *arch != "v5")) {
        return std::nullopt;
    }
    return Arguments{*arch, std::move(*output)};
}

} // namespace

int main(int argc, char** argv) {
    const auto arguments = parse_arguments(argc, argv);
    if (!arguments.has_value()) {
        std::cerr << kUsage;
        return 2;
    }

    const koi::NnueNetwork network = arguments->arch == "v4"
                                         ? koi::NnueNetwork::synthetic_v4()
                                         : gpu_compatible_v5_fixture();
    const auto encoded = koi::NnueLoader::serialize(network);
    if (!encoded.has_value()) {
        std::cerr << "unable to serialize NNUE fixture: " << encoded.error().message << '\n';
        return 1;
    }

    std::ofstream output(arguments->output, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "unable to open NNUE fixture output: " << arguments->output.string() << '\n';
        return 1;
    }
    output.write(reinterpret_cast<const char*>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
    output.close();
    if (!output) {
        std::cerr << "unable to write NNUE fixture output: " << arguments->output.string() << '\n';
        return 1;
    }
    return 0;
}
