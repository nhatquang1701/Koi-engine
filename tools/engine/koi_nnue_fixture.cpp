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

struct Arguments {
    std::string_view arch;
    std::filesystem::path output;
};

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
                                         : koi::NnueNetwork::synthetic_v5();
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
