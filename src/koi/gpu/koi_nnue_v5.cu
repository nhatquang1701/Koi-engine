// CUDA kernel for the Koi NNUE v5 evaluator.
//
// One thread block evaluates one position.  The kernel reproduces the CPU
// scalar integer path exactly: int64 accumulation of both perspective
// accumulators, a single clamp into the 0..127 activation range, full-width
// cross-perspective pair products, the 32-unit clipped-ReLU hidden layer, and
// the piece-count bucket head.  No floating point is used anywhere.
//
// The host compiles this file with `nvcc -ptx -arch=compute_61` and embeds the
// PTX with the driver API, so the engine has no CUDA link-time dependency.

#include <cstdint>

namespace {

constexpr int kInput = 36864;
constexpr int kHidden = 1536;
constexpr int kL1 = 32;
constexpr int kBuckets = 8;
constexpr int kThreatBase = 9216;
constexpr int kThreatBucket = 2304;
constexpr int kThreads = 256;
constexpr int kSlots = kHidden / kThreads;
constexpr int kMaxFeatures = 320;
constexpr int kThreatWords = (kThreatBucket + 31) / 32;

__device__ int king_bucket(int square) {
    const int file = square & 7;
    const int rank = square >> 3;
    const int zone = rank <= 2 ? 0 : (rank <= 5 ? 1 : 2);
    const int mirrored = file < 4 ? file + 4 : file;
    return zone * 4 + (mirrored - 4);
}

__device__ int perspective_square(int square, int side) {
    return side == 0 ? square : square ^ 56;
}

__device__ unsigned long long step_attacks(int square, const int* steps, int count) {
    const int file = square & 7;
    const int rank = square >> 3;
    unsigned long long mask = 0;
    for (int index = 0; index < count; ++index) {
        const int target_file = file + steps[index * 2];
        const int target_rank = rank + steps[index * 2 + 1];
        if (target_file < 0 || target_file > 7 || target_rank < 0 || target_rank > 7) {
            continue;
        }
        mask |= 1ull << (target_rank * 8 + target_file);
    }
    return mask;
}

__device__ unsigned long long ray_attacks(int square, unsigned long long occupancy,
                                          const int* directions, int count) {
    const int file = square & 7;
    const int rank = square >> 3;
    unsigned long long mask = 0;
    for (int index = 0; index < count; ++index) {
        const int delta_file = directions[index * 2];
        const int delta_rank = directions[index * 2 + 1];
        int target_file = file + delta_file;
        int target_rank = rank + delta_rank;
        while (target_file >= 0 && target_file <= 7 && target_rank >= 0 && target_rank <= 7) {
            const int target = target_rank * 8 + target_file;
            mask |= 1ull << target;
            if (((occupancy >> target) & 1ull) != 0ull) {
                break;
            }
            target_file += delta_file;
            target_rank += delta_rank;
        }
    }
    return mask;
}

} // namespace

