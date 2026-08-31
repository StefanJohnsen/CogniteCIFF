/*----------------------------------------------------------------
  WriteNWD.h

  Native archive-448 NWD writer primitives for container assembly, camera,
  hierarchy, Fragment data, materials, and atomic publication.
----------------------------------------------------------------*/

#pragma once

#if defined(min)
#pragma push_macro("min")
#undef min
#define FALCON_NWD_WRITE_RESTORE_MIN_MACRO
#endif

#if defined(max)
#pragma push_macro("max")
#undef max
#define FALCON_NWD_WRITE_RESTORE_MAX_MACRO
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "BlowfishNWD.h"
#include "ConversionOutput.h"
#include "Zlib.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nwd::write
{
    using Bytes = std::vector<uint8_t>;

    inline constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

    struct Point3d
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct Normal3f
    {
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
    };

    struct Transform
    {
        // Row-major affine matrix. Translation is stored at [3], [7], [11]
        // and the final row must be [0, 0, 0, 1].
        std::array<double, 16> values{
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            0.0, 0.0, 0.0, 1.0,
        };
    };

    struct Mesh
    {
        std::vector<Point3d> vertices;
        std::vector<uint32_t> indices;
        std::vector<Normal3f> normals;
    };

    struct Node
    {
        std::string name;
        size_t parent = std::numeric_limits<size_t>::max();
    };

    // Source-neutral material contract used by general ConvertNWD adapters.
    // A simple FBX/OBJ material can populate only material/transparency. NWD
    // roundtrips can additionally retain their native class-55 flags and
    // class-185 JSON/resource payload without exposing reader internals.
    struct AppearanceMaterial
    {
        std::array<float, 3> ambient{};
        std::array<float, 3> diffuse{};
        std::array<float, 3> specular{};
        std::array<float, 3> emissive{};
        float shininess = 0.0F;
        float transparency = 0.0F;
    };

    struct AppearanceResource
    {
        std::string path;
        uint32_t storageMode = 0U;
        std::optional<std::string> resolvedPath;
        std::optional<std::string> producer;
        std::optional<std::string> extension;
        std::vector<uint8_t> data;
    };

    enum class AppearanceMetadataKind : uint8_t
    {
        none,
        jsonProtein,
        legacyProtein,
    };

    struct AppearanceMetadata
    {
        AppearanceMetadataKind metadataKind = AppearanceMetadataKind::none;
        std::string metadataJson;
        std::optional<std::string> simpleMaterial;
        std::vector<AppearanceResource> resources;
    };

    struct Appearance
    {
        bool present = true;
        uint32_t flags = 1U;
        bool proteinTransparent = false;
        AppearanceMaterial material;
        size_t metadata = std::numeric_limits<size_t>::max();
    };

    struct Instance
    {
        size_t mesh = 0U;
        size_t node = 0U;
        uint32_t appearance = InvalidIndex;
        bool transparent = false;
        Transform transform{};
    };

    // Coordinates and transforms are serialized unchanged. Adapters must
    // normalize units and axes consistently before populating this DTO; the
    // semantic profile does not carry source GUID, unit, or axis metadata.
    struct Scene
    {
        std::vector<Node> nodes;
        std::vector<Mesh> meshes;
        std::vector<Instance> instances;
        std::vector<Appearance> appearances;
        std::vector<AppearanceMetadata> appearanceMetadata;
    };

    // Builds a complete archive-448 semantic NWD container. The input contract
    // deliberately contains no reader/model-NWD types so format adapters can
    // share this writer without inheriting the Navisworks reader.
    [[nodiscard]] Bytes makeSemantic(const Scene& scene);
    void writeSemantic(const std::filesystem::path& target, const Scene& scene);

    namespace detail
    {
        using Bytes = nwd::write::Bytes;

        inline constexpr uint32_t ArchiveVersion = 448U;
        inline constexpr size_t DirectoryPrefixZeros = 7U;
        inline constexpr std::string_view ContainerHeader =
            "#LcUStream-V1.1 binary double release C lichunk-008";
        inline constexpr std::string_view DirectoryHeader =
            "#LcUStream-V1.1 binary double release utf8 navisworks:448 lizlib";
        inline constexpr std::array<uint8_t, 32> DirectoryChecksumKey{
            0x5B, 0x51, 0x58, 0x93, 0x71, 0x1D, 0x22, 0xE3,
            0xD0, 0x4D, 0xEF, 0x35, 0x09, 0x53, 0xA4, 0x25,
            0x50, 0xF0, 0x13, 0x29, 0x06, 0x46, 0x15, 0xCC,
            0xE3, 0xB7, 0x58, 0x98, 0x29, 0x27, 0xC4, 0xC1,
        };
        // Minimal archive-448 protocol templates required by semantic NWD output.
        inline constexpr std::string_view SemanticSourceNamespace = "converted-nwd.nwd";
        inline constexpr std::string_view Archive448PartitionStoredBase64 =
            "eJztWU9vG0UUdxEHOHBEAnEA7dlr9p9jb4SUpknaRoQmwml7oFWYnZ21J8zOWLOzcUzJ1+HKtVyouPAFeuiZC3DkE1DezG4ce73rpIgD"
            "SF5p7PW833vz5r03782T41ar9UmreD6AgQU/I1KR2OYTGEjRM9KBVwK0AMbbMO5SRt6D7wN8iI6QVFRRwUsZt8qRwPgYxmt4WkvPn1tv"
            "9v3Pn7fKMYTxUanbCMaPMJ7DJp5ZsN0M9Lc2vbaVZ0SiLCMqsza/shzradtKkSKSIgYzz2AGPhQaWpvWPcJhHh+NhBIpUfB6QIcjZbWt"
            "mCSUG5ushO3zTCGOyX5MuKIJJRLgMP8NmU6EjI0GWgEMCgyFpCQr6GMpxuAh8/uZlYFIPjSv21rxAxrt7+pfZ4jlxAjx3WB3z+uGdjcI"
            "fDvY8Fz7zt07u7bb852djZ2dcHsvtJ5etK07KCMDPCIpWhTQsIcSqjkf7j9AKZnnsqyLmerTJUJMMizpuLBRhVbuf2me6TVrl8kmSOFR"
            "ZRrmc0mNYQznkRQJxK2O3aoQbecLY9jTL9C4hpKNUCwmO4LVk9UoTyOOKFuiATESghHEjSL3aQzOXkAlEFlEC0FpRCEQDjmbNgBUKrKB"
            "0eSwQQYWaSr4yTHl6kSJ4ZCRelxMkwRCfYGoZG5oCZLbShGeI+2dppWGbDoe7dJszFCDujQ7oJwgWYR7LcT4ZTtJCFaDSwvX6mSAIjoF"
            "4BnNaNS0L73eDZRfDLrZKmMm1D29r3quMjyahGZ1nrkUnY0JzhmStUSF5JBAxq2XC34aNIXfDDQh0X2UDaapPp7TmlW0QzgcRsh1miqJ"
            "tmPFDq6WNMuGiwS9B4xgB4Z9LhRBq+2UGwfnkPTKHLUocz5yxwhTNV0JhxKkIAuvlomnjPKYyBN0XhzyOqTTdtvOAlqimOaNeCM5JhhN"
            "B+AUBfFtMvRqOAWjy2sgGT7hQqaINWvqtO0r8A3UJAxOA6RixPaShGLIHbjZrN3iZDN2mCQrpS6e/j0evwHa2GwlHgrIYCwaMU7H6EkZ"
            "y1PKteW3V8vTIc0ziKdHerp5944zyyIHIsvuIqzEapcZ7DFJodIilctG2f687CJDkfOx4KT5SHg1meo6Q1fg11tagiKIQwE4YYQP1eiG"
            "4AmNr8EWaW63MPtqpEjgBCXJXjwkq0M5G4+IJNcEvdNxnG6Z674UCl3eHZpOk1NiB3BC4IbUqACkCLeEHkuo0+wmkk2WYqLMhkXl3tET"
            "C3kT8qy16UCxNJ+R+YSrlXtRrdW4gbXju92CvXiLZm8zMXCngftpw9quYXYNozvHJIeRKctNbB0n8Po9x/dCJ+yGfc8rlQj7fn/D77pB"
            "L/ScoJTbcXy/2wcG39dc8+tktVV9pVkuK+Ub7UgXUXKuz2npLzwSFBMG+dt4yNh3MKKJKiPhyp9zroAKJUl8Yq7RjaArfxWfGRS0xduI"
            "Nysix9MxWRYECR7ydpRXL78F4yylPSyib44ezBKNMc4ywFnIXEdgDVJBuP1KsoIdI1VV06+AlgCeMXme09iI3zvHI8gf5N7DauthrseP"
            "iivFMpUkYewHvmtHrkfsIHAcO8JeYEeBTyLP7/tJiIpLtCQJZAgoCOXd/uKivq9sem7ebz7/7tOt7396tfX63enZq19+3+rE73z+/stf"
            "X9C5/vG07B//WPeP6/7xKs7X/eO6f1z3j+v+cd0/rvvHdf+47h/X/eN/r390Ohuhefp9pxc6PbfUbGk2qp1d95v/534ziLpRD7uuvRH4"
            "oR34G8jux05gd8Oej7AbRkEQ/wv95tdJ8tvWTb/LfvNF2W++KPtNTb/t+y/rvlsftvR/rw8m+EAguJQcsXxI+SbDLOaTGGg/37r6H3Z1"
            "P1z/W+9V8/owdjafPHi8+1hSyBJPTCMHPVn8pLiN0G9JbC/8Y9wpNFhe7y8t9PUPn1X77SryUue/Aeu6zJw=";
        inline constexpr std::string_view Archive448PartitionPropsStoredBase64 =
            "AAAAAHicY2CAAAAACAAB";
        inline constexpr std::string_view Archive448CurrentViewStoredBase64 =
            "eJzzZmbABAeCHCB0kUMcz0bGdPf9+9GVHO+RN51/6L09hPfBvoqh+88vxZdwPoRWccBiOhDYQcQb0lBpBj2H/0CA6Z4iB5i5jGhSYP6C"
            "Poh8x2a4fUc0w3h2BReD+UxYXIAuxoQiBnM/ArCh0cQBTHOgoAFZnp3K5mGLUkrMwxZ+lJjHQmXzWKlsHqZ6dJpYe0jVh2kOANZpPDs=";
        // Neutral light-grey Freedom background, matching the display profile
        // used by topside6.nwd. This is decoded payload data; compressedChunk
        // applies the canonical archive-448 storage encoding.
        inline constexpr std::string_view CanonicalGreyBackgroundDecodedBase64 =
            "AAAAAAAAAAAYGBgYGBjoPxgYGBgYGOg/GBgYGBgY6D+amZmZmZnpPwAAAAAAAPA/AAAAAAAA8D8AAAAAAADwPwAAAAAAAPA/"
            "AAAAAAAA8D9aWlpaWlraP5KRkZGRkeE/3dzc3Nzc7D9aWlpaWlrqPx4eHh4eHu4/AAAAAAAA8D8eHh4eHh7ePx4eHh4eHt4/"
            "Hh4eHh4e3j+cm5ubm5vrP5ybm5ubm+s/nJubm5ub6z8AAAAAAAAAAA==";
        inline constexpr std::string_view Archive448FileInfoStoredBase64 =
            "eJz7////fwYg8Ogtnrx1D5ffLNXKC24vwqRAYoxA7A7EgkCcnJ9XllpUkpqim1eeogfEDBDw4tT+n4/kBCJ3LNFiZsjaZMZAQD0I/EcC"
            "6Prx6ZU7+WHd9RfWQZ08QXlbjndsDWj9nZoYFe06/X51+SXVqRNBagBPmUwq";
        inline constexpr size_t MinimumSemanticChunkCount = 8U;


        [[nodiscard]] inline uint32_t asU32(const size_t value, const char* label)
        {
            if (value > std::numeric_limits<uint32_t>::max())
                throw std::overflow_error(std::string(label) + " exceeds the NWD 32-bit range");
            return static_cast<uint32_t>(value);
        }

        [[nodiscard]] inline uint64_t checkedAdd(const uint64_t left, const uint64_t right,
                                                 const char* label)
        {
            if (right > std::numeric_limits<uint64_t>::max() - left)
                throw std::overflow_error(std::string(label) + " exceeds the NWD 64-bit range");
            return left + right;
        }

        [[nodiscard]] inline uint8_t base64Value(const char value)
        {
            if (value >= 'A' && value <= 'Z')
                return static_cast<uint8_t>(value - 'A');
            if (value >= 'a' && value <= 'z')
                return static_cast<uint8_t>(value - 'a' + 26);
            if (value >= '0' && value <= '9')
                return static_cast<uint8_t>(value - '0' + 52);
            if (value == '+')
                return 62U;
            if (value == '/')
                return 63U;
            throw std::logic_error("Invalid embedded NWD shell encoding");
        }

        [[nodiscard]] inline Bytes decodeBase64(const std::string_view encoded)
        {
            if ((encoded.size() & 3U) != 0U)
                throw std::logic_error("Invalid embedded NWD shell length");

            auto decoded = Bytes{};
            decoded.reserve((encoded.size() / 4U) * 3U);
            for (size_t offset = 0U; offset < encoded.size(); offset += 4U)
            {
                const auto thirdPadding = encoded[offset + 2U] == '=';
                const auto fourthPadding = encoded[offset + 3U] == '=';
                if ((thirdPadding && !fourthPadding) ||
                    ((thirdPadding || fourthPadding) && offset + 4U != encoded.size()))
                {
                    throw std::logic_error("Invalid embedded NWD shell padding");
                }

                const auto first = base64Value(encoded[offset]);
                const auto second = base64Value(encoded[offset + 1U]);
                const auto third = thirdPadding ? uint8_t{0U} : base64Value(encoded[offset + 2U]);
                const auto fourth = fourthPadding ? uint8_t{0U} : base64Value(encoded[offset + 3U]);
                decoded.push_back(static_cast<uint8_t>((first << 2U) | (second >> 4U)));
                if (!thirdPadding)
                    decoded.push_back(static_cast<uint8_t>((second << 4U) | (third >> 2U)));
                if (!fourthPadding)
                    decoded.push_back(static_cast<uint8_t>((third << 6U) | fourth));
            }
            return decoded;
        }

        class ByteWriter
        {
        public:
            void writeU8(const uint8_t value)
            {
                reserve(1U);
                bytes_.push_back(value);
            }

            void writeU32(const uint32_t value)
            {
                reserve(sizeof(value));
                for (uint32_t shift = 0U; shift < 32U; shift += 8U)
                    bytes_.push_back(static_cast<uint8_t>(value >> shift));
            }

            void writeI32(const int32_t value)
            {
                writeU32(static_cast<uint32_t>(value));
            }

            void writeU64(const uint64_t value)
            {
                reserve(sizeof(value));
                for (uint32_t shift = 0U; shift < 64U; shift += 8U)
                    bytes_.push_back(static_cast<uint8_t>(value >> shift));
            }

            void writeFloat(const float value)
            {
                writeU32(std::bit_cast<uint32_t>(value));
            }

            void writeDouble(const double value)
            {
                writeU64(std::bit_cast<uint64_t>(value));
            }

            void write(const std::span<const uint8_t> bytes)
            {
                reserve(bytes.size());
                bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
            }

            void write(const std::string_view text)
            {
                reserve(text.size());
                bytes_.insert(bytes_.end(), text.begin(), text.end());
            }

            void writePaddedString(const std::string_view text)
            {
                writeU32(asU32(text.size(), "NWD string"));
                write(text);
                align(4U);
            }

            void align(const size_t alignment)
            {
                if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
                    throw std::logic_error("Invalid NWD writer alignment");
                const auto remainder = bytes_.size() & (alignment - 1U);
                const auto padding = remainder == 0U ? 0U : alignment - remainder;
                reserve(padding);
                bytes_.insert(bytes_.end(), padding, uint8_t{0});
            }

            [[nodiscard]] size_t size() const noexcept
            {
                return bytes_.size();
            }

            [[nodiscard]] const Bytes& bytes() const noexcept
            {
                return bytes_;
            }

            [[nodiscard]] Bytes take() &&
            {
                return std::move(bytes_);
            }

        private:
            void reserve(const size_t additional)
            {
                if (additional > bytes_.max_size() - bytes_.size())
                    throw std::overflow_error("NWD output exceeds vector capacity");
            }

            Bytes bytes_;
        };

        [[nodiscard]] inline Bytes compress(const std::span<const uint8_t> decoded,
                                            const char* label)
        {
            auto capacity = std::max<size_t>(64U, decoded.size() + decoded.size() / 8U + 64U);
            auto encoded = Bytes(capacity);
            auto context = ::ciff::zlib::DeflateContext{};

            for (;;)
            {
                const auto result = ::ciff::zlib::deflate(context, encoded.data(), encoded.size(),
                                                        decoded.data(), decoded.size());
                if (result >= 0)
                {
                    encoded.resize(static_cast<size_t>(result));
                    return encoded;
                }
                if (result != ::ciff::zlib::deflate_error::destination_too_small)
                    throw std::runtime_error(std::string("Unable to compress ") + label);
                if (encoded.size() > encoded.max_size() / 2U)
                    throw std::overflow_error(std::string(label) + " compressed output is too large");
                encoded.resize(encoded.size() * 2U);
            }
        }

        [[nodiscard]] inline const char* inflateErrorName(const ptrdiff_t status) noexcept
        {
            using namespace ::ciff::zlib::inflate_error;

            switch (status)
            {
            case invalid_argument: return "invalid argument";
            case destination_too_large: return "destination too large";
            case truncated_input: return "truncated input";
            case invalid_wrapper_header: return "invalid zlib header";
            case unsupported_wrapper_feature: return "preset dictionary unsupported";
            case invalid_block_type: return "invalid block type";
            case invalid_stored_block: return "invalid stored block";
            case invalid_huffman_table: return "invalid Huffman tree";
            case invalid_code_length: return "invalid Huffman code";
            case invalid_length_code: return "invalid length";
            case invalid_distance_code: return "invalid distance";
            case invalid_back_reference: return "invalid back reference";
            case output_overrun: return "output overrun";
            case output_underrun: return "output underrun";
            case checksum_mismatch: return "checksum mismatch";
            case trailing_garbage: return "trailing garbage";
            default: return "unknown inflate error";
            }
        }

        [[nodiscard]] inline bool possibleZlibHeader(
            const std::span<const uint8_t> source) noexcept
        {
            if (source.size() < 2U)
                return false;

            const auto cmf = source[0];
            const auto flg = source[1];
            const auto header = static_cast<uint16_t>(
                (static_cast<uint16_t>(cmf) << 8U) | flg);
            return (cmf & 0x0fU) == 8U && (cmf >> 4U) <= 7U &&
                   header % 31U == 0U && (flg & 0x20U) == 0U;
        }

        [[nodiscard]] inline Bytes inflateExact(
            const std::span<const uint8_t> source, const size_t maximumDecodedSize)
        {
            if (!possibleZlibHeader(source))
                throw std::runtime_error(
                    "NWD chunk does not start with a supported zlib header");
            if (maximumDecodedSize == 0U)
                throw std::runtime_error("Invalid maximum NWD inflate size");

            auto context = ::ciff::zlib::InflateContext{};
            auto destination = Bytes{};
            const auto initialGuess = std::max<size_t>(
                1U, std::min(maximumDecodedSize,
                             source.size() > maximumDecodedSize / 4U
                                 ? maximumDecodedSize
                                 : source.size() * 4U));
            auto capacity = initialGuess;

            for (;;)
            {
                destination.resize(capacity);
                const auto status = ::ciff::zlib::inflateActualSize(
                    context, destination.data(), destination.size(), source.data(), source.size());
                if (status >= 0)
                {
                    if (status == 0)
                        throw std::runtime_error("Unsupported empty NWD zlib stream");
                    destination.resize(static_cast<size_t>(status));
                    return destination;
                }
                if (status == ::ciff::zlib::inflate_error::output_overrun)
                {
                    if (capacity == maximumDecodedSize)
                    {
                        throw std::runtime_error(
                            "NWD chunk exceeds configured decoded-size limit");
                    }
                    capacity = capacity > maximumDecodedSize / 2U
                                   ? maximumDecodedSize
                                   : capacity * 2U;
                    continue;
                }
                throw std::runtime_error(
                    std::string("Unable to inflate NWD chunk: ") + inflateErrorName(status));
            }
        }

        [[nodiscard]] inline uint32_t adler32(
            const std::span<const uint8_t> bytes) noexcept
        {
            constexpr uint32_t Modulus = 65521U;
            auto first = uint32_t{1U};
            auto second = uint32_t{};
            auto offset = size_t{};
            while (offset < bytes.size())
            {
                const auto count = std::min<size_t>(bytes.size() - offset, 5552U);
                for (size_t index = 0U; index < count; ++index)
                {
                    first += bytes[offset + index];
                    second += first;
                }
                first %= Modulus;
                second %= Modulus;
                offset += count;
            }
            return (second << 16U) | first;
        }

        struct StoredChunk
        {
            std::string name;
            uint32_t storageMode = 0U;
            uint32_t prefixLength = 0U;
            uint32_t indexLength = 0U;
            Bytes stored;
        };

        struct SemanticChunks
        {
            StoredChunk geometryCompress;
            StoredChunk geometry;
            StoredChunk logicalHierarchy;
            StoredChunk fragments;
            StoredChunk spatialHierarchy;
            std::optional<StoredChunk> currentView;
            std::optional<StoredChunk> headlight;
            std::optional<StoredChunk> background;
            std::optional<StoredChunk> shadOverrides;
            std::optional<StoredChunk> lights;
        };

        [[nodiscard]] inline StoredChunk makeEmbeddedChunk(
            std::string name, const uint32_t storageMode, const uint32_t prefixLength,
            const uint32_t indexLength, const std::string_view storedBase64)
        {
            return StoredChunk{
                .name = std::move(name),
                .storageMode = storageMode,
                .prefixLength = prefixLength,
                .indexLength = indexLength,
                .stored = decodeBase64(storedBase64),
            };
        }

        struct SpatialFragmentBounds
        {
            uint32_t fragmentIndex = 0U;
            std::array<double, 3> minimum{};
            std::array<double, 3> maximum{};
        };

        namespace spatial_detail
        {
            inline constexpr size_t MaximumBucketSize = 8U;
            inline constexpr size_t MaximumBranchFanout = 7U;
            inline constexpr uint32_t MortonBitsPerAxis = 21U;
            inline constexpr uint32_t MaximumMortonCoordinate = (1U << MortonBitsPerAxis) - 1U;

            struct OrderedFragment
            {
                uint64_t morton = 0U;
                uint32_t fragmentIndex = 0U;
            };

            struct Range
            {
                size_t first = 0U;
                size_t count = 0U;
            };

            [[nodiscard]] inline uint64_t mortonCode(const std::array<uint32_t, 3>& coordinates) noexcept
            {
                auto result = uint64_t{};
                for (uint32_t bit = 0U; bit < MortonBitsPerAxis; ++bit)
                {
                    result |= static_cast<uint64_t>((coordinates[0] >> bit) & 1U) << (3U * bit);
                    result |= static_cast<uint64_t>((coordinates[1] >> bit) & 1U) << (3U * bit + 1U);
                    result |= static_cast<uint64_t>((coordinates[2] >> bit) & 1U) << (3U * bit + 2U);
                }
                return result;
            }

            [[nodiscard]] inline uint32_t quantizeMortonCoordinate(const double value, const double minimum,
                                                                   const double maximum) noexcept
            {
                if (!(maximum > minimum))
                    return 0U;

                // Halving both differences prevents overflow for finite bounds
                // that span almost the complete double range.
                const auto extent = maximum * 0.5 - minimum * 0.5;
                if (!(extent > 0.0) || !std::isfinite(extent))
                    return value <= minimum ? 0U : MaximumMortonCoordinate;
                const auto offset = value * 0.5 - minimum * 0.5;
                const auto normalized = std::clamp(offset / extent, 0.0, 1.0);
                return static_cast<uint32_t>(normalized * static_cast<double>(MaximumMortonCoordinate) + 0.5);
            }

            [[nodiscard]] inline std::vector<Range> balancedRanges(const size_t elementCount,
                                                                   const size_t maximumRangeSize)
            {
                if (elementCount == 0U || maximumRangeSize == 0U)
                    return {};
                const auto rangeCount = (elementCount + maximumRangeSize - 1U) / maximumRangeSize;
                const auto baseSize = elementCount / rangeCount;
                const auto largerRangeCount = elementCount % rangeCount;
                auto ranges = std::vector<Range>{};
                ranges.reserve(rangeCount);
                auto first = size_t{};
                for (size_t range = 0U; range < rangeCount; ++range)
                {
                    const auto count = baseSize + (range < largerRangeCount ? 1U : 0U);
                    ranges.push_back(Range{.first = first, .count = count});
                    first += count;
                }
                return ranges;
            }

            inline void writeNode(ByteWriter& writer, const std::vector<std::vector<Range>>& levels, const size_t level,
                                  const size_t node, const std::span<const OrderedFragment> fragments)
            {
                const auto& range = levels.at(level).at(node);
                if (level == 0U)
                {
                    writer.writeU32(2U); // Fragment bucket.
                    writer.writeU32(asU32(range.count, "NWD spatial bucket size"));
                    for (size_t child = 0U; child < range.count; ++child)
                    {
                        writer.writeU32(1U); // Fragment leaf.
                        writer.writeU32(fragments[range.first + child].fragmentIndex);
                    }
                    return;
                }

                writer.writeU32(5U); // Spatial branch.
                writer.writeU32(asU32(range.count, "NWD spatial branch fanout"));
                for (size_t child = 0U; child < range.count; ++child)
                    writeNode(writer, levels, level - 1U, range.first + child, fragments);
            }
        }

        [[nodiscard]] inline Bytes makeSpatialHierarchy(
            const std::span<const SpatialFragmentBounds> bounds)
        {
            if (bounds.empty())
            {
                auto writer = ByteWriter{};
                writer.writeU32(5U); // Native spatial branch root.
                writer.writeU32(1U); // One empty bucket child.
                writer.writeU32(2U); // Fragment bucket.
                writer.writeU32(0U);
                return std::move(writer).take();
            }

            auto centroidMinimum =
                std::array<double, 3>{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                      std::numeric_limits<double>::max()};
            auto centroidMaximum =
                std::array<double, 3>{std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                                      std::numeric_limits<double>::lowest()};
            for (const auto& fragment : bounds)
            {
                auto centroid = std::array<double, 3>{};
                for (size_t axis = 0U; axis < centroid.size(); ++axis)
                {
                    if (!std::isfinite(fragment.minimum[axis]) || !std::isfinite(fragment.maximum[axis]) ||
                        fragment.minimum[axis] > fragment.maximum[axis])
                    {
                        throw std::invalid_argument("NWD spatial Fragment bounds are invalid");
                    }
                    centroid[axis] = fragment.minimum[axis] * 0.5 + fragment.maximum[axis] * 0.5;
                    centroidMinimum[axis] = std::min(centroidMinimum[axis], centroid[axis]);
                    centroidMaximum[axis] = std::max(centroidMaximum[axis], centroid[axis]);
                }
            }

            auto ordered = std::vector<spatial_detail::OrderedFragment>{};
            ordered.reserve(bounds.size());
            for (size_t fragment = 0U; fragment < bounds.size(); ++fragment)
            {
                auto coordinates = std::array<uint32_t, 3>{};
                for (size_t axis = 0U; axis < coordinates.size(); ++axis)
                {
                    const auto centroid =
                        bounds[fragment].minimum[axis] * 0.5 + bounds[fragment].maximum[axis] * 0.5;
                    coordinates[axis] = spatial_detail::quantizeMortonCoordinate(
                        centroid, centroidMinimum[axis], centroidMaximum[axis]);
                }
                ordered.push_back(spatial_detail::OrderedFragment{
                    .morton = spatial_detail::mortonCode(coordinates),
                    .fragmentIndex = bounds[fragment].fragmentIndex,
                });
            }
            std::sort(
                ordered.begin(), ordered.end(),
                [](const spatial_detail::OrderedFragment& left, const spatial_detail::OrderedFragment& right) {
                    if (left.morton != right.morton)
                        return left.morton < right.morton;
                    return left.fragmentIndex < right.fragmentIndex;
                });

            auto levels = std::vector<std::vector<spatial_detail::Range>>{};
            levels.push_back(spatial_detail::balancedRanges(ordered.size(), spatial_detail::MaximumBucketSize));
            while (levels.back().size() > spatial_detail::MaximumBranchFanout)
            {
                levels.push_back(
                    spatial_detail::balancedRanges(levels.back().size(), spatial_detail::MaximumBranchFanout));
            }

            auto writer = ByteWriter{};
            writer.writeU32(5U); // Spatial root is always a branch.
            writer.writeU32(asU32(levels.back().size(), "NWD spatial root fanout"));
            for (size_t child = 0U; child < levels.back().size(); ++child)
            {
                spatial_detail::writeNode(writer, levels, levels.size() - 1U, child, ordered);
            }
            return std::move(writer).take();
        }

        [[nodiscard]] inline StoredChunk compressedChunk(std::string name, Bytes decoded)
        {
            return StoredChunk{
                .name = std::move(name),
                .storageMode = 1U,
                .stored = compress(decoded, "NWD semantic chunk"),
            };
        }

        inline void writeU32At(Bytes& bytes, const size_t offset, const uint32_t value)
        {
            if (offset > bytes.size() || sizeof(value) > bytes.size() - offset)
                throw std::runtime_error("NWD CurrentView field lies outside its payload");
            for (size_t byte = 0U; byte < sizeof(value); ++byte)
                bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8U));
        }

        inline void writeDoubleAt(Bytes& bytes, const size_t offset, const double value)
        {
            if (!std::isfinite(value))
                throw std::invalid_argument("NWD CurrentView value is not finite");
            const auto bits = std::bit_cast<uint64_t>(value);
            if (offset > bytes.size() || sizeof(bits) > bytes.size() - offset)
                throw std::runtime_error("NWD CurrentView field lies outside its payload");
            for (size_t byte = 0U; byte < sizeof(bits); ++byte)
                bytes[offset + byte] = static_cast<uint8_t>(bits >> (byte * 8U));
        }

        [[nodiscard]] inline std::optional<uint32_t> readU32At(
            const std::span<const uint8_t> bytes, const size_t offset) noexcept
        {
            if (offset > bytes.size() || sizeof(uint32_t) > bytes.size() - offset)
                return std::nullopt;
            auto value = uint32_t{};
            for (size_t byte = 0U; byte < sizeof(value); ++byte)
                value |= static_cast<uint32_t>(bytes[offset + byte]) << (byte * 8U);
            return value;
        }

        [[nodiscard]] inline std::optional<size_t> addCurrentViewOffset(
            const size_t offset, const size_t amount, const size_t limit) noexcept
        {
            if (offset > limit || amount > limit - offset)
                return std::nullopt;
            return offset + amount;
        }

        [[nodiscard]] inline std::optional<size_t> alignCurrentViewOffset(
            const size_t offset, const size_t alignment, const size_t limit) noexcept
        {
            if (alignment == 0U || (alignment & (alignment - 1U)) != 0U ||
                offset > std::numeric_limits<size_t>::max() - (alignment - 1U))
            {
                return std::nullopt;
            }
            const auto aligned = (offset + alignment - 1U) & ~(alignment - 1U);
            if (aligned > limit)
                return std::nullopt;
            return aligned;
        }

        [[nodiscard]] inline std::optional<size_t> currentViewFocalDistanceOffset(
            const std::span<const uint8_t> decoded) noexcept
        {
            constexpr auto AvatarLengthOffset = size_t{144U};
            constexpr auto AvatarPayloadOffset = AvatarLengthOffset + sizeof(int32_t);
            const auto flags = readU32At(decoded, 0U);
            const auto avatarLengthBits = readU32At(decoded, AvatarLengthOffset);
            if (!flags || !avatarLengthBits)
                return std::nullopt;

            const auto avatarLength = std::bit_cast<int32_t>(*avatarLengthBits);
            auto offset = AvatarPayloadOffset;
            if (avatarLength != -1)
            {
                if (avatarLength < 0)
                    return std::nullopt;
                const auto afterAvatar = addCurrentViewOffset(
                    offset, static_cast<size_t>(avatarLength), decoded.size());
                if (!afterAvatar)
                    return std::nullopt;
                const auto aligned = alignCurrentViewOffset(*afterAvatar, 4U, decoded.size());
                if (!aligned)
                    return std::nullopt;
                offset = *aligned;
            }

            const auto afterAvatarState = addCurrentViewOffset(offset, 12U, decoded.size());
            if (!afterAvatarState)
                return std::nullopt;
            const auto alignedState = alignCurrentViewOffset(*afterAvatarState, 8U, decoded.size());
            if (!alignedState)
                return std::nullopt;
            const auto afterNavigationState = addCurrentViewOffset(
                *alignedState, 56U, decoded.size());
            if (!afterNavigationState)
                return std::nullopt;
            offset = *afterNavigationState;
            if ((*flags & 0x4U) != 0U)
            {
                const auto afterOptionalState = addCurrentViewOffset(offset, 24U, decoded.size());
                if (!afterOptionalState)
                    return std::nullopt;
                offset = *afterOptionalState;
            }
            if (offset > decoded.size() || sizeof(double) > decoded.size() - offset)
                return std::nullopt;
            return offset;
        }

        [[nodiscard]] inline std::array<double, 3> cross(
            const std::array<double, 3>& left, const std::array<double, 3>& right) noexcept
        {
            return {
                left[1] * right[2] - left[2] * right[1],
                left[2] * right[0] - left[0] * right[2],
                left[0] * right[1] - left[1] * right[0],
            };
        }

        [[nodiscard]] inline double dot(const std::array<double, 3>& left,
                                        const std::array<double, 3>& right) noexcept
        {
            return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
        }

        [[nodiscard]] inline std::array<double, 3> normalized(
            const std::array<double, 3>& value, const char* label)
        {
            const auto length = std::hypot(value[0], value[1], value[2]);
            if (!std::isfinite(length) || length <= 0.0)
                throw std::invalid_argument(std::string("NWD CurrentView has invalid ") + label);
            return {value[0] / length, value[1] / length, value[2] / length};
        }

        [[nodiscard]] inline std::array<double, 4> quaternionFromBasis(
            const std::array<double, 3>& right, const std::array<double, 3>& up,
            const std::array<double, 3>& backward)
        {
            // Matrix columns map the camera's local +X/+Y/+Z axes into world
            // space. Navisworks looks along local -Z.
            const auto m00 = right[0];
            const auto m01 = up[0];
            const auto m02 = backward[0];
            const auto m10 = right[1];
            const auto m11 = up[1];
            const auto m12 = backward[1];
            const auto m20 = right[2];
            const auto m21 = up[2];
            const auto m22 = backward[2];

            auto result = std::array<double, 4>{}; // x, y, z, w
            const auto trace = m00 + m11 + m22;
            if (trace > 0.0)
            {
                const auto scale = 2.0 * std::sqrt(trace + 1.0);
                result = {(m21 - m12) / scale, (m02 - m20) / scale,
                          (m10 - m01) / scale, 0.25 * scale};
            }
            else if (m00 > m11 && m00 > m22)
            {
                const auto scale = 2.0 * std::sqrt(1.0 + m00 - m11 - m22);
                result = {0.25 * scale, (m01 + m10) / scale,
                          (m02 + m20) / scale, (m21 - m12) / scale};
            }
            else if (m11 > m22)
            {
                const auto scale = 2.0 * std::sqrt(1.0 + m11 - m00 - m22);
                result = {(m01 + m10) / scale, 0.25 * scale,
                          (m12 + m21) / scale, (m02 - m20) / scale};
            }
            else
            {
                const auto scale = 2.0 * std::sqrt(1.0 + m22 - m00 - m11);
                result = {(m02 + m20) / scale, (m12 + m21) / scale,
                          0.25 * scale, (m10 - m01) / scale};
            }

            const auto length = std::sqrt(result[0] * result[0] + result[1] * result[1] +
                                          result[2] * result[2] + result[3] * result[3]);
            if (!std::isfinite(length) || length <= 0.0)
                throw std::runtime_error("Unable to construct NWD CurrentView orientation");
            for (auto& component : result)
                component /= length;
            if (result[3] < 0.0)
            {
                for (auto& component : result)
                    component = -component;
            }
            return result;
        }

        [[nodiscard]] inline StoredChunk makeCurrentView(
            const std::span<const SpatialFragmentBounds> fragmentBounds,
            const std::optional<std::span<const uint8_t>> decodedSourceTemplate = std::nullopt)
        {
            if (fragmentBounds.empty())
                throw std::invalid_argument("NWD CurrentView requires scene bounds");

            // Preserve an archive-448 source view's non-camera state whenever
            // its variable layout can locate the orbit/zoom focal distance.
            // Unsupported or absent templates fall back to the canonical view.
            const auto view = makeEmbeddedChunk(
                std::string{SemanticSourceNamespace} + "\\LcOpCurrentViewElement",
                1U, 0U, 0U, Archive448CurrentViewStoredBase64);
            auto decoded = inflateExact(view.stored, size_t{1U} << 20U);
            if (decoded.size() < 96U)
                throw std::runtime_error("NWD CurrentView payload is too small for its camera");
            auto focalDistanceOffset = currentViewFocalDistanceOffset(decoded);
            if (!focalDistanceOffset)
                throw std::logic_error("Canonical NWD CurrentView has an unsupported layout");
            if (decodedSourceTemplate && decodedSourceTemplate->size() >= 96U)
            {
                if (const auto sourceFocalDistanceOffset =
                        currentViewFocalDistanceOffset(*decodedSourceTemplate))
                {
                    decoded.assign(decodedSourceTemplate->begin(), decodedSourceTemplate->end());
                    focalDistanceOffset = sourceFocalDistanceOffset;
                }
            }

            auto minimum = std::array<double, 3>{
                std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                std::numeric_limits<double>::max()};
            auto maximum = std::array<double, 3>{
                std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                std::numeric_limits<double>::lowest()};
            for (const auto& bounds : fragmentBounds)
            {
                for (size_t axis = 0U; axis < 3U; ++axis)
                {
                    if (!std::isfinite(bounds.minimum[axis]) ||
                        !std::isfinite(bounds.maximum[axis]) ||
                        bounds.minimum[axis] > bounds.maximum[axis])
                    {
                        throw std::invalid_argument("NWD CurrentView scene bounds are invalid");
                    }
                    minimum[axis] = std::min(minimum[axis], bounds.minimum[axis]);
                    maximum[axis] = std::max(maximum[axis], bounds.maximum[axis]);
                }
            }

            auto center = std::array<double, 3>{};
            auto extent = std::array<double, 3>{};
            for (size_t axis = 0U; axis < 3U; ++axis)
            {
                extent[axis] = maximum[axis] - minimum[axis];
                center[axis] = minimum[axis] + extent[axis] * 0.5;
                if (!std::isfinite(extent[axis]) || !std::isfinite(center[axis]))
                    throw std::overflow_error("NWD CurrentView scene bounds exceed the numeric range");
            }

            // A stable oblique view: look from southwest and above, with the
            // projected world +Z direction defining screen-up. Geometry and
            // model axes remain completely unchanged.
            const auto forward = normalized({1.0, 1.0, -0.5}, "view direction");
            const auto worldUp = std::array<double, 3>{0.0, 0.0, 1.0};
            const auto right = normalized(cross(forward, worldUp), "right axis");
            const auto cameraUp = normalized(cross(right, forward), "up axis");
            const auto backward = std::array<double, 3>{-forward[0], -forward[1], -forward[2]};
            const auto quaternion = quaternionFromBasis(right, cameraUp, backward);

            constexpr auto Aspect = 1.0;
            constexpr auto VerticalFieldOfView = 0.7853981633974483; // 45 degrees.
            const auto verticalTangent = std::tan(VerticalFieldOfView * 0.5);
            const auto horizontalTangent = verticalTangent * Aspect;
            auto requiredDistance = 0.0;
            auto localDepths = std::array<double, 8>{};
            auto cornerIndex = size_t{};
            for (size_t mask = 0U; mask < 8U; ++mask)
            {
                const auto delta = std::array<double, 3>{
                    (mask & 1U ? maximum[0] : minimum[0]) - center[0],
                    (mask & 2U ? maximum[1] : minimum[1]) - center[1],
                    (mask & 4U ? maximum[2] : minimum[2]) - center[2],
                };
                const auto horizontal = std::abs(dot(delta, right));
                const auto vertical = std::abs(dot(delta, cameraUp));
                const auto depth = dot(delta, forward);
                localDepths[cornerIndex++] = depth;
                requiredDistance = std::max(
                    requiredDistance,
                    std::max(horizontal / horizontalTangent - depth,
                             vertical / verticalTangent - depth));
            }

            auto sceneScale = std::hypot(extent[0], extent[1], extent[2]);
            if (!std::isfinite(sceneScale))
                throw std::overflow_error("NWD CurrentView scene extent exceeds the numeric range");
            if (sceneScale <= 0.0)
                sceneScale = 1.0;
            const auto distance = std::max(requiredDistance, sceneScale * 0.5) + sceneScale * 0.1;
            const auto position = std::array<double, 3>{
                center[0] - forward[0] * distance,
                center[1] - forward[1] * distance,
                center[2] - forward[2] * distance,
            };
            auto minimumDepth = std::numeric_limits<double>::max();
            auto maximumDepth = std::numeric_limits<double>::lowest();
            for (const auto localDepth : localDepths)
            {
                minimumDepth = std::min(minimumDepth, distance + localDepth);
                maximumDepth = std::max(maximumDepth, distance + localDepth);
            }
            const auto clipMargin = sceneScale * 0.1;
            const auto nearClip = std::max(sceneScale * 1.0e-6, minimumDepth - clipMargin);
            const auto farClip = std::max(nearClip + sceneScale * 1.0e-6,
                                          maximumDepth + clipMargin);

            writeU32At(decoded, 4U, 0U); // Perspective camera.
            for (size_t axis = 0U; axis < position.size(); ++axis)
                writeDoubleAt(decoded, 8U + axis * sizeof(double), position[axis]);
            for (size_t component = 0U; component < quaternion.size(); ++component)
                writeDoubleAt(decoded, 32U + component * sizeof(double), quaternion[component]);
            writeDoubleAt(decoded, 64U, Aspect);
            writeDoubleAt(decoded, 72U, VerticalFieldOfView);
            writeDoubleAt(decoded, 80U, nearClip);
            writeDoubleAt(decoded, 88U, farClip);
            writeDoubleAt(decoded, *focalDistanceOffset, distance); // Orbit/zoom focal distance.

            const auto source = std::string{SemanticSourceNamespace};
            return compressedChunk(source + "\\LcOpCurrentViewElement", std::move(decoded));
        }

        struct DirectoryEntry
        {
            std::string_view name;
            uint32_t storageMode = 0U;
            uint64_t physicalOffset = 0U;
            uint64_t storedLength = 0U;
            uint32_t prefixLength = 0U;
            uint32_t indexLength = 0U;
        };

        [[nodiscard]] inline Bytes makeDirectory(
            const std::span<const DirectoryEntry> entries)
        {
            auto writer = ByteWriter{};
            writer.writeI32(-1);
            writer.writeU32(asU32(entries.size(), "NWD chunk count"));
            for (const auto& entry : entries)
            {
                if (entry.name.empty())
                    throw std::logic_error("NWD directory entry has no chunk name");
                writer.writeU32(asU32(entry.name.size(), "NWD chunk name"));
                writer.write(entry.name);
                writer.align(4U);
                writer.writeU32(entry.storageMode);
                writer.align(8U);
                writer.writeU64(entry.physicalOffset);
                writer.writeU64(entry.storedLength);
                writer.writeU32(entry.prefixLength);
                writer.writeU32(entry.indexLength);
                writer.align(8U);
            }
            return std::move(writer).take();
        }

        [[nodiscard]] inline std::array<uint8_t, 8> makeDirectoryChecksum(
            const std::span<const DirectoryEntry> entries)
        {
            // LcUWriteChunkStream checksums the semantic directory fields,
            // excluding the entry count, string lengths, and alignment bytes.
            auto fields = ByteWriter{};
            for (const auto& entry : entries)
            {
                if (entry.name.empty())
                    throw std::logic_error("NWD directory entry has no chunk name");
                fields.write(entry.name);
                fields.writeU32(entry.storageMode);
                fields.writeU64(entry.physicalOffset);
                fields.writeU64(entry.storedLength);
                fields.writeU32(entry.prefixLength);
                fields.writeU32(entry.indexLength);
            }

            const auto checksum = adler32(fields.bytes());
            auto plain = ByteWriter{};
            plain.writeU32(checksum);
            plain.writeU32(checksum);
            if (!nwd::blowfish::selfTest())
                throw std::runtime_error("NWD Blowfish self-test failed");
            return nwd::blowfish::Cipher(DirectoryChecksumKey).encryptNativeBlock(plain.bytes());
        }

        [[nodiscard]] inline Bytes makeContainer(std::vector<StoredChunk> chunks)
        {
            if (chunks.empty())
                throw std::invalid_argument("NWD container requires at least one chunk");

            for (size_t index = 0U; index < chunks.size(); ++index)
            {
                if (chunks[index].name.empty())
                    throw std::invalid_argument("NWD chunk name cannot be empty");
                for (size_t previous = 0U; previous < index; ++previous)
                {
                    if (chunks[previous].name == chunks[index].name)
                        throw std::invalid_argument("NWD container has duplicate chunk name: " +
                                                    chunks[index].name);
                }
            }

            auto entries = std::vector<DirectoryEntry>{};
            entries.reserve(chunks.size());
            auto nextOffset = checkedAdd(ContainerHeader.size(), 1U, "NWD header length");
            for (const auto& chunk : chunks)
            {
                entries.push_back({
                    .name = chunk.name,
                    .storageMode = chunk.storageMode,
                    .physicalOffset = nextOffset,
                    .storedLength = chunk.stored.size(),
                    .prefixLength = chunk.prefixLength,
                    .indexLength = chunk.indexLength,
                });
                nextOffset = checkedAdd(nextOffset, chunk.stored.size(), "NWD chunk layout");
            }
            const auto directoryOffset = nextOffset;
            const auto directory = makeDirectory(entries);
            const auto compressedDirectory = compress(directory, "NWD chunk directory");
            const auto directoryChecksum = makeDirectoryChecksum(entries);

            auto output = ByteWriter{};
            output.write(ContainerHeader);
            output.writeU8('\n');
            for (const auto& chunk : chunks)
                output.write(chunk.stored);
            if (output.size() != directoryOffset)
                throw std::logic_error("Internal NWD directory offset mismatch");
            output.write(DirectoryHeader);
            output.writeU8('\n');
            for (size_t zero = 0U; zero < DirectoryPrefixZeros; ++zero)
                output.writeU8(0U);
            output.write(compressedDirectory);
            output.write(directoryChecksum);
            output.writeU64(directoryOffset);
            output.write(ContainerHeader);
            output.writeU8('\n');
            return std::move(output).take();
        }

        [[nodiscard]] inline Bytes makeContainer(SemanticChunks semantic)
        {
            auto chunks = std::vector<StoredChunk>{};
            const auto expectedChunkCount = MinimumSemanticChunkCount +
                (semantic.currentView.has_value() ? size_t{1U} : size_t{0U}) +
                (semantic.headlight.has_value() ? size_t{1U} : size_t{0U}) +
                (semantic.background.has_value() ? size_t{1U} : size_t{0U}) +
                (semantic.shadOverrides.has_value() ? size_t{1U} : size_t{0U}) +
                (semantic.lights.has_value() ? size_t{1U} : size_t{0U});
            chunks.reserve(expectedChunkCount);
            chunks.push_back(std::move(semantic.geometryCompress));
            chunks.push_back(std::move(semantic.geometry));
            const auto source = std::string{SemanticSourceNamespace};
            chunks.push_back(makeEmbeddedChunk(
                source + "\\LcOpNwdPartition", 1U, 0U, 0U,
                Archive448PartitionStoredBase64));
            chunks.push_back(makeEmbeddedChunk(
                source + "\\LcOaPartitionProps", 1U, 4U, 11U,
                Archive448PartitionPropsStoredBase64));
            chunks.push_back(std::move(semantic.logicalHierarchy));
            chunks.push_back(std::move(semantic.fragments));
            chunks.push_back(std::move(semantic.spatialHierarchy));
            if (semantic.currentView)
                chunks.push_back(std::move(*semantic.currentView));
            if (semantic.headlight)
                chunks.push_back(std::move(*semantic.headlight));
            if (semantic.background)
                chunks.push_back(std::move(*semantic.background));
            if (semantic.shadOverrides)
                chunks.push_back(std::move(*semantic.shadOverrides));
            if (semantic.lights)
                chunks.push_back(std::move(*semantic.lights));
            chunks.push_back(makeEmbeddedChunk(
                "LcOpOdyFileInfoChunk", 1U, 0U, 0U,
                Archive448FileInfoStoredBase64));

            if (chunks.size() != expectedChunkCount)
                throw std::logic_error("Freedom NWD profile has the wrong chunk count");

            return makeContainer(std::move(chunks));
        }

        inline constexpr uint32_t GeometryPageCapacity = 65536U;
        inline constexpr uint32_t MaximumTrianglesPerRecord = 21845U;

        struct MeshPart
        {
            uint32_t geometryReference = 0U;
            uint32_t triangleCount = 0U;
            std::array<float, 3> minimum{};
            std::array<float, 3> maximum{};
            uint32_t signature = 0U;
        };

        struct Occurrence
        {
            size_t mesh = 0U;
            uint32_t appearance = InvalidIndex;
            Transform transform{};
        };

        struct FragmentPlan
        {
            uint32_t logicalNodeReference = 0U;
            uint32_t occurrenceIndex = 0U;
            uint32_t partIndex = 0U;
        };

        struct LogicalPayload
        {
            Bytes decoded;
            std::vector<FragmentPlan> fragments;
            std::vector<uint32_t> outputNodeObjectIds;
            uint32_t nodeCount = 0U;
        };

        struct EncodedGeometryChunks
        {
            StoredChunk settings;
            StoredChunk geometry;
        };

        [[nodiscard]] inline uint32_t allocateObjectId(uint32_t& nextObjectId)
        {
            if (nextObjectId == std::numeric_limits<uint32_t>::max())
                throw std::overflow_error("NWD object ID range is exhausted");
            return nextObjectId++;
        }

        [[nodiscard]] inline float checkedFloat(const double value, const char* field)
        {
            if (!std::isfinite(value) ||
                value < -static_cast<double>(std::numeric_limits<float>::max()) ||
                value > static_cast<double>(std::numeric_limits<float>::max()))
            {
                throw std::invalid_argument(std::string("NWD output ") + field +
                                            " is not finite float geometry");
            }
            return static_cast<float>(value);
        }

        [[nodiscard]] inline int16_t quantizeNormal(const float value)
        {
            if (!std::isfinite(value))
                throw std::invalid_argument("NWD output normal is not finite");
            return static_cast<int16_t>(
                std::lround(std::clamp(value, -1.0F, 1.0F) * 32767.0F));
        }

        inline void writeSemanticU16(ByteWriter& writer, const uint16_t value)
        {
            writer.writeU8(static_cast<uint8_t>(value));
            writer.writeU8(static_cast<uint8_t>(value >> 8U));
        }

        inline void writeSemanticI16(ByteWriter& writer, const int16_t value)
        {
            writeSemanticU16(writer, static_cast<uint16_t>(value));
        }

        template <typename MeshLike>
        [[nodiscard]] inline Bytes makeGeometryRecord(
            const MeshLike& mesh, const uint32_t firstTriangle, const uint32_t triangleCount,
            std::array<float, 3>& minimum, std::array<float, 3>& maximum)
        {
            const auto firstCorner = static_cast<size_t>(firstTriangle) * 3U;
            const auto cornerCount = triangleCount * 3U;
            if (firstCorner > mesh.indices.size() ||
                cornerCount > mesh.indices.size() - firstCorner)
            {
                throw std::runtime_error("NWD output triangle range exceeds its index stream");
            }

            auto sourceVertices = std::vector<uint32_t>{};
            auto localIndices = std::vector<uint16_t>{};
            auto localIndexBySource = std::unordered_map<uint32_t, uint16_t>{};
            sourceVertices.reserve(std::min<size_t>(cornerCount, mesh.vertices.size()));
            localIndices.reserve(cornerCount);
            localIndexBySource.reserve(sourceVertices.capacity());

            for (uint32_t corner = 0U; corner < cornerCount; ++corner)
            {
                const auto sourceVertex = mesh.indices[firstCorner + corner];
                if (sourceVertex >= mesh.vertices.size() || sourceVertex >= mesh.normals.size())
                    throw std::runtime_error("NWD output triangle contains an invalid vertex index");
                const auto found = localIndexBySource.find(sourceVertex);
                if (found != localIndexBySource.end())
                {
                    localIndices.push_back(found->second);
                    continue;
                }
                if (sourceVertices.size() >=
                    static_cast<size_t>(std::numeric_limits<uint16_t>::max()))
                {
                    throw std::overflow_error(
                        "NWD output Geometry part exceeds the uint16 point range");
                }
                const auto localIndex = static_cast<uint16_t>(sourceVertices.size());
                sourceVertices.push_back(sourceVertex);
                localIndexBySource.emplace(sourceVertex, localIndex);
                localIndices.push_back(localIndex);
            }
            if (sourceVertices.empty())
                throw std::runtime_error("NWD output Geometry part has no referenced points");
            while (sourceVertices.size() < triangleCount)
                sourceVertices.push_back(sourceVertices.front());

            auto writer = ByteWriter{};
            const auto pointCount = static_cast<uint32_t>(sourceVertices.size());
            writer.writeU32(100U);
            writer.writeU32(94U);
            writer.writeU32(101U);
            writer.writeU32(60U);
            writer.writeU32(pointCount);
            minimum.fill(std::numeric_limits<float>::max());
            maximum.fill(std::numeric_limits<float>::lowest());
            for (const auto sourceVertex : sourceVertices)
            {
                const auto& point = mesh.vertices[sourceVertex];
                const auto values = std::array<float, 3>{
                    checkedFloat(point.x, "position"), checkedFloat(point.y, "position"),
                    checkedFloat(point.z, "position")};
                for (size_t axis = 0U; axis < values.size(); ++axis)
                {
                    writer.writeFloat(values[axis]);
                    minimum[axis] = std::min(minimum[axis], values[axis]);
                    maximum[axis] = std::max(maximum[axis], values[axis]);
                }
            }
            writer.writeU32(0U);
            writer.writeU32(102U);
            writer.writeU32(100U);
            writer.writeI32(static_cast<int32_t>(pointCount));
            for (const auto sourceVertex : sourceVertices)
            {
                const auto& normal = mesh.normals[sourceVertex];
                writeSemanticI16(writer, quantizeNormal(normal.x));
                writeSemanticI16(writer, quantizeNormal(normal.y));
                writeSemanticI16(writer, quantizeNormal(normal.z));
            }
            writer.align(4U);
            writer.writeU32(0U);
            writer.writeI32(static_cast<int32_t>(triangleCount));
            for (uint32_t triangle = 0U; triangle < triangleCount; ++triangle)
            {
                writer.writeU8(3U);
                writer.writeU8(0U);
            }
            writer.align(4U);
            writer.writeI32(static_cast<int32_t>(localIndices.size()));
            for (const auto localIndex : localIndices)
                writeSemanticU16(writer, localIndex);
            writer.align(8U);
            return std::move(writer).take();
        }

        [[nodiscard]] inline Bytes makeGeometrySettings()
        {
            static constexpr std::array<uint8_t, 40> NativeUncompressedSettings{
                0x00, 0x2D, 0xC3, 0x62, 0xD0, 0xA4, 0xCC, 0x40,
                0x0A, 0x62, 0xD4, 0xA4, 0x45, 0x6A, 0x91, 0x11,
                0x97, 0xA0, 0x44, 0x39, 0xB5, 0xC6, 0x45, 0xA0,
                0x08, 0x38, 0x33, 0x16, 0x29, 0x30, 0x9C, 0x9C,
                0x60, 0x09, 0x92, 0x4A, 0xDE, 0x16, 0xB2, 0x0F,
            };
            return {NativeUncompressedSettings.begin(), NativeUncompressedSettings.end()};
        }

        [[nodiscard]] inline EncodedGeometryChunks makeGeometryChunks(
            const std::span<const Bytes> records, const std::span<const uint32_t> recordSizes)
        {
            if (records.empty() || records.size() != recordSizes.size())
                throw std::logic_error("NWD output has no consistent Geometry records");
            auto decoded = Bytes{};
            auto decodedSize = size_t{};
            for (const auto& record : records)
            {
                if (record.size() > decoded.max_size() - decodedSize)
                    throw std::overflow_error("NWD Geometry decoded payload is too large");
                decodedSize += record.size();
            }
            decoded.reserve(decodedSize);
            for (const auto& record : records)
                decoded.insert(decoded.end(), record.begin(), record.end());

            auto compressedPages = std::vector<Bytes>{};
            auto pageSizes = std::vector<uint32_t>{};
            for (size_t offset = 0U; offset < decoded.size(); offset += GeometryPageCapacity)
            {
                const auto size = std::min<size_t>(GeometryPageCapacity, decoded.size() - offset);
                compressedPages.push_back(compress(
                    std::span<const uint8_t>{decoded.data() + offset, size}, "NWD Geometry page"));
                pageSizes.push_back(asU32(compressedPages.back().size(), "NWD Geometry page"));
            }

            auto index = ByteWriter{};
            index.writeU32(asU32(records.size(), "NWD Geometry record count"));
            index.writeU32(asU32(compressedPages.size(), "NWD Geometry page count"));
            for (const auto size : pageSizes)
                index.writeU32(size);
            for (const auto size : recordSizes)
                index.writeU32(size);
            if (records.size() > 1U)
                index.writeU32(0U);
            const auto compressedIndex = compress(index.bytes(), "NWD Geometry index");
            auto stored = ByteWriter{};
            stored.writeU32(GeometryPageCapacity);
            for (const auto& page : compressedPages)
                stored.write(page);
            stored.write(compressedIndex);
            const auto source = std::string{SemanticSourceNamespace};
            return {
                .settings = StoredChunk{.name = source + "\\LcOpNwdGeometryCompress",
                                        .storageMode = 3U,
                                        .stored = makeGeometrySettings()},
                .geometry = StoredChunk{.name = source + "\\LcOpNwdGeometry",
                                        .storageMode = 1U,
                                        .prefixLength = sizeof(uint32_t),
                                        .indexLength = asU32(compressedIndex.size(),
                                                             "NWD Geometry compressed index"),
                                        .stored = std::move(stored).take()},
            };
        }

        inline void writeSemanticVec3f(ByteWriter& writer, const std::array<float, 3>& value)
        {
            for (const auto component : value)
                writer.writeFloat(component);
        }

        inline void writeSemanticDefaultLogicalMaterial(ByteWriter& writer,
                                                        uint32_t& nextObjectId,
                                                        uint32_t& materialObjectId)
        {
            writer.writeU32(1U);
            if (materialObjectId != 0U)
            {
                writer.writeU32(1U);
                writer.writeU32(materialObjectId);
                return;
            }
            materialObjectId = allocateObjectId(nextObjectId);
            writer.writeU32(materialObjectId);
            writer.writeU32(31U);
            writer.writePaddedString("Default");
            writer.writeU32(0U);
            writer.writeU32(0U);
            writer.align(8U);
            for (size_t component = 0U; component < 3U; ++component)
                writer.writeDouble(0.0);
            for (size_t component = 0U; component < 3U; ++component)
                writer.writeDouble(0.8);
            for (size_t component = 0U; component < 6U; ++component)
                writer.writeDouble(0.0);
            writer.writeDouble(0.2);
            writer.writeDouble(0.0);
        }

        struct LogicalState
        {
            const std::vector<Node>& nodes;
            const std::vector<std::vector<uint32_t>>& children;
            const std::vector<bool>& active;
            const std::vector<std::vector<uint32_t>>& nodeOccurrences;
            const std::vector<Occurrence>& occurrences;
            const std::vector<std::vector<MeshPart>>& meshParts;
            uint32_t nextObjectId = 101U;
            uint32_t nextReference = 1U;
            uint32_t geometryClassObjectId = 0U;
            uint32_t materialObjectId = 0U;
            uint32_t nodeCount = 0U;
            std::vector<FragmentPlan> fragments;
            std::vector<uint32_t> outputNodeObjectIds;
        };

        inline void writeSemanticGeometryLeaf(
            ByteWriter& writer, const std::string& name,
            const std::span<const uint32_t> occurrenceIndices, LogicalState& state,
            const uint32_t sourceNodeIndex = InvalidIndex)
        {
            const auto reference = state.nextReference++;
            ++state.nodeCount;
            const auto objectId = allocateObjectId(state.nextObjectId);
            writer.writeU32(objectId);
            if (sourceNodeIndex != InvalidIndex)
            {
                if (sourceNodeIndex >= state.outputNodeObjectIds.size())
                    throw std::logic_error("NWD logical source-node mapping is out of range");
                if (state.outputNodeObjectIds[sourceNodeIndex] != 0U)
                    throw std::logic_error("NWD logical source node was emitted more than once");
                state.outputNodeObjectIds[sourceNodeIndex] = objectId;
            }
            writer.writeU32(22U);
            writer.writePaddedString(name.empty() ? "Geometry" : name);
            if (state.geometryClassObjectId == 0U)
            {
                state.geometryClassObjectId = allocateObjectId(state.nextObjectId);
                writer.writeU32(state.geometryClassObjectId);
                writer.writeU32(52U);
                writer.writePaddedString("LcOaExGeometry");
                writer.writePaddedString("LcOaExGeometry");
            }
            else
            {
                writer.writeU32(1U);
                writer.writeU32(state.geometryClassObjectId);
            }
            writer.writeU32(0U);
            writer.writeU32(0U);
            writeSemanticDefaultLogicalMaterial(
                writer, state.nextObjectId, state.materialObjectId);
            for (const auto occurrenceIndex : occurrenceIndices)
            {
                if (occurrenceIndex >= state.occurrences.size())
                    throw std::logic_error("NWD logical leaf occurrence is out of range");
                const auto& occurrence = state.occurrences[occurrenceIndex];
                if (occurrence.mesh >= state.meshParts.size())
                    throw std::logic_error("NWD logical leaf Geometry record is out of range");
                const auto& parts = state.meshParts[occurrence.mesh];
                for (size_t partIndex = 0U; partIndex < parts.size(); ++partIndex)
                {
                    state.fragments.push_back(FragmentPlan{
                        .logicalNodeReference = reference,
                        .occurrenceIndex = occurrenceIndex,
                        .partIndex = asU32(partIndex, "NWD mesh-part index"),
                    });
                }
            }
        }

        inline void writeSemanticLogicalNode(ByteWriter& writer, const uint32_t nodeIndex,
                                             LogicalState& state, const size_t depth)
        {
            if (depth > 1024U)
                throw std::invalid_argument("NWD output hierarchy exceeds the supported depth");
            if (nodeIndex >= state.nodes.size())
                throw std::logic_error("NWD logical node is out of range");
            const auto& node = state.nodes[nodeIndex];
            const auto& childIndices = state.children[nodeIndex];
            const auto& occurrenceIndices = state.nodeOccurrences[nodeIndex];
            const auto activeChildCount = static_cast<size_t>(std::count_if(
                childIndices.begin(), childIndices.end(),
                [&](const uint32_t child) { return state.active[child]; }));
            if (activeChildCount == 0U && !occurrenceIndices.empty())
            {
                writeSemanticGeometryLeaf(
                    writer, node.name, occurrenceIndices, state, nodeIndex);
                return;
            }

            static_cast<void>(state.nextReference++);
            ++state.nodeCount;
            const auto objectId = allocateObjectId(state.nextObjectId);
            writer.writeU32(objectId);
            if (nodeIndex >= state.outputNodeObjectIds.size())
                throw std::logic_error("NWD logical source-node mapping is out of range");
            if (state.outputNodeObjectIds[nodeIndex] != 0U)
                throw std::logic_error("NWD logical source node was emitted more than once");
            state.outputNodeObjectIds[nodeIndex] = objectId;
            writer.writeU32(53U);
            writer.writePaddedString(node.name.empty() ? "Group" : node.name);
            writer.writeU32(0U);
            writer.writeU32(0U);
            writer.writeU32(0U);
            writer.writeU32(0U);
            const auto syntheticLeaf = !occurrenceIndices.empty();
            writer.writeU32(asU32(activeChildCount + (syntheticLeaf ? 1U : 0U),
                                  "NWD logical child count"));
            if (syntheticLeaf)
            {
                writeSemanticGeometryLeaf(
                    writer, node.name.empty() ? "Geometry" : node.name + " Geometry",
                    occurrenceIndices, state);
            }
            for (const auto child : childIndices)
            {
                if (state.active[child])
                    writeSemanticLogicalNode(writer, child, state, depth + 1U);
            }
        }

        [[nodiscard]] inline LogicalPayload makeLogicalHierarchy(
            const std::vector<Node>& nodes,
            const std::vector<std::vector<uint32_t>>& children,
            const std::vector<uint32_t>& roots,
            const std::vector<std::vector<uint32_t>>& nodeOccurrences,
            const std::vector<Occurrence>& occurrences,
            const std::vector<std::vector<MeshPart>>& meshParts,
            const std::vector<uint32_t>& traversalOrder)
        {
            auto active = std::vector<bool>(nodes.size());
            for (auto cursor = traversalOrder.rbegin(); cursor != traversalOrder.rend(); ++cursor)
            {
                const auto nodeIndex = *cursor;
                auto value = !nodeOccurrences[nodeIndex].empty();
                for (const auto child : children[nodeIndex])
                    value = value || active[child];
                active[nodeIndex] = value;
            }
            const auto activeRootCount = static_cast<size_t>(std::count_if(
                roots.begin(), roots.end(), [&](const uint32_t root) { return active[root]; }));
            if (activeRootCount == 0U)
                throw std::invalid_argument("NWD source hierarchy contains no triangle geometry");
            auto writer = ByteWriter{};
            writer.writeU32(0U);
            writer.writeU32(100U);
            writer.writeU32(asU32(activeRootCount, "NWD logical root count"));
            auto state = LogicalState{
                .nodes = nodes,
                .children = children,
                .active = active,
                .nodeOccurrences = nodeOccurrences,
                .occurrences = occurrences,
                .meshParts = meshParts,
                .nextObjectId = 101U,
                .nextReference = 1U,
                .geometryClassObjectId = 0U,
                .materialObjectId = 0U,
                .nodeCount = 0U,
                .fragments = {},
                .outputNodeObjectIds = {},
            };
            state.outputNodeObjectIds.assign(nodes.size(), 0U);
            for (const auto root : roots)
            {
                if (active[root])
                    writeSemanticLogicalNode(writer, root, state, 0U);
            }
            if (state.fragments.empty())
                throw std::invalid_argument("NWD source hierarchy contains no geometry fragments");
            writer.writeU32(1U);
            writer.align(8U);
            return {.decoded = std::move(writer).take(),
                    .fragments = std::move(state.fragments),
                    .outputNodeObjectIds = std::move(state.outputNodeObjectIds),
                    .nodeCount = state.nodeCount};
        }

        struct MaterialKey
        {
            std::array<uint32_t, 14> values{};
            [[nodiscard]] bool operator==(const MaterialKey&) const noexcept = default;
        };

        struct MaterialKeyHash
        {
            [[nodiscard]] size_t operator()(const MaterialKey& key) const noexcept
            {
                auto result = static_cast<size_t>(1469598103934665603ULL);
                for (const auto value : key.values)
                {
                    result ^= static_cast<size_t>(value);
                    result *= static_cast<size_t>(1099511628211ULL);
                }
                return result;
            }
        };

        struct AppearanceWireState
        {
            std::vector<uint32_t> appearanceObjectIds;
            std::vector<uint32_t> metadataObjectIds;
            std::unordered_map<MaterialKey, uint32_t, MaterialKeyHash> materials;
        };

        [[nodiscard]] inline MaterialKey materialKey(const AppearanceMaterial& material)
        {
            const auto components = std::array<float, 14>{
                material.ambient[0], material.ambient[1], material.ambient[2],
                material.diffuse[0], material.diffuse[1], material.diffuse[2],
                material.specular[0], material.specular[1], material.specular[2],
                material.emissive[0], material.emissive[1], material.emissive[2],
                material.shininess, material.transparency,
            };
            auto result = MaterialKey{};
            for (size_t index = 0U; index < components.size(); ++index)
            {
                if (!std::isfinite(components[index]))
                    throw std::invalid_argument("NWD Fragment material is not finite");
                result.values[index] = std::bit_cast<uint32_t>(components[index]);
            }
            return result;
        }

        inline void writeSemanticNullablePaddedString(
            ByteWriter& writer, const std::optional<std::string>& value)
        {
            if (!value.has_value())
            {
                writer.writeI32(-1);
                return;
            }
            if (value->size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
                throw std::overflow_error("NWD nullable string exceeds the signed 32-bit range");
            writer.writeI32(static_cast<int32_t>(value->size()));
            writer.write(*value);
            writer.align(4U);
        }

        inline void writeSemanticMaterial(
            ByteWriter& writer, uint32_t& nextObjectId,
            std::unordered_map<MaterialKey, uint32_t, MaterialKeyHash>& materials,
            const AppearanceMaterial& material)
        {
            const auto key = materialKey(material);
            if (const auto found = materials.find(key); found != materials.end())
            {
                writer.writeU32(1U);
                writer.writeU32(found->second);
                return;
            }
            const auto materialId = allocateObjectId(nextObjectId);
            materials.emplace(key, materialId);
            writer.writeU32(materialId);
            writer.writeU32(54U);
            writeSemanticVec3f(writer, material.ambient);
            writeSemanticVec3f(writer, material.diffuse);
            writeSemanticVec3f(writer, material.specular);
            writeSemanticVec3f(writer, material.emissive);
            writer.writeFloat(material.shininess);
            writer.writeFloat(material.transparency);
        }

        inline void writeSemanticJsonMetadata(
            ByteWriter& writer, uint32_t& nextObjectId, AppearanceWireState& state,
            const std::span<const AppearanceMetadata> metadataCatalog,
            const size_t metadataIndex)
        {
            if (metadataIndex >= metadataCatalog.size() ||
                metadataIndex >= state.metadataObjectIds.size())
            {
                throw std::logic_error("NWD Fragment appearance metadata is out of range");
            }
            const auto& metadata = metadataCatalog[metadataIndex];
            if (metadata.metadataKind != AppearanceMetadataKind::jsonProtein)
                throw std::logic_error("NWD JSON appearance metadata has the wrong catalog kind");
            if (state.metadataObjectIds[metadataIndex] != 0U)
            {
                writer.writeU32(1U);
                writer.writeU32(state.metadataObjectIds[metadataIndex]);
                return;
            }
            const auto metadataId = allocateObjectId(nextObjectId);
            state.metadataObjectIds[metadataIndex] = metadataId;
            writer.writeU32(metadataId);
            writer.writeU32(185U);
            writer.writePaddedString(metadata.metadataJson);
            writeSemanticNullablePaddedString(writer, metadata.simpleMaterial);
            if (metadata.resources.size() >
                static_cast<size_t>(std::numeric_limits<int32_t>::max()))
            {
                throw std::overflow_error(
                    "NWD appearance resource count exceeds the signed 32-bit range");
            }
            writer.writeI32(static_cast<int32_t>(metadata.resources.size()));
            for (const auto& resource : metadata.resources)
            {
                writer.writePaddedString(resource.path);
                writer.writeU32(resource.storageMode);
                if (resource.storageMode != 0U)
                {
                    writeSemanticNullablePaddedString(writer, resource.producer);
                    writeSemanticNullablePaddedString(writer, resource.extension);
                    writer.align(8U);
                    writer.writeU64(static_cast<uint64_t>(resource.data.size()));
                    writer.write(resource.data);
                }
                else
                {
                    writeSemanticNullablePaddedString(writer, resource.resolvedPath);
                }
                writer.align(4U);
            }
        }

        inline void writeSemanticAppearance(
            ByteWriter& writer, uint32_t& nextObjectId, AppearanceWireState& state,
            const std::span<const Appearance> appearances,
            const std::span<const AppearanceMetadata> metadataCatalog,
            const uint32_t appearanceIndex)
        {
            if (appearanceIndex >= appearances.size() ||
                appearanceIndex >= state.appearanceObjectIds.size())
            {
                throw std::logic_error("NWD Fragment appearance is out of range");
            }
            const auto& appearance = appearances[appearanceIndex];
            if (!appearance.present)
            {
                writer.writeU32(0U);
                return;
            }
            if (state.appearanceObjectIds[appearanceIndex] != 0U)
            {
                writer.writeU32(1U);
                writer.writeU32(state.appearanceObjectIds[appearanceIndex]);
                return;
            }
            if (appearance.flags != 1U && appearance.flags != 17U &&
                appearance.flags != 33U && appearance.flags != 49U &&
                appearance.flags != 65U && appearance.flags != 81U &&
                appearance.flags != 97U && appearance.flags != 113U)
            {
                throw std::invalid_argument("Unsupported NWD Fragment appearance flags " +
                                            std::to_string(appearance.flags));
            }
            const auto appearanceId = allocateObjectId(nextObjectId);
            state.appearanceObjectIds[appearanceIndex] = appearanceId;
            writer.writeU32(appearanceId);
            writer.writeU32(55U);
            writer.writeU32(appearance.flags);
            writeSemanticMaterial(writer, nextObjectId, state.materials, appearance.material);
            if ((appearance.flags & 0x20U) != 0U)
            {
                if (appearance.metadata == std::numeric_limits<size_t>::max())
                {
                    writer.writeU32(0U);
                }
                else if (appearance.metadata >= metadataCatalog.size())
                {
                    throw std::logic_error("NWD Fragment appearance metadata is out of range");
                }
                else if (metadataCatalog[appearance.metadata].metadataKind ==
                         AppearanceMetadataKind::jsonProtein)
                {
                    writeSemanticJsonMetadata(writer, nextObjectId, state, metadataCatalog,
                                              appearance.metadata);
                }
                else if (metadataCatalog[appearance.metadata].metadataKind ==
                         AppearanceMetadataKind::legacyProtein)
                {
                    throw std::invalid_argument(
                        "Legacy class-182 NWD appearance metadata is not yet writable");
                }
                else
                {
                    throw std::logic_error("NWD Fragment appearance metadata has no wire kind");
                }
            }
            if ((appearance.flags & 0x40U) != 0U)
            {
                writer.align(4U);
                writer.writeU32(appearance.proteinTransparent ? 1U : 0U);
            }
        }

        inline void validateTransform(const Transform& transform)
        {
            for (const auto value : transform.values)
            {
                if (!std::isfinite(value))
                    throw std::invalid_argument("NWD fragment transform is not finite");
            }
            if (transform.values[12] != 0.0 || transform.values[13] != 0.0 ||
                transform.values[14] != 0.0 || transform.values[15] != 1.0)
            {
                throw std::invalid_argument("NWD fragment transform is projective");
            }
        }

        [[nodiscard]] inline bool isIdentityLinear(const Transform& transform) noexcept
        {
            const auto& matrix = transform.values;
            return matrix[0] == 1.0 && matrix[1] == 0.0 && matrix[2] == 0.0 &&
                   matrix[4] == 0.0 && matrix[5] == 1.0 && matrix[6] == 0.0 &&
                   matrix[8] == 0.0 && matrix[9] == 0.0 && matrix[10] == 1.0;
        }

        [[nodiscard]] inline bool hasTranslation(const Transform& transform) noexcept
        {
            return transform.values[3] != 0.0 || transform.values[7] != 0.0 ||
                   transform.values[11] != 0.0;
        }

        inline void writeSemanticFragmentOrigin(ByteWriter& writer, uint32_t& nextObjectId,
                                                const Transform& transform,
                                                const bool identityLinear)
        {
            if (identityLinear)
            {
                writer.writeU32(0U);
                return;
            }
            writer.writeU32(allocateObjectId(nextObjectId));
            writer.writeU32(17U);
            writer.align(8U);
            const std::array<std::array<size_t, 3>, 3> columns{{
                {{0U, 4U, 8U}}, {{1U, 5U, 9U}}, {{2U, 6U, 10U}},
            }};
            for (const auto& column : columns)
            {
                for (const auto component : column)
                    writer.writeFloat(checkedFloat(transform.values[component], "transform"));
                writer.writeFloat(0.0F);
            }
            writer.writeFloat(1.0F);
            writer.writeU32(0U);
            writer.writeDouble(transform.values[3]);
            writer.writeDouble(transform.values[7]);
            writer.writeDouble(transform.values[11]);
        }

        [[nodiscard]] inline Bytes makeFragments(
            const std::span<const FragmentPlan> plans,
            const std::span<const Occurrence> occurrences,
            const std::vector<std::vector<MeshPart>>& meshParts,
            const std::span<const Appearance> appearances,
            const std::span<const AppearanceMetadata> metadataCatalog,
            const std::span<const Instance> instances)
        {
            auto writer = ByteWriter{};
            writer.writeU32(asU32(plans.size(), "NWD Fragment count"));
            auto nextObjectId = uint32_t{100U};
            auto appearanceState = AppearanceWireState{};
            appearanceState.appearanceObjectIds.assign(appearances.size(), 0U);
            appearanceState.metadataObjectIds.assign(metadataCatalog.size(), 0U);
            for (const auto& plan : plans)
            {
                if (plan.occurrenceIndex >= occurrences.size() ||
                    plan.occurrenceIndex >= instances.size())
                {
                    throw std::logic_error("NWD Fragment occurrence is out of range");
                }
                const auto& occurrence = occurrences[plan.occurrenceIndex];
                const auto& instance = instances[plan.occurrenceIndex];
                if (occurrence.mesh >= meshParts.size() ||
                    plan.partIndex >= meshParts[occurrence.mesh].size())
                {
                    throw std::logic_error("NWD Fragment mesh part is out of range");
                }
                const auto& part = meshParts[occurrence.mesh][plan.partIndex];
                const auto identityLinear = isIdentityLinear(occurrence.transform);
                writer.writeU32(allocateObjectId(nextObjectId));
                writer.writeU32(13U);
                writer.align(8U);
                writer.writeDouble(1.0);
                writer.writeU32(part.triangleCount);
                writer.writeU32(4U);
                const auto placementFlags = identityLinear && !hasTranslation(occurrence.transform)
                                                ? 2U : 0x40000102U;
                writer.writeU32(placementFlags | (instance.transparent ? 0x1U : 0U));
                writer.writeU32(plan.logicalNodeReference);
                writeSemanticFragmentOrigin(
                    writer, nextObjectId, occurrence.transform, identityLinear);
                writeSemanticAppearance(writer, nextObjectId, appearanceState, appearances,
                                        metadataCatalog, occurrence.appearance);
                writer.align(4U);
                writer.writeU32(allocateObjectId(nextObjectId));
                writer.writeU32(93U);
                writeSemanticVec3f(writer, part.minimum);
                writeSemanticVec3f(writer, part.maximum);
                writer.align(8U);
                if (identityLinear)
                {
                    writer.writeDouble(occurrence.transform.values[3]);
                    writer.writeDouble(occurrence.transform.values[7]);
                    writer.writeDouble(occurrence.transform.values[11]);
                }
                else
                {
                    writer.writeDouble(0.0);
                    writer.writeDouble(0.0);
                    writer.writeDouble(0.0);
                }
                writer.writeFloat(1.0F);
                writer.writeU32(part.signature);
                writer.writeU32(part.geometryReference);
            }
            writer.writeU32(0U);
            return std::move(writer).take();
        }

        [[nodiscard]] inline std::array<double, 3> transformPoint(
            const Transform& transform, const std::array<double, 3>& point) noexcept
        {
            const auto& matrix = transform.values;
            return {
                matrix[0] * point[0] + matrix[1] * point[1] + matrix[2] * point[2] + matrix[3],
                matrix[4] * point[0] + matrix[5] * point[1] + matrix[6] * point[2] + matrix[7],
                matrix[8] * point[0] + matrix[9] * point[1] + matrix[10] * point[2] + matrix[11],
            };
        }

        [[nodiscard]] inline std::vector<SpatialFragmentBounds> makeSpatialFragmentBounds(
            const std::span<const FragmentPlan> plans,
            const std::span<const Occurrence> occurrences,
            const std::vector<std::vector<MeshPart>>& meshParts)
        {
            auto result = std::vector<SpatialFragmentBounds>{};
            result.reserve(plans.size());
            for (size_t fragmentIndex = 0U; fragmentIndex < plans.size(); ++fragmentIndex)
            {
                const auto& plan = plans[fragmentIndex];
                if (plan.occurrenceIndex >= occurrences.size())
                    throw std::logic_error("NWD spatial Fragment occurrence is out of range");
                const auto& occurrence = occurrences[plan.occurrenceIndex];
                if (occurrence.mesh >= meshParts.size() ||
                    plan.partIndex >= meshParts[occurrence.mesh].size())
                {
                    throw std::logic_error("NWD spatial Fragment mesh part is out of range");
                }
                const auto& part = meshParts[occurrence.mesh][plan.partIndex];
                auto minimum = std::array<double, 3>{
                    std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                    std::numeric_limits<double>::max()};
                auto maximum = std::array<double, 3>{
                    std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest(),
                    std::numeric_limits<double>::lowest()};
                for (uint32_t corner = 0U; corner < 8U; ++corner)
                {
                    const auto local = std::array<double, 3>{
                        (corner & 1U) != 0U ? part.maximum[0] : part.minimum[0],
                        (corner & 2U) != 0U ? part.maximum[1] : part.minimum[1],
                        (corner & 4U) != 0U ? part.maximum[2] : part.minimum[2],
                    };
                    const auto world = transformPoint(occurrence.transform, local);
                    for (size_t axis = 0U; axis < world.size(); ++axis)
                    {
                        if (!std::isfinite(world[axis]))
                        {
                            throw std::invalid_argument(
                                "NWD spatial Fragment world bounds are not finite");
                        }
                        minimum[axis] = std::min(minimum[axis], world[axis]);
                        maximum[axis] = std::max(maximum[axis], world[axis]);
                    }
                }
                result.push_back(SpatialFragmentBounds{
                    .fragmentIndex = asU32(fragmentIndex, "NWD spatial Fragment index"),
                    .minimum = minimum,
                    .maximum = maximum,
                });
            }
            return result;
        }
    }

    [[nodiscard]] inline Bytes makeSemantic(const Scene& scene)
    {
        if (scene.nodes.empty())
            throw std::invalid_argument("NWD semantic scene has no nodes");
        if (scene.nodes.size() > std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("NWD semantic node count exceeds the 32-bit range");

        auto records = std::vector<Bytes>{};
        auto recordSizes = std::vector<uint32_t>{};
        auto meshParts = std::vector<std::vector<detail::MeshPart>>(scene.meshes.size());
        for (size_t meshIndex = 0U; meshIndex < scene.meshes.size(); ++meshIndex)
        {
            const auto& mesh = scene.meshes[meshIndex];
            // A geometry slot without triangles is intentionally omitted, but
            // a non-empty index stream must always describe a complete mesh.
            if (mesh.indices.empty())
                continue;
            if (mesh.vertices.empty())
                throw std::invalid_argument("NWD semantic mesh has indices but no vertices");
            if (mesh.normals.size() != mesh.vertices.size() || mesh.indices.size() % 3U != 0U)
                throw std::invalid_argument("NWD semantic mesh has incomplete triangle geometry");
            const auto triangleCount = mesh.indices.size() / 3U;
            if (triangleCount > std::numeric_limits<uint32_t>::max())
                throw std::overflow_error("NWD semantic mesh triangle count exceeds the 32-bit range");
            for (uint32_t first = 0U; first < triangleCount;
                 first += detail::MaximumTrianglesPerRecord)
            {
                const auto count = std::min<uint32_t>(
                    detail::MaximumTrianglesPerRecord,
                    static_cast<uint32_t>(triangleCount) - first);
                auto minimum = std::array<float, 3>{};
                auto maximum = std::array<float, 3>{};
                auto record = detail::makeGeometryRecord(mesh, first, count, minimum, maximum);
                auto signature = detail::adler32(record);
                if (signature == 0U)
                    signature = 1U;
                records.push_back(std::move(record));
                recordSizes.push_back(detail::asU32(records.back().size(), "NWD Geometry record"));
                meshParts[meshIndex].push_back(detail::MeshPart{
                    .geometryReference = detail::asU32(records.size(), "NWD Geometry reference"),
                    .triangleCount = count,
                    .minimum = minimum,
                    .maximum = maximum,
                    .signature = signature,
                });
            }
        }
        if (records.empty())
            throw std::invalid_argument("NWD semantic scene has no triangle geometry");

        auto appearances = scene.appearances;
        auto defaultAppearance = InvalidIndex;
        auto resolveAppearance = [&](const uint32_t requested) -> uint32_t
        {
            if (requested != InvalidIndex)
            {
                if (requested >= appearances.size())
                    throw std::invalid_argument("NWD semantic appearance is out of range");
                return requested;
            }
            if (defaultAppearance == InvalidIndex)
            {
                defaultAppearance = detail::asU32(appearances.size(), "NWD appearance index");
                auto appearance = Appearance{};
                appearance.material.diffuse = {1.0F, 1.0F, 1.0F};
                appearance.material.shininess = 0.2F;
                appearances.push_back(appearance);
            }
            return defaultAppearance;
        };

        auto children = std::vector<std::vector<uint32_t>>(scene.nodes.size());
        auto roots = std::vector<uint32_t>{};
        roots.reserve(scene.nodes.size());
        for (size_t nodeIndex = 0U; nodeIndex < scene.nodes.size(); ++nodeIndex)
        {
            const auto parent = scene.nodes[nodeIndex].parent;
            if (parent == nodeIndex || parent == std::numeric_limits<size_t>::max())
            {
                roots.push_back(detail::asU32(nodeIndex, "NWD root node index"));
                continue;
            }
            if (parent >= scene.nodes.size())
                throw std::invalid_argument("NWD semantic node parent is out of range");
            children[parent].push_back(detail::asU32(nodeIndex, "NWD child node index"));
        }
        if (roots.empty())
            throw std::invalid_argument("NWD semantic hierarchy has no root");

        auto traversalOrder = std::vector<uint32_t>{};
        traversalOrder.reserve(scene.nodes.size());
        auto visited = std::vector<bool>(scene.nodes.size());
        auto stack = std::vector<std::pair<uint32_t, size_t>>{};
        for (auto root = roots.rbegin(); root != roots.rend(); ++root)
            stack.emplace_back(*root, 0U);
        while (!stack.empty())
        {
            const auto [current, depth] = stack.back();
            stack.pop_back();
            if (depth > 1024U)
                throw std::invalid_argument("NWD semantic hierarchy exceeds the supported depth");
            if (visited[current])
                throw std::invalid_argument("NWD semantic hierarchy contains a cycle");
            visited[current] = true;
            traversalOrder.push_back(current);
            const auto& nodeChildren = children[current];
            for (auto child = nodeChildren.rbegin(); child != nodeChildren.rend(); ++child)
                stack.emplace_back(*child, depth + 1U);
        }
        if (traversalOrder.size() != scene.nodes.size())
            throw std::invalid_argument("NWD semantic hierarchy is not connected to a root");

        auto occurrences = std::vector<detail::Occurrence>{};
        auto activeInstances = std::vector<Instance>{};
        auto nodeOccurrences = std::vector<std::vector<uint32_t>>(scene.nodes.size());
        occurrences.reserve(scene.instances.size());
        activeInstances.reserve(scene.instances.size());
        for (const auto& instance : scene.instances)
        {
            if (instance.node >= scene.nodes.size())
                throw std::invalid_argument("NWD semantic Instance node is out of range");
            if (instance.mesh >= scene.meshes.size())
                throw std::invalid_argument("NWD semantic Instance mesh is out of range");
            if (meshParts[instance.mesh].empty())
                continue;
            detail::validateTransform(instance.transform);
            auto normalized = instance;
            normalized.appearance = resolveAppearance(instance.appearance);
            const auto occurrenceIndex = detail::asU32(
                occurrences.size(), "NWD Fragment occurrence index");
            occurrences.push_back(detail::Occurrence{
                .mesh = normalized.mesh,
                .appearance = normalized.appearance,
                .transform = normalized.transform,
            });
            activeInstances.push_back(std::move(normalized));
            nodeOccurrences[instance.node].push_back(occurrenceIndex);
        }
        if (occurrences.empty())
            throw std::invalid_argument("NWD semantic scene has no geometry instances");

        auto geometry = detail::makeGeometryChunks(records, recordSizes);
        auto logical = detail::makeLogicalHierarchy(
            scene.nodes, children, roots, nodeOccurrences, occurrences, meshParts, traversalOrder);
        auto fragments = detail::makeFragments(
            logical.fragments, occurrences, meshParts, appearances, scene.appearanceMetadata,
            activeInstances);
        auto spatialBounds = detail::makeSpatialFragmentBounds(
            logical.fragments, occurrences, meshParts);
        const auto source = std::string{detail::SemanticSourceNamespace};
        return detail::makeContainer(detail::SemanticChunks{
            .geometryCompress = std::move(geometry.settings),
            .geometry = std::move(geometry.geometry),
            .logicalHierarchy = detail::compressedChunk(
                source + "\\LcOpNwdLogicalHierarchy", std::move(logical.decoded)),
            .fragments = detail::compressedChunk(
                source + "\\LcOpNwdFragments", std::move(fragments)),
            .spatialHierarchy = detail::compressedChunk(
                source + "\\LcOpNwdSpatialHierarchy",
                detail::makeSpatialHierarchy(spatialBounds)),
            .currentView = detail::makeCurrentView(spatialBounds),
            .headlight = std::nullopt,
            .background = detail::compressedChunk(
                source + "\\LcOpBackgroundElement",
                detail::decodeBase64(detail::CanonicalGreyBackgroundDecodedBase64)),
            .shadOverrides = std::nullopt,
            .lights = std::nullopt,
        });
    }

    inline void writeSemantic(const std::filesystem::path& target, const Scene& scene)
    {
        if (target.empty() || target.filename().empty())
            throw std::invalid_argument("NWD target path must name a file");
        const auto parent = target.parent_path();
        auto error = std::error_code{};
        if (!parent.empty() && (!std::filesystem::is_directory(parent, error) || error))
            throw std::runtime_error("NWD target directory does not exist: " + parent.string());
        const auto container = makeSemantic(scene);
        auto temporary = ciff::conversion::AtomicFile::create(target);
        temporary.write(container);
        temporary.publish();
    }
}

#ifdef FALCON_NWD_WRITE_RESTORE_MAX_MACRO
#pragma pop_macro("max")
#undef FALCON_NWD_WRITE_RESTORE_MAX_MACRO
#endif

#ifdef FALCON_NWD_WRITE_RESTORE_MIN_MACRO
#pragma pop_macro("min")
#undef FALCON_NWD_WRITE_RESTORE_MIN_MACRO
#endif
