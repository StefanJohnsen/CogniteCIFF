/*----------------------------------------------------------------
  WriteInflateFBX.h

  Header-only zlib/DEFLATE compressor for FBX array payloads.
  This file provides a standalone, dependency-free implementation
  for deflating data blocks to be written into binary FBX files.

  Requirements: C++20
  OS Support:   Windows/MacOS/Linux

  Normative format sources used for this implementation:
  1. RFC 1950, "ZLIB Compressed Data Format Specification version 3.3"
     https://www.rfc-editor.org/rfc/rfc1950.html
     Used for:
     - zlib wrapper layout (CMF/FLG header bytes)
     - Adler-32 checksum encoding in network byte order
     - FDICT flag handling (preset dictionary not used)

  2. RFC 1951, "DEFLATE Compressed Data Format Specification version 1.3"
     https://www.rfc-editor.org/rfc/rfc1951.html
     Used for:
     - block header layout (BFINAL/BTYPE)
     - fixed Huffman code tables
     - bit-packing order (LSB-first within bytes)
     - canonical Huffman code generation
     - length/distance coding and 32K window constraints

  Implementation notes:
  - Uses fixed Huffman codes for simplicity and reasonable compression
  - Hash-based LZ77 matcher with configurable chain depth
  - Emits a single final block per compression call
  - Balanced approach between compression ratio and speed

  Scope note:
  This file only writes zlib/DEFLATE-compressed FBX array payloads.
  FBX node/mesh transform semantics are handled in ReadFBX.h and
  ConvertFBX.h.

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

namespace fbx::inflate
{
    struct WriteContext;

    namespace write_error
    {
        inline constexpr ptrdiff_t invalid_argument = -101;
        inline constexpr ptrdiff_t destination_too_small = -102;
        inline constexpr ptrdiff_t destination_too_large = -103;
        inline constexpr ptrdiff_t failed_to_flush = -104;
        inline constexpr ptrdiff_t unsupported_input = -105;
    } // namespace write_error

    namespace write_config
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
    } // namespace write_config

    namespace write_detail
    {
        using namespace fbx::inflate::write_error;

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

                s1 %= write_config::ADLER_MOD;
                s2 %= write_config::ADLER_MOD;
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
            std::array<uint16_t, write_config::LITERAL_LENGTH_SYMBOLS> litLenCodes = {};
            std::array<uint8_t, write_config::LITERAL_LENGTH_SYMBOLS> litLenBits = {};
            std::array<uint16_t, write_config::DISTANCE_SYMBOLS> distCodes = {};
            std::array<uint8_t, write_config::DISTANCE_SYMBOLS> distBits = {};

            FixedEncoderTables()
            {
                auto litLenWidths = std::array<uint8_t, write_config::LITERAL_LENGTH_SYMBOLS>{};
                auto distWidths = std::array<uint8_t, write_config::DISTANCE_SYMBOLS>{};

                for (size_t valueIndex = 0; valueIndex <= 143; ++valueIndex)
                    litLenWidths[valueIndex] = write_config::FIXED_LITLEN_BITS_0_143;

                for (size_t valueIndex = 144; valueIndex <= 255; ++valueIndex)
                    litLenWidths[valueIndex] = write_config::FIXED_LITLEN_BITS_144_255;

                for (size_t valueIndex = 256; valueIndex <= 279; ++valueIndex)
                    litLenWidths[valueIndex] = write_config::FIXED_LITLEN_BITS_256_279;

                for (size_t valueIndex = 280; valueIndex <= 287; ++valueIndex)
                    litLenWidths[valueIndex] = write_config::FIXED_LITLEN_BITS_280_287;

                distWidths.fill(write_config::FIXED_DISTANCE_BITS);
                buildCodes(litLenWidths, litLenCodes, litLenBits);
                buildCodes(distWidths, distCodes, distBits);
            }

            template <size_t Count>
            static void buildCodes(const std::array<uint8_t, Count>& widths, std::array<uint16_t, Count>& codes,
                                   std::array<uint8_t, Count>& bits) noexcept
            {
                auto widthHistogram = std::array<uint16_t, write_config::MAX_BITS + 1>{};

                for (auto width : widths)
                {
                    if (width != 0)
                        ++widthHistogram[width];
                }

                auto nextCode = std::array<uint16_t, write_config::MAX_BITS + 1>{};
                uint16_t runningCode = 0;

                for (size_t width = 1; width <= write_config::MAX_BITS; ++width)
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
            return (value * 2654435761U) >> (32U - write_config::HASH_BITS);
        }

        inline void prepareChains(WorkingSet& work, const size_t inputSize)
        {
            work.hashHead.assign(write_config::HASH_SIZE, -1);
            work.hashPrev.assign(inputSize, -1);
        }

        inline void insertPosition(WorkingSet& work, const uint8_t* src, const size_t srcSize,
                                   const size_t pos) noexcept
        {
            if (pos + write_config::MIN_MATCH > srcSize)
                return;

            const auto hash = hashBytes(src + pos);
            work.hashPrev[pos] = work.hashHead[hash];
            work.hashHead[hash] = static_cast<int32_t>(pos);
        }

        [[nodiscard]] inline Match findMatch(const WorkingSet& work, const uint8_t* src, const size_t srcSize,
                                             const size_t pos) noexcept
        {
            if (pos + write_config::MIN_MATCH > srcSize)
                return {};

            const auto hash = hashBytes(src + pos);
            auto candidate = work.hashHead[hash];
            auto best = Match{};
            const auto remaining = srcSize - pos;
            const auto maxLength = remaining < write_config::MAX_MATCH ? remaining : write_config::MAX_MATCH;
            size_t depth = 0;

            while (candidate >= 0 && depth < write_config::MAX_CHAIN)
            {
                const auto candidatePos = static_cast<size_t>(candidate);
                const auto distance = pos - candidatePos;

                if (distance > write_config::WINDOW_SIZE)
                    break;

                size_t length = 0;

                while (length < maxLength && src[candidatePos + length] == src[pos + length])
                    ++length;

                if (length >= write_config::MIN_MATCH && length > best.length)
                {
                    best.length = length;
                    best.distance = distance;

                    if (length == write_config::MAX_MATCH)
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

        [[nodiscard]] inline bool writeLengthDistance(BitPacker& packer, const size_t length, const size_t distance) noexcept
        {
            const auto& tables = fixedTables();
            size_t lengthSlot = 0;

            while (lengthSlot + 1 < write_config::LENGTH_CODES &&
                   static_cast<size_t>(write_config::LENGTH_BASES[lengthSlot + 1]) <= length)
            {
                ++lengthSlot;
            }

            if (lengthSlot >= write_config::LENGTH_CODES)
                return false;

            const auto lengthSymbol = static_cast<uint16_t>(257 + lengthSlot);
            const auto lengthExtraBits = write_config::LENGTH_EXTRA_BITS[lengthSlot];
            const auto lengthExtraValue =
                static_cast<uint32_t>(length - static_cast<size_t>(write_config::LENGTH_BASES[lengthSlot]));

            if (!packer.pushBits(tables.litLenCodes[lengthSymbol], tables.litLenBits[lengthSymbol]))
                return false;

            if (!packer.pushBits(lengthExtraValue, lengthExtraBits))
                return false;

            size_t distanceSlot = 0;

            while (distanceSlot + 1 < write_config::DISTANCE_CODES &&
                   static_cast<size_t>(write_config::DISTANCE_BASES[distanceSlot + 1]) <= distance)
            {
                ++distanceSlot;
            }

            if (distanceSlot >= write_config::DISTANCE_CODES)
                return false;

            const auto distanceExtraBits = write_config::DISTANCE_EXTRA_BITS[distanceSlot];
            const auto distanceExtraValue =
                static_cast<uint32_t>(distance - static_cast<size_t>(write_config::DISTANCE_BASES[distanceSlot]));

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

                if (readPos + write_config::MIN_MATCH <= srcSize)
                    match = findMatch(work, src, srcSize, readPos);

                if (match.length >= write_config::MIN_MATCH)
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
    } // namespace write_detail

    struct WriteContext
    {
        WriteContext() : state(std::make_unique<write_detail::WorkingSet>())
        {
        }

        WriteContext(WriteContext&&) noexcept = default;
        WriteContext& operator=(WriteContext&&) noexcept = default;
        WriteContext(const WriteContext&) = delete;
        WriteContext& operator=(const WriteContext&) = delete;

        [[nodiscard]] write_detail::WorkingSet& work() noexcept
        {
            return *state;
        }

      private:
        std::unique_ptr<write_detail::WorkingSet> state;
    };

    [[nodiscard]] inline ptrdiff_t write(WriteContext& context, void* dst, const size_t dstSize, const void* src,
                                         const size_t srcSize)
    {
        using namespace write_detail;

        if (srcSize > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max()))
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
        write_detail::storeU32be(outputBytes + 2 + payloadSize, write_detail::checksumAdler(inputBytes, srcSize));
        const auto totalSize = size_t{2} + payloadSize + 4;

        return static_cast<ptrdiff_t>(totalSize);
    }

    [[nodiscard]] inline ptrdiff_t write(void* dst, const size_t dstSize, const void* src, const size_t srcSize)
    {
        auto context = WriteContext{};
        return write(context, dst, dstSize, src, srcSize);
    }
} // namespace fbx::inflate