extern "C" __global__ void __launch_bounds__(kThreads) koi_nnue_v5_eval(
    const unsigned long long* __restrict__ occupancy,
    const unsigned char* __restrict__ pieces,
    const unsigned char* __restrict__ side_to_move,
    const unsigned char* __restrict__ king_squares,
    const std::int16_t* __restrict__ feature_weights,
    const std::int32_t* __restrict__ hidden_bias,
    const std::int8_t* __restrict__ l1_weights,
    const std::int32_t* __restrict__ l1_bias,
    const std::int8_t* __restrict__ output_weights,
    const std::int32_t* __restrict__ output_bias,
    int l1_shift,
    int output_shift,
    std::int32_t* __restrict__ scores,
    unsigned char* __restrict__ overflow) {
    const int position = blockIdx.x;
    const int tid = threadIdx.x;
    const unsigned long long occ_white = occupancy[position * 2 + 0];
    const unsigned long long occ_black = occupancy[position * 2 + 1];
    const unsigned long long occ_all = occ_white | occ_black;
    const unsigned char* board = pieces + position * 64;
    const int side = side_to_move[position];
    const int own_king = king_squares[position * 2 + side];
    const int opp_king = king_squares[position * 2 + (1 - side)];
    const int own_bucket = king_bucket(perspective_square(own_king, side));
    const int opp_bucket = king_bucket(perspective_square(opp_king, 1 - side));

    __shared__ unsigned int used_words[2 * kThreatWords];
    __shared__ unsigned short features[2][kMaxFeatures];
    __shared__ int feature_count[2];
    __shared__ int activations[2][kHidden];
    __shared__ int l1_values[kL1];
    __shared__ unsigned int overflow_flag;

    if (tid == 0) {
        feature_count[0] = 0;
        feature_count[1] = 0;
        overflow_flag = 0;
    }
    for (int index = tid; index < 2 * kThreatWords; index += kThreads) {
        used_words[index] = 0;
    }
    __syncthreads();

    // Phase 1: enumerate both sparse feature views (group A plus group B).
    if (tid == 0) {
        const int knight_steps[16] = {1, 2, 2, 1, 2, -1, 1, -2, -1, -2, -2, -1, -2, 1, -1, 2};
        const int king_steps[16] = {1, 1, 1, 0, 1, -1, 0, -1, -1, -1, -1, 0, -1, 1, 0, 1};
        const int bishop_directions[8] = {1, 1, 1, -1, -1, -1, -1, 1};
        const int rook_directions[8] = {1, 0, -1, 0, 0, 1, 0, -1};

        auto push = [&](int view, int index) {
            if (feature_count[view] >= kMaxFeatures) {
                overflow_flag = 1;
                return;
            }
            features[view][feature_count[view]++] = static_cast<unsigned short>(index);
        };

        // Group A: one input per piece per view.
        for (int square = 0; square < 64; ++square) {
            const unsigned char piece = board[square];
            if (piece == 0) {
                continue;
            }
            const int color = piece >> 3;
            const int type = piece & 7;
            for (int view = 0; view < 2; ++view) {
                const int perspective = view == 0 ? side : 1 - side;
                const int bucket = view == 0 ? own_bucket : opp_bucket;
                const int plane = color == perspective ? type - 1 : type + 5;
                push(view, bucket * 768 + plane * 64 + perspective_square(square, perspective));
            }
        }

        // Group B: every attack relation in the position, deduplicated per view.
        for (int square = 0; square < 64; ++square) {
            const unsigned char piece = board[square];
            if (piece == 0) {
                continue;
            }
            const int color = piece >> 3;
            const int type = piece & 7;
            const unsigned long long targets = color == 0 ? occ_black : occ_white;
            unsigned long long attacks = 0;
            switch (type) {
                case 1: {
                    const int file = square & 7;
                    const int rank = square >> 3;
                    const int delta_rank = color == 0 ? 1 : -1;
                    if (file > 0) {
                        attacks |= 1ull << ((rank + delta_rank) * 8 + file - 1);
                    }
                    if (file < 7) {
                        attacks |= 1ull << ((rank + delta_rank) * 8 + file + 1);
                    }
                    break;
                }
                case 2:
                    attacks = step_attacks(square, knight_steps, 8);
                    break;
                case 3:
                    attacks = ray_attacks(square, occ_all, bishop_directions, 4);
                    break;
                case 4:
                    attacks = ray_attacks(square, occ_all, rook_directions, 4);
                    break;
                case 5:
                    attacks = ray_attacks(square, occ_all, bishop_directions, 4) |
                        ray_attacks(square, occ_all, rook_directions, 4);
                    break;
                case 6:
                    attacks = step_attacks(square, king_steps, 8);
                    break;
                default:
                    break;
            }
            unsigned long long victims = attacks & targets;
            while (victims != 0ull) {
                const int victim_square = __ffsll(static_cast<long long>(victims)) - 1;
                victims &= victims - 1ull;
                const int victim_type = board[victim_square] & 7;
                for (int view = 0; view < 2; ++view) {
                    const int perspective = view == 0 ? side : 1 - side;
                    const int bucket = view == 0 ? own_bucket : opp_bucket;
                    const int attacker = perspective_square(square, perspective);
                    const int victim = perspective_square(victim_square, perspective);
                    int offset = 0;
                    switch (type) {
                        case 1:
                            offset = (victim_type - 1) * 64 + victim;
                            break;
                        case 2:
                            offset = 384 + (victim_type - 1) * 64 + victim;
                            break;
                        case 3:
                            offset = 768 + attacker * 6 + (victim_type - 1);
                            break;
                        case 4:
                            offset = 768 + 384 + attacker * 6 + (victim_type - 1);
                            break;
                        case 5:
                            offset = 768 + 768 + attacker * 6 + (victim_type - 1);
                            break;
                        case 6:
                            offset = 1920 + (victim_type - 1) * 64 + victim;
                            break;
                        default:
                            break;
                    }
                    const int index = kThreatBase + bucket * kThreatBucket + offset;
                    const int local = index - kThreatBase - bucket * kThreatBucket;
                    unsigned int* word = &used_words[view * kThreatWords + (local >> 5)];
                    const unsigned int bit = 1u << (local & 31);
                    if ((*word & bit) != 0u) {
                        continue;
                    }
                    *word |= bit;
                    push(view, index);
                }
            }
        }
    }
    __syncthreads();

    if (overflow_flag != 0u) {
        if (tid == 0) {
            overflow[position] = 1;
        }
        return;
    }
    if (tid == 0) {
        overflow[position] = 0;
    }

    // Phase 2: int64 accumulation of both perspective accumulators.
    long long own_accumulator[kSlots];
    long long opp_accumulator[kSlots];
    for (int slot = 0; slot < kSlots; ++slot) {
        const int hidden = tid + slot * kThreads;
        own_accumulator[slot] = hidden_bias[hidden];
        opp_accumulator[slot] = hidden_bias[hidden];
    }
    for (int view = 0; view < 2; ++view) {
        const int count = feature_count[view];
        for (int feature = 0; feature < count; ++feature) {
            const std::int16_t* row =
                feature_weights + static_cast<int>(features[view][feature]) * kHidden;
            if (view == 0) {
                for (int slot = 0; slot < kSlots; ++slot) {
                    own_accumulator[slot] += row[tid + slot * kThreads];
                }
            } else {
                for (int slot = 0; slot < kSlots; ++slot) {
                    opp_accumulator[slot] += row[tid + slot * kThreads];
                }
            }
        }
    }
    for (int slot = 0; slot < kSlots; ++slot) {
        const int hidden = tid + slot * kThreads;
        long long own_value = own_accumulator[slot];
        long long opp_value = opp_accumulator[slot];
        if (own_value < 0) {
            own_value = 0;
        } else if (own_value > 127) {
            own_value = 127;
        }
        if (opp_value < 0) {
            opp_value = 0;
        } else if (opp_value > 127) {
            opp_value = 127;
        }
        activations[0][hidden] = static_cast<int>(own_value);
        activations[1][hidden] = static_cast<int>(opp_value);
    }
    __syncthreads();

    // Phase 3: full-width cross pairs and the 32-unit clipped-ReLU layer.
    const int warp = tid >> 5;
    const int lane = tid & 31;
    long long partial[4];
    for (int unit = 0; unit < 4; ++unit) {
        const std::int8_t* row = l1_weights + (warp * 4 + unit) * kHidden;
        long long sum = 0;
        for (int hidden = lane; hidden < kHidden; hidden += 32) {
            sum += static_cast<long long>(row[hidden]) *
                (static_cast<long long>(activations[0][hidden]) * activations[1][hidden]);
        }
        partial[unit] = sum;
    }
    for (int unit = 0; unit < 4; ++unit) {
        for (int offset = 16; offset > 0; offset >>= 1) {
            partial[unit] += __shfl_down_sync(0xffffffffu, partial[unit], offset);
        }
    }
    if (lane == 0) {
        for (int unit = 0; unit < 4; ++unit) {
            const int target = warp * 4 + unit;
            long long value = partial[unit] + l1_bias[target];
            if (l1_shift > 0) {
                value >>= l1_shift;
            }
            if (value < 0) {
                value = 0;
            } else if (value > 127) {
                value = 127;
            }
            l1_values[target] = static_cast<int>(value);
        }
    }
    __syncthreads();

    // Phase 4: piece-count bucket head.
    if (tid == 0) {
        int count = 0;
        for (int square = 0; square < 64; ++square) {
            if (board[square] != 0) {
                ++count;
            }
        }
        const int missing = 32 - (count < 32 ? count : 32);
        int bucket = missing / 4;
        if (bucket > kBuckets - 1) {
            bucket = kBuckets - 1;
        }
        long long output = output_bias[bucket];
        for (int unit = 0; unit < kL1; ++unit) {
            output += static_cast<long long>(output_weights[bucket * kL1 + unit]) * l1_values[unit];
        }
        if (output_shift > 0) {
            output >>= output_shift;
        }
        if (output < (-2147483647LL - 1LL)) {
            output = -2147483647LL - 1LL;
        }
        if (output > 2147483647LL) {
            output = 2147483647LL;
        }
        scores[position] = static_cast<std::int32_t>(output);
    }
}
