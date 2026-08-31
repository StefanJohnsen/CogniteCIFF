/*----------------------------------------------------------------
  Zlib.h

  Header-only zlib/DEFLATE compressor and decompressor.

  Provides a standalone, dependency-free implementation for
  compressing and decompressing zlib-wrapped DEFLATE streams.

  Requirements: C++20
  OS Support:   Windows/macOS/Linux

  Format specifications:
  - RFC 1950: zlib wrapper and Adler-32 checksum
  - RFC 1951: DEFLATE blocks and Huffman coding

  License: MIT
  Maintainer: Stefan Falk Johnsen
  Copyright (c) 2026 FalconCoding

  GitHub: https://github.com/StefanJohnsen
----------------------------------------------------------------*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace ciff::zlib
{
    struct InflateContext;

    namespace inflate_error
    {
        inline constexpr ptrdiff_t invalid_argument = -1;
        inline constexpr ptrdiff_t destination_too_large = -2;
        inline constexpr ptrdiff_t truncated_input = -3;
        inline constexpr ptrdiff_t invalid_wrapper_header = -4;
        inline constexpr ptrdiff_t unsupported_wrapper_feature = -5;
        inline constexpr ptrdiff_t invalid_block_type = -6;
        inline constexpr ptrdiff_t invalid_stored_block = -7;
        inline constexpr ptrdiff_t invalid_huffman_table = -8;
        inline constexpr ptrdiff_t invalid_code_length = -9;
        inline constexpr ptrdiff_t invalid_length_code = -10;
        inline constexpr ptrdiff_t invalid_distance_code = -11;
        inline constexpr ptrdiff_t invalid_back_reference = -12;
        inline constexpr ptrdiff_t output_overrun = -13;
        inline constexpr ptrdiff_t output_underrun = -14;
        inline constexpr ptrdiff_t checksum_mismatch = -15;
        inline constexpr ptrdiff_t trailing_garbage = -16;
    }

    namespace config
    {
        inline constexpr uint32_t ADLER_MOD = 65521U;
        inline constexpr size_t MAX_BITS = 15;
        inline constexpr size_t LITERAL_LENGTH_SYMBOLS = 288;
        inline constexpr size_t DISTANCE_SYMBOLS = 32;
        inline constexpr size_t LENGTH_CODES = 29;
        inline constexpr size_t DISTANCE_CODES = 30;
        inline constexpr size_t CODE_LENGTH_CODES = 19;
        inline constexpr std::array<int, LENGTH_CODES> LENGTH_BASES = {{3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                                        15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                                        67, 83, 99, 115, 131, 163, 195, 227, 258}};
        inline constexpr std::array<uint8_t, LENGTH_CODES> LENGTH_EXTRA_BITS = {
            {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}};
        inline constexpr std::array<int, DISTANCE_CODES> DISTANCE_BASES = {
            {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}};
        inline constexpr std::array<uint8_t, DISTANCE_CODES> DISTANCE_EXTRA_BITS = {
            {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13}};
        inline constexpr std::array<uint8_t, CODE_LENGTH_CODES> CODE_LENGTH_ORDER = {
            {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15}};
        inline constexpr uint8_t FIXED_LITLEN_BITS_0_143 = 8;
        inline constexpr uint8_t FIXED_LITLEN_BITS_144_255 = 9;
        inline constexpr uint8_t FIXED_LITLEN_BITS_256_279 = 7;
        inline constexpr uint8_t FIXED_LITLEN_BITS_280_287 = 8;
        inline constexpr uint8_t FIXED_DISTANCE_BITS = 5;
    }

    namespace detail
    {
        using namespace zlib::inflate_error;

        struct WorkingSet;

        struct BitCursor
        {
            const uint8_t* inputBegin = nullptr;
            const uint8_t* inputCursor = nullptr;
            const uint8_t* inputLimit = nullptr;
            uint64_t bitWindow = 0;
            uint32_t windowFill = 0;

            BitCursor(const uint8_t* data, const size_t size)
                : inputBegin(data), inputCursor(data), inputLimit(data + size)
            {
            }

            [[nodiscard]] size_t cursorBytes() const noexcept
            {
                return static_cast<size_t>(inputCursor - inputBegin);
            }

            [[nodiscard]] size_t usedBits() const noexcept
            {
                return cursorBytes() * 8U - windowFill;
            }

            bool ensureBits(const uint32_t count) noexcept
            {
                // Fast path: bulk load up to 8 bytes at once when possible
                if (windowFill < count)
                {
                    const auto bytesAvailable = static_cast<size_t>(inputLimit - inputCursor);

                    if (bytesAvailable >= 8 && windowFill <= 56)
                    {
                        // Bulk load 8 bytes (only use what fits in 64-bit window)
                        uint64_t bytes;
                        std::memcpy(&bytes, inputCursor, 8);
                        bitWindow |= bytes << windowFill;
                        const auto bytesToConsume = (64U - windowFill) / 8U;
                        inputCursor += bytesToConsume;
                        windowFill += bytesToConsume * 8U;
                    }
                    else
                    {
                        // Fallback: load one byte at a time
                        while (windowFill < count && inputCursor < inputLimit)
                        {
                            bitWindow |= static_cast<uint64_t>(*inputCursor++) << windowFill;
                            windowFill += 8;
                        }
                    }
                }

                return windowFill >= count;
            }

            [[nodiscard]] uint32_t peekBits(const uint32_t count) const noexcept
            {
                return static_cast<uint32_t>(bitWindow & ((UINT64_C(1) << count) - 1U));
            }

            void discardBits(const uint32_t count) noexcept
            {
                bitWindow >>= count;
                windowFill -= count;
            }

            bool tryPeekBits(const uint32_t count, uint32_t& packedValue) noexcept
            {
                if (!ensureBits(count))
                    return false;

                packedValue = peekBits(count);
                return true;
            }

            bool pullBit(uint32_t& valueBit) noexcept
            {
                if (!ensureBits(1))
                    return false;

                valueBit = static_cast<uint32_t>(bitWindow & 1U);
                discardBits(1);
                return true;
            }

            bool pullBits(const uint32_t count, uint32_t& packedValue) noexcept
            {
                if (count == 0)
                {
                    packedValue = 0;
                    return true;
                }

                if (!ensureBits(count))
                    return false;

                packedValue = peekBits(count);
                discardBits(count);
                return true;
            }

            void clearByteRemainder() noexcept
            {
                const auto remainder = windowFill & 7U;

                if (remainder != 0)
                    discardBits(remainder);
            }
        };

        struct CursorState
        {
            explicit CursorState(BitCursor& cursor) noexcept
                : cursor(cursor), inputCursor(cursor.inputCursor), inputLimit(cursor.inputLimit),
                  bitWindow(cursor.bitWindow), windowFill(cursor.windowFill)
            {
            }

            ~CursorState()
            {
                cursor.inputCursor = inputCursor;
                cursor.bitWindow = bitWindow;
                cursor.windowFill = windowFill;
            }

            bool ensureBits(const uint32_t count) noexcept
            {
                // Fast path: bulk load up to 8 bytes at once when possible
                if (windowFill < count)
                {
                    const auto bytesAvailable = static_cast<size_t>(inputLimit - inputCursor);

                    if (bytesAvailable >= 8 && windowFill <= 56)
                    {
                        // Bulk load 8 bytes (only use what fits in 64-bit window)
                        uint64_t bytes;
                        std::memcpy(&bytes, inputCursor, 8);
                        bitWindow |= bytes << windowFill;
                        const auto bytesToConsume = (64U - windowFill) / 8U;
                        inputCursor += bytesToConsume;
                        windowFill += bytesToConsume * 8U;
                    }
                    else
                    {
                        // Fallback: load one byte at a time
                        while (windowFill < count && inputCursor < inputLimit)
                        {
                            bitWindow |= static_cast<uint64_t>(*inputCursor++) << windowFill;
                            windowFill += 8;
                        }
                    }
                }

                return windowFill >= count;
            }

            [[nodiscard]] uint32_t peekBits(const uint32_t count) const noexcept
            {
                return static_cast<uint32_t>(bitWindow & ((UINT64_C(1) << count) - 1U));
            }

            void discardBits(const uint32_t count) noexcept
            {
                bitWindow >>= count;
                windowFill -= count;
            }

            bool pullBits(const uint32_t count, uint32_t& packedValue) noexcept
            {
                if (count == 0)
                {
                    packedValue = 0;
                    return true;
                }

                if (!ensureBits(count))
                    return false;

                packedValue = peekBits(count);
                discardBits(count);
                return true;
            }

            BitCursor& cursor;
            const uint8_t* inputCursor = nullptr;
            const uint8_t* inputLimit = nullptr;
            uint64_t bitWindow = 0;
            uint32_t windowFill = 0;
        };

        [[nodiscard]] inline uint16_t flipBits(uint16_t codeword, const uint8_t width) noexcept
        {
            uint16_t flipped = 0;

            for (uint8_t bitIndex = 0; bitIndex < width; ++bitIndex)
            {
                flipped = static_cast<uint16_t>((flipped << 1U) | (codeword & 1U));
                codeword >>= 1U;
            }

            return flipped;
        }

        struct DecodeTree
        {
            static constexpr uint32_t FAST_BITS = 10;
            static constexpr size_t FAST_SIZE = 1U << FAST_BITS;
            static constexpr size_t SECONDARY_SIZE = (1U << config::MAX_BITS) - FAST_SIZE;

            std::array<uint32_t, FAST_SIZE> fastTable = {};
            std::array<uint32_t, SECONDARY_SIZE> secondaryTable = {};
            std::array<uint16_t, FAST_SIZE> touchedFast = {};
            std::array<uint16_t, SECONDARY_SIZE> touchedSecondary = {};
            size_t touchedFastCount = 0;
            size_t touchedSecondaryCount = 0;
            size_t secondaryCount = 0;
            bool hasEntries = false;

            [[nodiscard]] static uint32_t packEntry(const uint16_t value, const uint8_t width,
                                                    const uint8_t extraBits = 0) noexcept
            {
                return static_cast<uint32_t>(value) | (static_cast<uint32_t>(width) << 16U) |
                       (static_cast<uint32_t>(extraBits) << 24U);
            }

            [[nodiscard]] static uint16_t entryValue(const uint32_t entry) noexcept
            {
                return static_cast<uint16_t>(entry & 0xffffU);
            }

            [[nodiscard]] static uint8_t entryWidth(const uint32_t entry) noexcept
            {
                return static_cast<uint8_t>((entry >> 16U) & 0xffU);
            }

            [[nodiscard]] static uint8_t entryExtraBits(const uint32_t entry) noexcept
            {
                return static_cast<uint8_t>((entry >> 24U) & 0xffU);
            }

            void reset() noexcept
            {
                for (size_t index = 0; index < touchedFastCount; ++index)
                    fastTable[touchedFast[index]] = 0;

                for (size_t index = 0; index < touchedSecondaryCount; ++index)
                    secondaryTable[touchedSecondary[index]] = 0;

                touchedFastCount = 0;
                touchedSecondaryCount = 0;
                secondaryCount = 0;
                hasEntries = false;
            }

            bool build(const uint8_t* widths, const size_t valueCount)
            {
                reset();

                auto widthHistogram = std::array<uint16_t, config::MAX_BITS + 1>{};

                for (size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex)
                {
                    const auto width = widths[valueIndex];

                    if (width == 0)
                        continue;

                    if (width > config::MAX_BITS)
                        return false;

                    hasEntries = true;
                    ++widthHistogram[width];
                }

                auto remainingSlots = 1;

                for (size_t width = 1; width <= config::MAX_BITS; ++width)
                {
                    remainingSlots <<= 1;
                    remainingSlots -= widthHistogram[width];

                    if (remainingSlots < 0)
                        return false;
                }

                auto firstCodeByWidth = std::array<uint16_t, config::MAX_BITS + 1>{};
                uint16_t runningCode = 0;

                for (size_t width = 1; width <= config::MAX_BITS; ++width)
                {
                    runningCode = static_cast<uint16_t>((runningCode + widthHistogram[width - 1]) << 1U);
                    firstCodeByWidth[width] = runningCode;
                }

                auto subBitsByPrefix = std::array<uint8_t, FAST_SIZE>{};
                auto nextCode = firstCodeByWidth;

                for (size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex)
                {
                    const auto width = widths[valueIndex];

                    if (width == 0)
                        continue;

                    if (width <= FAST_BITS)
                    {
                        ++nextCode[width];
                        continue;
                    }

                    const auto codeword = flipBits(nextCode[width]++, width);
                    const auto prefix = static_cast<size_t>(codeword & ((1U << FAST_BITS) - 1U));
                    const auto subBits = static_cast<uint8_t>(width - FAST_BITS);

                    if (subBitsByPrefix[prefix] < subBits)
                        subBitsByPrefix[prefix] = subBits;
                }

                for (size_t prefix = 0; prefix < FAST_SIZE; ++prefix)
                {
                    const auto subBits = subBitsByPrefix[prefix];

                    if (subBits == 0)
                        continue;

                    const auto subSize = size_t{1} << subBits;

                    if (secondaryCount + subSize > secondaryTable.size())
                        return false;

                    if (touchedFastCount >= FAST_SIZE)
                        return false;

                    touchedFast[touchedFastCount++] = static_cast<uint16_t>(prefix);
                    fastTable[prefix] = packEntry(static_cast<uint16_t>(secondaryCount), FAST_BITS, subBits);
                    secondaryCount += subSize;
                }

                nextCode = firstCodeByWidth;

                for (size_t valueIndex = 0; valueIndex < valueCount; ++valueIndex)
                {
                    const auto width = widths[valueIndex];

                    if (width == 0)
                        continue;

                    const auto codeword = flipBits(nextCode[width]++, width);
                    const auto packed = packEntry(static_cast<uint16_t>(valueIndex), width);

                    if (width <= FAST_BITS)
                    {
                        const auto fillCount = 1U << (FAST_BITS - width);

                        for (uint32_t fillIndex = 0; fillIndex < fillCount; ++fillIndex)
                        {
                            const auto tableIndex = static_cast<size_t>(codeword | (fillIndex << width));

                            if (fastTable[tableIndex] != 0)
                                return false;

                            if (touchedFastCount >= FAST_SIZE)
                                return false;

                            touchedFast[touchedFastCount++] = static_cast<uint16_t>(tableIndex);
                            fastTable[tableIndex] = packed;
                        }
                    }
                    else
                    {
                        const auto prefix = static_cast<size_t>(codeword & ((1U << FAST_BITS) - 1U));
                        const auto fastEntry = fastTable[prefix];
                        const auto subBits = entryExtraBits(fastEntry);
                        const auto subBase = static_cast<size_t>(entryValue(fastEntry));
                        const auto tailWidth = static_cast<uint8_t>(width - FAST_BITS);
                        const auto tailCode = static_cast<uint32_t>(codeword >> FAST_BITS);
                        const auto fillCount = 1U << (subBits - tailWidth);

                        for (uint32_t fillIndex = 0; fillIndex < fillCount; ++fillIndex)
                        {
                            const auto subIndex = subBase + static_cast<size_t>(tailCode | (fillIndex << tailWidth));

                            if (secondaryTable[subIndex] != 0)
                                return false;

                            if (touchedSecondaryCount >= SECONDARY_SIZE)
                                return false;

                            touchedSecondary[touchedSecondaryCount++] = static_cast<uint16_t>(subIndex);
                            secondaryTable[subIndex] = packed;
                        }
                    }
                }

                return true;
            }

            ptrdiff_t decode(BitCursor& cursor) const noexcept
            {
                if (!hasEntries)
                    return invalid_huffman_table;

                const auto hasFastWindow = cursor.ensureBits(FAST_BITS);
                const auto available = hasFastWindow ? FAST_BITS : cursor.windowFill;

                if (available == 0)
                    return truncated_input;

                const auto fastEntry = fastTable[cursor.peekBits(available)];
                const auto fastExtra = entryExtraBits(fastEntry);

                if (fastExtra == 0)
                {
                    const auto width = static_cast<uint32_t>(entryWidth(fastEntry));

                    if (width == 0)
                        return hasFastWindow ? invalid_huffman_table : truncated_input;

                    if (width > available)
                        return truncated_input;

                    cursor.discardBits(width);
                    return static_cast<ptrdiff_t>(entryValue(fastEntry));
                }

                if (!hasFastWindow)
                    return truncated_input;

                const auto totalBits = static_cast<uint32_t>(FAST_BITS + fastExtra);
                const auto hasFullSubtable = cursor.ensureBits(totalBits);
                const auto availableTotal = hasFullSubtable ? totalBits : cursor.windowFill;

                if (availableTotal < FAST_BITS)
                    return truncated_input;

                const auto tailValue = cursor.peekBits(availableTotal) >> FAST_BITS;
                const auto tailIndex = static_cast<size_t>(tailValue);
                const auto subIndex = static_cast<size_t>(entryValue(fastEntry)) + tailIndex;
                const auto subEntry = secondaryTable[subIndex];
                const auto width = static_cast<uint32_t>(entryWidth(subEntry));

                if (width == 0)
                    return hasFullSubtable ? invalid_huffman_table : truncated_input;

                if (width > availableTotal)
                    return truncated_input;

                cursor.discardBits(width);
                return static_cast<ptrdiff_t>(entryValue(subEntry));
            }
        };

        inline ptrdiff_t decodeTreeValue(const DecodeTree& tree, CursorState& state) noexcept
        {
            if (!tree.hasEntries)
                return invalid_huffman_table;

            if (state.ensureBits(config::MAX_BITS))
            {
                const auto allBits = state.peekBits(config::MAX_BITS);
                const auto fastEntry = tree.fastTable[allBits & ((1U << DecodeTree::FAST_BITS) - 1U)];
                const auto fastExtra = DecodeTree::entryExtraBits(fastEntry);

                if (fastExtra == 0)
                {
                    const auto width = static_cast<uint32_t>(DecodeTree::entryWidth(fastEntry));

                    if (width == 0)
                        return invalid_huffman_table;

                    state.discardBits(width);
                    return static_cast<ptrdiff_t>(DecodeTree::entryValue(fastEntry));
                }

                const auto subIndex =
                    static_cast<size_t>(DecodeTree::entryValue(fastEntry)) +
                    static_cast<size_t>((allBits >> DecodeTree::FAST_BITS) & ((1U << fastExtra) - 1U));
                const auto subEntry = tree.secondaryTable[subIndex];
                const auto width = static_cast<uint32_t>(DecodeTree::entryWidth(subEntry));

                if (width == 0)
                    return invalid_huffman_table;

                state.discardBits(width);
                return static_cast<ptrdiff_t>(DecodeTree::entryValue(subEntry));
            }

            const auto hasFastWindow = state.ensureBits(DecodeTree::FAST_BITS);
            const auto available = hasFastWindow ? DecodeTree::FAST_BITS : state.windowFill;

            if (available == 0)
                return truncated_input;

            const auto fastEntry = tree.fastTable[state.peekBits(available)];
            const auto fastExtra = DecodeTree::entryExtraBits(fastEntry);

            if (fastExtra == 0)
            {
                const auto width = static_cast<uint32_t>(DecodeTree::entryWidth(fastEntry));

                if (width == 0)
                    return hasFastWindow ? invalid_huffman_table : truncated_input;

                if (width > available)
                    return truncated_input;

                state.discardBits(width);
                return static_cast<ptrdiff_t>(DecodeTree::entryValue(fastEntry));
            }

            if (!hasFastWindow)
                return truncated_input;

            const auto totalBits = static_cast<uint32_t>(DecodeTree::FAST_BITS + fastExtra);
            const auto hasFullSubtable = state.ensureBits(totalBits);
            const auto availableTotal = hasFullSubtable ? totalBits : state.windowFill;

            if (availableTotal < DecodeTree::FAST_BITS)
                return truncated_input;

            const auto tailValue = state.peekBits(availableTotal) >> DecodeTree::FAST_BITS;
            const auto subIndex =
                static_cast<size_t>(DecodeTree::entryValue(fastEntry)) + static_cast<size_t>(tailValue);
            const auto subEntry = tree.secondaryTable[subIndex];
            const auto width = static_cast<uint32_t>(DecodeTree::entryWidth(subEntry));

            if (width == 0)
                return hasFullSubtable ? invalid_huffman_table : truncated_input;

            if (width > availableTotal)
                return truncated_input;

            state.discardBits(width);
            return static_cast<ptrdiff_t>(DecodeTree::entryValue(subEntry));
        }

        [[nodiscard]] inline uint32_t loadU32be(const uint8_t* data) noexcept
        {
            return (static_cast<uint32_t>(data[0]) << 24U) | (static_cast<uint32_t>(data[1]) << 16U) |
                   (static_cast<uint32_t>(data[2]) << 8U) | static_cast<uint32_t>(data[3]);
        }

        [[nodiscard]] inline uint32_t checksumAdler(const uint8_t* data, const size_t size) noexcept
        {
            uint32_t s1 = 1;
            uint32_t s2 = 0;
            size_t remaining = size;

            while (remaining != 0)
            {
                auto chunk = remaining > 5552 ? 5552 : remaining;
                remaining -= chunk;

                while (chunk >= 16)
                {
                    s1 += data[0];
                    s2 += s1;
                    s1 += data[1];
                    s2 += s1;
                    s1 += data[2];
                    s2 += s1;
                    s1 += data[3];
                    s2 += s1;
                    s1 += data[4];
                    s2 += s1;
                    s1 += data[5];
                    s2 += s1;
                    s1 += data[6];
                    s2 += s1;
                    s1 += data[7];
                    s2 += s1;
                    s1 += data[8];
                    s2 += s1;
                    s1 += data[9];
                    s2 += s1;
                    s1 += data[10];
                    s2 += s1;
                    s1 += data[11];
                    s2 += s1;
                    s1 += data[12];
                    s2 += s1;
                    s1 += data[13];
                    s2 += s1;
                    s1 += data[14];
                    s2 += s1;
                    s1 += data[15];
                    s2 += s1;
                    data += 16;
                    chunk -= 16;
                }

                while (chunk-- != 0)
                {
                    s1 += *data++;
                    s2 += s1;
                }

                s1 %= config::ADLER_MOD;
                s2 %= config::ADLER_MOD;
            }

            return (s2 << 16U) | s1;
        }

        [[nodiscard]] inline uint64_t hashWidths(const uint8_t* widths, const size_t count, const size_t litCount,
                                                 const size_t distCount) noexcept
        {
            auto hash = UINT64_C(1469598103934665603);

            hash ^= static_cast<uint64_t>(litCount);
            hash *= UINT64_C(1099511628211);
            hash ^= static_cast<uint64_t>(distCount);
            hash *= UINT64_C(1099511628211);

            for (size_t index = 0; index < count; ++index)
            {
                hash ^= static_cast<uint64_t>(widths[index]);
                hash *= UINT64_C(1099511628211);
            }

            return hash;
        }

        struct TreeCacheSlot
        {
            bool valid = false;
            uint32_t age = 0;
            size_t litCount = 0;
            size_t distCount = 0;
            uint64_t signature = 0;
            std::array<uint8_t, config::LITERAL_LENGTH_SYMBOLS + config::DISTANCE_SYMBOLS> widths = {};
            DecodeTree litLenTree;
            DecodeTree distTree;
        };

        struct WorkingSet
        {
            static constexpr size_t TREE_CACHE_CAPACITY = 16;

            DecodeTree codeLenTree;
            std::array<uint8_t, config::LITERAL_LENGTH_SYMBOLS + config::DISTANCE_SYMBOLS> mergedWidths = {};
            std::array<TreeCacheSlot, TREE_CACHE_CAPACITY> treeCache = {};
            const DecodeTree* activeLitLenTree = nullptr;
            const DecodeTree* activeDistTree = nullptr;
            uint32_t cacheAge = 0;
        };

        inline bool buildStaticTrees(DecodeTree& litLenTree, DecodeTree& distTree)
        {
            auto litLenWidths = std::array<uint8_t, config::LITERAL_LENGTH_SYMBOLS>{};
            auto distWidths = std::array<uint8_t, config::DISTANCE_SYMBOLS>{};

            for (size_t valueIndex = 0; valueIndex <= 143; ++valueIndex)
                litLenWidths[valueIndex] = config::FIXED_LITLEN_BITS_0_143;

            for (size_t valueIndex = 144; valueIndex <= 255; ++valueIndex)
                litLenWidths[valueIndex] = config::FIXED_LITLEN_BITS_144_255;

            for (size_t valueIndex = 256; valueIndex <= 279; ++valueIndex)
                litLenWidths[valueIndex] = config::FIXED_LITLEN_BITS_256_279;

            for (size_t valueIndex = 280; valueIndex <= 287; ++valueIndex)
                litLenWidths[valueIndex] = config::FIXED_LITLEN_BITS_280_287;

            distWidths.fill(config::FIXED_DISTANCE_BITS);

            return litLenTree.build(litLenWidths.data(), litLenWidths.size()) &&
                   distTree.build(distWidths.data(), distWidths.size());
        }

        inline ptrdiff_t parseDynamicTrees(BitCursor& cursor, WorkingSet& work)
        {
            auto state = CursorState(cursor);
            uint32_t litField = 0;
            uint32_t distField = 0;
            uint32_t codeLenField = 0;

            if (!state.pullBits(5, litField) || !state.pullBits(5, distField) || !state.pullBits(4, codeLenField))
                return truncated_input;

            const auto litCount = static_cast<size_t>(litField) + 257;
            const auto distCount = static_cast<size_t>(distField) + 1;
            const auto codeLenCount = static_cast<size_t>(codeLenField) + 4;

            auto codeLenWidths = std::array<uint8_t, config::CODE_LENGTH_CODES>{};

            for (size_t codeIndex = 0; codeIndex < codeLenCount; ++codeIndex)
            {
                uint32_t widthField = 0;

                if (!state.pullBits(3, widthField))
                    return truncated_input;

                codeLenWidths[config::CODE_LENGTH_ORDER[codeIndex]] = static_cast<uint8_t>(widthField);
            }

            if (!work.codeLenTree.build(codeLenWidths.data(), codeLenWidths.size()))
                return invalid_huffman_table;

            auto& mergedWidths = work.mergedWidths;

            for (size_t writeIndex = 0; writeIndex < litCount + distCount;)
            {
                const auto decodedValue = decodeTreeValue(work.codeLenTree, state);

                if (decodedValue < 0)
                    return decodedValue;

                if (decodedValue <= 15)
                {
                    mergedWidths[writeIndex++] = static_cast<uint8_t>(decodedValue);
                    continue;
                }

                uint32_t extraField = 0;
                uint8_t repeatCount = 0;
                uint8_t repeatedWidth = 0;

                switch (decodedValue)
                {
                    case 16:
                        if (writeIndex == 0)
                            return invalid_code_length;

                        if (!state.pullBits(2, extraField))
                            return truncated_input;

                        repeatCount = static_cast<uint8_t>(3 + extraField);
                        repeatedWidth = mergedWidths[writeIndex - 1];
                        break;
                    case 17:
                        if (!state.pullBits(3, extraField))
                            return truncated_input;

                        repeatCount = static_cast<uint8_t>(3 + extraField);
                        repeatedWidth = 0;
                        break;
                    case 18:
                        if (!state.pullBits(7, extraField))
                            return truncated_input;

                        repeatCount = static_cast<uint8_t>(11 + extraField);
                        repeatedWidth = 0;
                        break;
                    default:
                        return invalid_code_length;
                }

                if (writeIndex + repeatCount > litCount + distCount)
                    return invalid_code_length;

                std::memset(mergedWidths.data() + writeIndex, repeatedWidth, repeatCount);
                writeIndex += repeatCount;
            }

            if (mergedWidths[256] == 0)
                return invalid_huffman_table;

            const auto totalCount = litCount + distCount;
            const auto signature = hashWidths(mergedWidths.data(), totalCount, litCount, distCount);

            for (auto& slot : work.treeCache)
            {
                if (!slot.valid || slot.litCount != litCount || slot.distCount != distCount ||
                    slot.signature != signature)
                    continue;

                if (std::memcmp(slot.widths.data(), mergedWidths.data(), totalCount) != 0)
                    continue;

                slot.age = ++work.cacheAge;
                work.activeLitLenTree = &slot.litLenTree;
                work.activeDistTree = &slot.distTree;
                return 0;
            }

            auto* victim = &work.treeCache[0];

            for (auto& slot : work.treeCache)
            {
                if (!slot.valid)
                {
                    victim = &slot;
                    break;
                }

                if (slot.age < victim->age)
                    victim = &slot;
            }

            if (!victim->litLenTree.build(mergedWidths.data(), litCount))
                return invalid_huffman_table;

            if (!victim->distTree.build(mergedWidths.data() + litCount, distCount))
                return invalid_huffman_table;

            std::memcpy(victim->widths.data(), mergedWidths.data(), totalCount);
            victim->valid = true;
            victim->age = ++work.cacheAge;
            victim->litCount = litCount;
            victim->distCount = distCount;
            victim->signature = signature;
            work.activeLitLenTree = &victim->litLenTree;
            work.activeDistTree = &victim->distTree;

            return 0;
        }

        inline ptrdiff_t decodeHuffmanBlock(BitCursor& cursor, const DecodeTree& litLenTree, const DecodeTree& distTree,
                                            uint8_t* dst, const size_t dstSize, size_t& writeCursor)
        {
            auto state = CursorState(cursor);
            auto* writePtr = dst + writeCursor;
            auto* const writeLimit = dst + dstSize;

            for (;;)
            {
                const auto decodedToken = decodeTreeValue(litLenTree, state);

                if (decodedToken < 0)
                    return decodedToken;

                if (decodedToken < 256)
                {
                    if (writePtr >= writeLimit)
                        return output_overrun;

                    *writePtr++ = static_cast<uint8_t>(decodedToken);
                    continue;
                }

                if (decodedToken == 256)
                {
                    writeCursor = static_cast<size_t>(writePtr - dst);
                    return 0;
                }

                if (decodedToken > 285)
                    return invalid_length_code;

                const auto lengthSlot = static_cast<size_t>(decodedToken - 257);
                uint32_t lengthExtra = 0;

                if (!state.pullBits(config::LENGTH_EXTRA_BITS[lengthSlot], lengthExtra))
                    return truncated_input;

                const auto copyCount =
                    static_cast<size_t>(config::LENGTH_BASES[lengthSlot] + static_cast<int>(lengthExtra));
                const auto distanceToken = decodeTreeValue(distTree, state);

                if (distanceToken < 0)
                    return distanceToken;

                if (distanceToken > 29)
                    return invalid_distance_code;

                uint32_t distanceExtra = 0;
                const auto distanceSlot = static_cast<size_t>(distanceToken);

                if (!state.pullBits(config::DISTANCE_EXTRA_BITS[distanceSlot], distanceExtra))
                    return truncated_input;

                const auto backOffset =
                    static_cast<size_t>(config::DISTANCE_BASES[distanceSlot] + static_cast<int>(distanceExtra));
                const auto writtenBytes = static_cast<size_t>(writePtr - dst);

                if (backOffset == 0 || backOffset > writtenBytes)
                    return invalid_back_reference;

                if (copyCount > static_cast<size_t>(writeLimit - writePtr))
                    return output_overrun;

                if (backOffset == 1)
                {
                    std::memset(writePtr, writePtr[-1], copyCount);
                    writePtr += copyCount;
                    continue;
                }

                if (copyCount <= backOffset)
                {
                    std::memcpy(writePtr, writePtr - backOffset, copyCount);
                    writePtr += copyCount;
                    continue;
                }

                auto produced = backOffset;
                auto remainingCopy = copyCount;
                std::memcpy(writePtr, writePtr - backOffset, backOffset);
                writePtr += backOffset;
                remainingCopy -= backOffset;

                while (remainingCopy != 0)
                {
                    const auto chunk = remainingCopy < produced ? remainingCopy : produced;
                    std::memcpy(writePtr, writePtr - produced, chunk);
                    writePtr += chunk;
                    remainingCopy -= chunk;
                    produced += chunk;
                }
            }
        }

        struct FixedTrees
        {
            DecodeTree litLen;
            DecodeTree dist;
            bool valid = false;

            FixedTrees()
            {
                valid = buildStaticTrees(litLen, dist);
            }
        };

        [[nodiscard]] inline const FixedTrees& fixedTrees() noexcept
        {
            static const FixedTrees trees;
            return trees;
        }

    }

    struct InflateContext
    {
        InflateContext() : state(std::make_unique<detail::WorkingSet>())
        {
        }

        InflateContext(InflateContext&&) noexcept = default;
        InflateContext& operator=(InflateContext&&) noexcept = default;
        InflateContext(const InflateContext&) = delete;
        InflateContext& operator=(const InflateContext&) = delete;

        [[nodiscard]] detail::WorkingSet& work() noexcept
        {
            return *state;
        }

      private:
        std::unique_ptr<detail::WorkingSet> state;
    };

    // Exact-output callers supply the expected uncompressed size. Capacity-based
    // callers can instead retrieve the actual size after the complete zlib stream,
    // including its checksum, has been verified.
    inline ptrdiff_t inflateImpl(InflateContext& context, void* dst, const size_t dstSize, const void* src,
                                 const size_t srcSize, const bool requireExactOutputSize)
    {
        using namespace detail;

        if (dstSize > static_cast<size_t>((std::numeric_limits<ptrdiff_t>::max)()))
            return inflate_error::destination_too_large;

        if ((dstSize != 0 && dst == nullptr) || src == nullptr)
            return inflate_error::invalid_argument;

        if (srcSize < 6)
            return inflate_error::truncated_input;

        const auto* inputBytes = static_cast<const uint8_t*>(src);
        auto* outputBytes = static_cast<uint8_t*>(dst);
        const auto headerCmf = inputBytes[0];
        const auto headerFlg = inputBytes[1];

        if ((headerCmf & 0x0FU) != 8U || (headerCmf >> 4U) > 7U ||
            (((static_cast<uint16_t>(headerCmf) << 8U) | headerFlg) % 31U) != 0U)
            return inflate_error::invalid_wrapper_header;

        if ((headerFlg & 0x20U) != 0U)
            return inflate_error::unsupported_wrapper_feature;

        const auto payloadBytes = srcSize - 6;
        auto cursor = BitCursor(inputBytes + 2, payloadBytes);
        auto& working = context.work();
        size_t writePos = 0;
        auto reachedFinalBlock = false;

        do
        {
            uint32_t lastFlag = 0;
            uint32_t blockKind = 0;

            if (!cursor.pullBits(1, lastFlag) || !cursor.pullBits(2, blockKind))
                return inflate_error::truncated_input;

            reachedFinalBlock = lastFlag != 0;

            switch (blockKind)
            {
                case 0:
                {
                    cursor.clearByteRemainder();

                    uint32_t storedLen = 0;
                    uint32_t storedLenCheck = 0;

                    if (!cursor.pullBits(16, storedLen) || !cursor.pullBits(16, storedLenCheck))
                        return inflate_error::truncated_input;

                    if ((storedLen ^ 0xFFFFU) != storedLenCheck)
                        return inflate_error::invalid_stored_block;

                    if (static_cast<size_t>(storedLen) > dstSize - writePos)
                        return inflate_error::output_overrun;

                    for (uint32_t byteIndex = 0; byteIndex < storedLen; ++byteIndex)
                    {
                        uint32_t plainByte = 0;

                        if (!cursor.pullBits(8, plainByte))
                            return inflate_error::truncated_input;

                        outputBytes[writePos++] = static_cast<uint8_t>(plainByte);
                    }

                    break;
                }
                case 1:
                case 2:
                {
                    const DecodeTree* litLenTree = nullptr;
                    const DecodeTree* distTree = nullptr;

                    if (blockKind == 1)
                    {
                        const auto& trees = fixedTrees();

                        if (!trees.valid)
                            return inflate_error::invalid_huffman_table;

                        litLenTree = &trees.litLen;
                        distTree = &trees.dist;
                    }
                    else
                    {
                        const auto treeStatus = parseDynamicTrees(cursor, working);

                        if (treeStatus < 0)
                            return treeStatus;

                        litLenTree = working.activeLitLenTree;
                        distTree = working.activeDistTree;
                    }

                    const auto blockStatus =
                        decodeHuffmanBlock(cursor, *litLenTree, *distTree, outputBytes, dstSize, writePos);

                    if (blockStatus < 0)
                        return blockStatus;

                    break;
                }
                default:
                    return inflate_error::invalid_block_type;
            }
        }
        while (!reachedFinalBlock);

        if (requireExactOutputSize && writePos != dstSize)
            return inflate_error::output_underrun;

        const auto payloadUsed = (cursor.usedBits() + 7U) / 8U;

        if (payloadUsed != payloadBytes)
            return inflate_error::trailing_garbage;

        const auto expectedChecksum = loadU32be(inputBytes + srcSize - 4);
        const auto actualChecksum = checksumAdler(outputBytes, writePos);

        if (expectedChecksum != actualChecksum)
            return inflate_error::checksum_mismatch;

        return static_cast<ptrdiff_t>(writePos);
    }

    // Decode into an exactly-sized destination.  This is the original public
    // contract used by callers that know the decoded size.
    inline ptrdiff_t inflate(InflateContext& context, void* dst, const size_t dstSize, const void* src,
                             const size_t srcSize)
    {
        return inflateImpl(context, dst, dstSize, src, srcSize, true);
    }

    // Decode into a destination with at least enough capacity and return the
    // actual decoded byte count.  A too-small destination still reports
    // `output_overrun`; successful calls validate the complete stream, trailing
    // bytes, and Adler-32 just like `inflate`.
    //
    // This is deliberately opt-in for streams whose decoded size is not known in
    // advance, avoiding repeated full DEFLATE passes while sizing the destination.
    inline ptrdiff_t inflateActualSize(InflateContext& context, void* dst, const size_t dstCapacity, const void* src,
                                       const size_t srcSize)
    {
        return inflateImpl(context, dst, dstCapacity, src, srcSize, false);
    }

    inline ptrdiff_t inflate(void* dst, const size_t dstSize, const void* src, const size_t srcSize)
    {
        auto context = InflateContext{};
        return inflate(context, dst, dstSize, src, srcSize);
    }

    struct DeflateContext;

    namespace deflate_error
    {
        inline constexpr ptrdiff_t invalid_argument = -101;
        inline constexpr ptrdiff_t destination_too_small = -102;
        inline constexpr ptrdiff_t destination_too_large = -103;
        inline constexpr ptrdiff_t failed_to_flush = -104;
        inline constexpr ptrdiff_t unsupported_input = -105;
    }

    namespace deflate_config
    {
        inline constexpr uint32_t ADLER_MOD = 65521U;
        inline constexpr size_t WINDOW_SIZE = 32768;
        inline constexpr size_t MIN_MATCH = 3;
        inline constexpr size_t MAX_MATCH = 258;
        inline constexpr size_t MAX_BITS = 15;
        inline constexpr size_t HASH_BITS = 15;
        inline constexpr size_t HASH_SIZE = size_t{1} << HASH_BITS;
        inline constexpr size_t MAX_CHAIN = 32;
        inline constexpr size_t LITERAL_LENGTH_SYMBOLS = 288;
        inline constexpr size_t DISTANCE_SYMBOLS = 32;
        inline constexpr size_t LENGTH_CODES = 29;
        inline constexpr size_t DISTANCE_CODES = 30;
        inline constexpr std::array<int, LENGTH_CODES> LENGTH_BASES = {{3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                                                        15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                                                        67, 83, 99, 115, 131, 163, 195, 227, 258}};
        inline constexpr std::array<uint8_t, LENGTH_CODES> LENGTH_EXTRA_BITS = {
            {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0}};
        inline constexpr std::array<int, DISTANCE_CODES> DISTANCE_BASES = {
            {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}};
        inline constexpr std::array<uint8_t, DISTANCE_CODES> DISTANCE_EXTRA_BITS = {
            {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13}};
        inline constexpr uint8_t FIXED_LITLEN_BITS_0_143 = 8;
        inline constexpr uint8_t FIXED_LITLEN_BITS_144_255 = 9;
        inline constexpr uint8_t FIXED_LITLEN_BITS_256_279 = 7;
        inline constexpr uint8_t FIXED_LITLEN_BITS_280_287 = 8;
        inline constexpr uint8_t FIXED_DISTANCE_BITS = 5;
    }

    namespace deflate_detail
    {
        using namespace zlib::deflate_error;

        struct WorkingSet;

        [[nodiscard]] inline uint16_t flipBits(uint16_t codeword, const uint8_t width) noexcept
        {
            uint16_t flipped = 0;

            for (uint8_t bitIndex = 0; bitIndex < width; ++bitIndex)
            {
                flipped = static_cast<uint16_t>((flipped << 1U) | (codeword & 1U));
                codeword >>= 1U;
            }

            return flipped;
        }

        struct BitPacker
        {
            uint8_t* outputBegin = nullptr;
            uint8_t* outputCursor = nullptr;
            uint8_t* outputLimit = nullptr;
            uint64_t bitWindow = 0;
            uint32_t windowFill = 0;

            BitPacker(uint8_t* data, const size_t size) noexcept
                : outputBegin(data), outputCursor(data), outputLimit(data + size)
            {
            }

            bool flushWholeBytes() noexcept
            {
                // Flush complete bytes from the bit window
                while (windowFill >= 8)
                {
                    if (outputCursor >= outputLimit)
                        return false;

                    *outputCursor++ = static_cast<uint8_t>(bitWindow & 0xffU);
                    bitWindow >>= 8;
                    windowFill -= 8;
                }

                return true;
            }

            bool pushBits(const uint32_t packedValue, const uint32_t count) noexcept
            {
                if (count == 0)
                    return true;

                bitWindow |= static_cast<uint64_t>(packedValue) << windowFill;
                windowFill += count;
                return flushWholeBytes();
            }

            bool finish() noexcept
            {
                if (windowFill != 0)
                {
                    if (outputCursor >= outputLimit)
                        return false;

                    *outputCursor++ = static_cast<uint8_t>(bitWindow & 0xffU);
                    bitWindow = 0;
                    windowFill = 0;
                }

                return true;
            }

            [[nodiscard]] size_t bytesWritten() const noexcept
            {
                return static_cast<size_t>(outputCursor - outputBegin);
            }
        };

        [[nodiscard]] inline uint32_t checksumAdler(const uint8_t* data, const size_t size) noexcept
        {
            uint32_t s1 = 1;
            uint32_t s2 = 0;
            size_t remaining = size;

            while (remaining != 0)
            {
                auto chunk = remaining > 5552 ? 5552 : remaining;
                remaining -= chunk;

                while (chunk >= 16)
                {
                    s1 += data[0];
                    s2 += s1;
                    s1 += data[1];
                    s2 += s1;
                    s1 += data[2];
                    s2 += s1;
                    s1 += data[3];
                    s2 += s1;
                    s1 += data[4];
                    s2 += s1;
                    s1 += data[5];
                    s2 += s1;
                    s1 += data[6];
                    s2 += s1;
                    s1 += data[7];
                    s2 += s1;
                    s1 += data[8];
                    s2 += s1;
                    s1 += data[9];
                    s2 += s1;
                    s1 += data[10];
                    s2 += s1;
                    s1 += data[11];
                    s2 += s1;
                    s1 += data[12];
                    s2 += s1;
                    s1 += data[13];
                    s2 += s1;
                    s1 += data[14];
                    s2 += s1;
                    s1 += data[15];
                    s2 += s1;
                    data += 16;
                    chunk -= 16;
                }

                while (chunk-- != 0)
                {
                    s1 += *data++;
                    s2 += s1;
                }

                s1 %= deflate_config::ADLER_MOD;
                s2 %= deflate_config::ADLER_MOD;
            }

            return (s2 << 16U) | s1;
        }

        inline void storeU32be(uint8_t* dst, const uint32_t value) noexcept
        {
            dst[0] = static_cast<uint8_t>(value >> 24U);
            dst[1] = static_cast<uint8_t>(value >> 16U);
            dst[2] = static_cast<uint8_t>(value >> 8U);
            dst[3] = static_cast<uint8_t>(value);
        }

        struct FixedEncoderTables
        {
            std::array<uint16_t, deflate_config::LITERAL_LENGTH_SYMBOLS> litLenCodes = {};
            std::array<uint8_t, deflate_config::LITERAL_LENGTH_SYMBOLS> litLenBits = {};
            std::array<uint16_t, deflate_config::DISTANCE_SYMBOLS> distCodes = {};
            std::array<uint8_t, deflate_config::DISTANCE_SYMBOLS> distBits = {};

            FixedEncoderTables()
            {
                auto litLenWidths = std::array<uint8_t, deflate_config::LITERAL_LENGTH_SYMBOLS>{};
                auto distWidths = std::array<uint8_t, deflate_config::DISTANCE_SYMBOLS>{};

                for (size_t valueIndex = 0; valueIndex <= 143; ++valueIndex)
                    litLenWidths[valueIndex] = deflate_config::FIXED_LITLEN_BITS_0_143;

                for (size_t valueIndex = 144; valueIndex <= 255; ++valueIndex)
                    litLenWidths[valueIndex] = deflate_config::FIXED_LITLEN_BITS_144_255;

                for (size_t valueIndex = 256; valueIndex <= 279; ++valueIndex)
                    litLenWidths[valueIndex] = deflate_config::FIXED_LITLEN_BITS_256_279;

                for (size_t valueIndex = 280; valueIndex <= 287; ++valueIndex)
                    litLenWidths[valueIndex] = deflate_config::FIXED_LITLEN_BITS_280_287;

                distWidths.fill(deflate_config::FIXED_DISTANCE_BITS);
                buildCodes(litLenWidths, litLenCodes, litLenBits);
                buildCodes(distWidths, distCodes, distBits);
            }

            template <size_t Count>
            static void buildCodes(const std::array<uint8_t, Count>& widths, std::array<uint16_t, Count>& codes,
                                   std::array<uint8_t, Count>& bits) noexcept
            {
                auto widthHistogram = std::array<uint16_t, deflate_config::MAX_BITS + 1>{};

                for (auto width : widths)
                {
                    if (width != 0)
                        ++widthHistogram[width];
                }

                auto nextCode = std::array<uint16_t, deflate_config::MAX_BITS + 1>{};
                uint16_t runningCode = 0;

                for (size_t width = 1; width <= deflate_config::MAX_BITS; ++width)
                {
                    runningCode = static_cast<uint16_t>((runningCode + widthHistogram[width - 1]) << 1U);
                    nextCode[width] = runningCode;
                }

                for (size_t symbol = 0; symbol < Count; ++symbol)
                {
                    const auto width = widths[symbol];
                    bits[symbol] = width;

                    if (width == 0)
                        continue;

                    codes[symbol] = flipBits(nextCode[width]++, width);
                }
            }
        };

        [[nodiscard]] inline const FixedEncoderTables& fixedTables() noexcept
        {
            static const FixedEncoderTables tables;
            return tables;
        }

        struct Match
        {
            size_t length = 0;
            size_t distance = 0;
        };

        struct WorkingSet
        {
            std::vector<int32_t> hashHead;
            std::vector<int32_t> hashPrev;
        };

        [[nodiscard]] inline uint32_t hashBytes(const uint8_t* data) noexcept
        {
            const auto value = (static_cast<uint32_t>(data[0]) << 16U) | (static_cast<uint32_t>(data[1]) << 8U) |
                               static_cast<uint32_t>(data[2]);
            return (value * 2654435761U) >> (32U - deflate_config::HASH_BITS);
        }

        inline void prepareChains(WorkingSet& work, const size_t inputSize)
        {
            work.hashHead.assign(deflate_config::HASH_SIZE, -1);
            work.hashPrev.assign(inputSize, -1);
        }

        inline void insertPosition(WorkingSet& work, const uint8_t* src, const size_t srcSize,
                                   const size_t pos) noexcept
        {
            if (pos + deflate_config::MIN_MATCH > srcSize)
                return;

            const auto hash = hashBytes(src + pos);
            work.hashPrev[pos] = work.hashHead[hash];
            work.hashHead[hash] = static_cast<int32_t>(pos);
        }

        [[nodiscard]] inline Match findMatch(const WorkingSet& work, const uint8_t* src, const size_t srcSize,
                                             const size_t pos) noexcept
        {
            if (pos + deflate_config::MIN_MATCH > srcSize)
                return {};

            const auto hash = hashBytes(src + pos);
            auto candidate = work.hashHead[hash];
            auto best = Match{};
            const auto remaining = srcSize - pos;
            const auto maxLength = remaining < deflate_config::MAX_MATCH ? remaining : deflate_config::MAX_MATCH;
            size_t depth = 0;

            while (candidate >= 0 && depth < deflate_config::MAX_CHAIN)
            {
                const auto candidatePos = static_cast<size_t>(candidate);
                const auto distance = pos - candidatePos;

                if (distance > deflate_config::WINDOW_SIZE)
                    break;

                size_t length = 0;

                while (length < maxLength && src[candidatePos + length] == src[pos + length])
                    ++length;

                if (length >= deflate_config::MIN_MATCH && length > best.length)
                {
                    best.length = length;
                    best.distance = distance;

                    if (length == deflate_config::MAX_MATCH)
                        break;
                }

                candidate = work.hashPrev[candidatePos];
                ++depth;
            }

            return best;
        }

        [[nodiscard]] inline bool writeLiteral(BitPacker& packer, const uint16_t value) noexcept
        {
            const auto& tables = fixedTables();
            return packer.pushBits(tables.litLenCodes[value], tables.litLenBits[value]);
        }

        [[nodiscard]] inline bool writeLengthDistance(BitPacker& packer, const size_t length,
                                                      const size_t distance) noexcept
        {
            const auto& tables = fixedTables();
            size_t lengthSlot = 0;

            while (lengthSlot + 1 < deflate_config::LENGTH_CODES &&
                   static_cast<size_t>(deflate_config::LENGTH_BASES[lengthSlot + 1]) <= length)
            {
                ++lengthSlot;
            }

            if (lengthSlot >= deflate_config::LENGTH_CODES)
                return false;

            const auto lengthSymbol = static_cast<uint16_t>(257 + lengthSlot);
            const auto lengthExtraBits = deflate_config::LENGTH_EXTRA_BITS[lengthSlot];
            const auto lengthExtraValue =
                static_cast<uint32_t>(length - static_cast<size_t>(deflate_config::LENGTH_BASES[lengthSlot]));

            if (!packer.pushBits(tables.litLenCodes[lengthSymbol], tables.litLenBits[lengthSymbol]))
                return false;

            if (!packer.pushBits(lengthExtraValue, lengthExtraBits))
                return false;

            size_t distanceSlot = 0;

            while (distanceSlot + 1 < deflate_config::DISTANCE_CODES &&
                   static_cast<size_t>(deflate_config::DISTANCE_BASES[distanceSlot + 1]) <= distance)
            {
                ++distanceSlot;
            }

            if (distanceSlot >= deflate_config::DISTANCE_CODES)
                return false;

            const auto distanceExtraBits = deflate_config::DISTANCE_EXTRA_BITS[distanceSlot];
            const auto distanceExtraValue =
                static_cast<uint32_t>(distance - static_cast<size_t>(deflate_config::DISTANCE_BASES[distanceSlot]));

            if (!packer.pushBits(tables.distCodes[distanceSlot], tables.distBits[distanceSlot]))
                return false;

            return packer.pushBits(distanceExtraValue, distanceExtraBits);
        }

        [[nodiscard]] inline ptrdiff_t writeFixedBlock(BitPacker& packer, WorkingSet& work, const uint8_t* src,
                                                       const size_t srcSize)
        {
            prepareChains(work, srcSize);

            if (!packer.pushBits(0x3U, 3))
                return destination_too_small;

            size_t readPos = 0;

            while (readPos < srcSize)
            {
                auto match = Match{};

                if (readPos + deflate_config::MIN_MATCH <= srcSize)
                    match = findMatch(work, src, srcSize, readPos);

                if (match.length >= deflate_config::MIN_MATCH)
                {
                    if (!writeLengthDistance(packer, match.length, match.distance))
                        return destination_too_small;

                    const auto limit = readPos + match.length;

                    while (readPos < limit)
                    {
                        insertPosition(work, src, srcSize, readPos);
                        ++readPos;
                    }

                    continue;
                }

                if (!writeLiteral(packer, src[readPos]))
                    return destination_too_small;

                insertPosition(work, src, srcSize, readPos);
                ++readPos;
            }

            if (!writeLiteral(packer, 256))
                return destination_too_small;

            return 0;
        }
    }

    struct DeflateContext
    {
        DeflateContext() : state(std::make_unique<deflate_detail::WorkingSet>())
        {
        }

        DeflateContext(DeflateContext&&) noexcept = default;
        DeflateContext& operator=(DeflateContext&&) noexcept = default;
        DeflateContext(const DeflateContext&) = delete;
        DeflateContext& operator=(const DeflateContext&) = delete;

        [[nodiscard]] deflate_detail::WorkingSet& work() noexcept
        {
            return *state;
        }

      private:
        std::unique_ptr<deflate_detail::WorkingSet> state;
    };

    [[nodiscard]] inline ptrdiff_t deflate(DeflateContext& context, void* dst, const size_t dstSize, const void* src,
                                           const size_t srcSize)
    {
        using namespace deflate_detail;

        if (srcSize > static_cast<size_t>((std::numeric_limits<ptrdiff_t>::max)()))
            return destination_too_large;

        if ((dstSize != 0 && dst == nullptr) || (srcSize != 0 && src == nullptr))
            return invalid_argument;

        if (dstSize < 6)
            return destination_too_small;

        const auto* inputBytes = static_cast<const uint8_t*>(src);
        auto* outputBytes = static_cast<uint8_t*>(dst);
        outputBytes[0] = 0x78;
        outputBytes[1] = 0x01;

        auto packer = BitPacker(outputBytes + 2, dstSize - 6);
        const auto blockStatus = writeFixedBlock(packer, context.work(), inputBytes, srcSize);

        if (blockStatus < 0)
            return blockStatus;

        if (!packer.finish())
            return failed_to_flush;

        const auto payloadSize = packer.bytesWritten();
        deflate_detail::storeU32be(outputBytes + 2 + payloadSize, deflate_detail::checksumAdler(inputBytes, srcSize));
        const auto totalSize = size_t{2} + payloadSize + 4;

        return static_cast<ptrdiff_t>(totalSize);
    }

    [[nodiscard]] inline ptrdiff_t deflate(void* dst, const size_t dstSize, const void* src, const size_t srcSize)
    {
        auto context = DeflateContext{};
        return deflate(context, dst, dstSize, src, srcSize);
    }
}
