/*----------------------------------------------------------------
  ConvertDWG.h

  Native core reused from approved local AutodeskFBX/ConvertDWG.h.
  Adapted only for the ciff module namespace; no FBX runtime dependency.
  Original independent implementation and method references retained.

  Independently authored, bounded AC1015 / AutoCAD 2000 DWG writer.
  Method reference: Open Design Specification for .dwg files, v5.4.1,
  https://www.opendesign.com/files/guestdownloads/OpenDesign_Specification_for_.dwg_files.pdf
  accessed 2026-10-09, sections 2, 3, 9, 20.4.1, 20.4.32,
  20.4.52-54, 23.1, 26, 27. Approved local AutocadDWG reader was used to
  cross-check field interpretation. No external implementation imported.
----------------------------------------------------------------*/
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>
#include <string_view>
#include <unordered_map>

#include "ConversionOutput.h"
#include "DefaultDWG.h"
#include "Convert.h"
#include "ProcessCIFF.h"
#include "CmdBar.h"
#include "CmdArgs.h"
#include "WriteBuffer.h"

namespace ciff::dwg::write
{
    using Bytes = std::vector<uint8_t>;

    struct Point3d
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct Face
    {
        std::array<Point3d, 3> vertices{};
        uint16_t color = 7U;
        size_t layerIndex = 0U;
    };

    struct Scene
    {
        // Empty layers selects the seed's layer 0. Otherwise layerIndex
        // indexes this list; names must be unique ASCII DWG layer names.
        std::vector<std::string> layers;
        std::vector<Face> faces;
        uint16_t insunits = 6U; // meters
    };

    namespace detail
    {
        inline void require(bool condition, const char* message)
        {
            if (!condition)
            {
                throw std::runtime_error(message);
            }
        }

        inline uint16_t crc16(std::span<const uint8_t> data, uint16_t crc = 0xC0C1U)
        {
            for (uint8_t byte : data)
            {
                crc ^= byte;
                for (unsigned bit = 0; bit < 8U; ++bit)
                {
                    crc = static_cast<uint16_t>((crc >> 1U) ^ ((crc & 1U) ? 0xA001U : 0U));
                }
            }
            return crc;
        }

        inline uint64_t little(std::span<const uint8_t> bytes, size_t position, unsigned count)
        {
            require(position <= bytes.size() && count <= bytes.size() - position, "DWG: truncated integer");
            uint64_t value = 0U;
            for (unsigned i = 0; i < count; ++i)
            {
                value |= static_cast<uint64_t>(bytes[position + i]) << (8U * i);
            }
            return value;
        }

        inline void appendLittle(Bytes& bytes, uint64_t value, unsigned count)
        {
            for (unsigned i = 0; i < count; ++i)
            {
                bytes.push_back(static_cast<uint8_t>(value >> (8U * i)));
            }
        }

        inline void patchLittle(Bytes& bytes, size_t position, uint64_t value, unsigned count)
        {
            require(position <= bytes.size() && count <= bytes.size() - position, "DWG: invalid integer patch");
            for (unsigned i = 0; i < count; ++i)
            {
                bytes[position + i] = static_cast<uint8_t>(value >> (8U * i));
            }
        }

        struct Handle
        {
            uint8_t code = 0U;
            uint64_t value = 0U;
        };

        class BitInput
        {
          public:
            explicit BitInput(std::span<const uint8_t> source) : bytes(source)
            {
            }
            uint64_t bits(unsigned count)
            {
                require(count <= 64U && cursor <= bytes.size() * 8U && count <= bytes.size() * 8U - cursor,
                        "DWG: truncated bit stream");
                uint64_t value = 0U;
                while (count-- != 0U)
                {
                    value = (value << 1U) | ((bytes[cursor / 8U] >> (7U - cursor % 8U)) & 1U);
                    ++cursor;
                }
                return value;
            }
            uint64_t raw(unsigned count)
            {
                uint64_t value = 0U;
                for (unsigned i = 0; i < count; ++i)
                {
                    value |= bits(8U) << (8U * i);
                }
                return value;
            }
            uint16_t shortValue()
            {
                switch (bits(2U))
                {
                    case 0:
                        return static_cast<uint16_t>(raw(2U));
                    case 1:
                        return static_cast<uint16_t>(bits(8U));
                    case 2:
                        return 0U;
                    default:
                        return 256U;
                }
            }
            uint32_t longValue()
            {
                const auto code = bits(2U);
                require(code != 3U, "DWG: invalid bitlong");
                return code == 0U ? static_cast<uint32_t>(raw(4U)) : code == 1U ? static_cast<uint32_t>(bits(8U)) : 0U;
            }
            double doubleValue()
            {
                const auto code = bits(2U);
                require(code != 3U, "DWG: invalid bitdouble");
                return code == 0U ? std::bit_cast<double>(raw(8U)) : code == 1U ? 1.0 : 0.0;
            }
            Handle handle()
            {
                Handle result;
                result.code = static_cast<uint8_t>(bits(4U));
                const auto count = bits(4U);
                require(count <= 8U, "DWG: invalid handle length");
                for (uint64_t i = 0U; i < count; ++i)
                {
                    result.value = (result.value << 8U) | bits(8U);
                }
                return result;
            }
            std::string text()
            {
                const auto count = shortValue();
                std::string result;
                result.reserve(count);
                for (unsigned i = 0; i < count; ++i)
                {
                    result.push_back(static_cast<char>(bits(8U)));
                }
                return result;
            }
            void doubles(size_t count)
            {
                while (count-- != 0U)
                {
                    (void)doubleValue();
                }
            }
            void shorts(size_t count)
            {
                while (count-- != 0U)
                {
                    (void)shortValue();
                }
            }
            void longs(size_t count)
            {
                while (count-- != 0U)
                {
                    (void)longValue();
                }
            }
            void handles(size_t count)
            {
                while (count-- != 0U)
                {
                    (void)handle();
                }
            }
            void texts(size_t count)
            {
                while (count-- != 0U)
                {
                    (void)text();
                }
            }
            size_t cursor = 0U;
            std::span<const uint8_t> bytes;
        };

        class BitOutput
        {
          public:
            void bits(uint64_t value, unsigned count)
            {
                require(count <= 64U, "DWG: invalid bit output count");
                while (count-- != 0U)
                {
                    if (cursor % 8U == 0U)
                    {
                        bytes.push_back(0U);
                    }
                    bytes.back() |= static_cast<uint8_t>(((value >> count) & 1U) << (7U - cursor % 8U));
                    ++cursor;
                }
            }
            void raw(uint64_t value, unsigned count)
            {
                for (unsigned i = 0; i < count; ++i)
                {
                    bits(static_cast<uint8_t>(value >> (8U * i)), 8U);
                }
            }
            void shortValue(uint16_t value)
            {
                if (value == 0U)
                {
                    bits(2U, 2U);
                }
                else if (value == 256U)
                {
                    bits(3U, 2U);
                }
                else if (value < 256U)
                {
                    bits(1U, 2U);
                    bits(value, 8U);
                }
                else
                {
                    bits(0U, 2U);
                    raw(value, 2U);
                }
            }
            void longValue(uint32_t value)
            {
                if (value == 0U)
                {
                    bits(2U, 2U);
                }
                else if (value < 256U)
                {
                    bits(1U, 2U);
                    bits(value, 8U);
                }
                else
                {
                    bits(0U, 2U);
                    raw(value, 4U);
                }
            }
            void doubleValue(double value)
            {
                if (value == 0.0)
                {
                    bits(2U, 2U);
                }
                else if (value == 1.0)
                {
                    bits(1U, 2U);
                }
                else
                {
                    bits(0U, 2U);
                    raw(std::bit_cast<uint64_t>(value), 8U);
                }
            }
            void defaultDouble(double value, double defaultValue)
            {
                if (value == defaultValue)
                {
                    bits(0U, 2U);
                }
                else
                {
                    bits(3U, 2U);
                    raw(std::bit_cast<uint64_t>(value), 8U);
                }
            }
            void handle(Handle value)
            {
                unsigned count = 0U;
                for (auto remaining = value.value; remaining != 0U; remaining >>= 8U)
                {
                    ++count;
                }
                bits(value.code, 4U);
                bits(count, 4U);
                for (unsigned i = count; i != 0U; --i)
                {
                    bits(static_cast<uint8_t>(value.value >> (8U * (i - 1U))), 8U);
                }
            }
            void text(const std::string& value)
            {
                require(value.size() <= 255U, "DWG: layer name exceeds 255 bytes");
                shortValue(static_cast<uint16_t>(value.size()));
                for (unsigned char byte : value)
                {
                    bits(byte, 8U);
                }
            }
            void copy(std::span<const uint8_t> source, size_t begin, size_t end)
            {
                require(begin <= end && end <= source.size() * 8U, "DWG: invalid bit copy");
                for (size_t i = begin; i < end; ++i)
                {
                    bits((source[i / 8U] >> (7U - i % 8U)) & 1U, 1U);
                }
            }
            void patchRaw(size_t position, uint64_t value, unsigned count)
            {
                require(position + count * 8U <= cursor, "DWG: invalid bit patch");
                for (unsigned i = 0; i < count * 8U; ++i)
                {
                    const size_t location = position + i;
                    const auto mask = static_cast<uint8_t>(1U << (7U - location % 8U));
                    const auto bit = static_cast<uint8_t>((value >> ((i / 8U) * 8U + 7U - i % 8U)) & 1U);
                    bytes[location / 8U] = static_cast<uint8_t>((bytes[location / 8U] & ~mask) | (bit ? mask : 0U));
                }
            }
            Bytes bytes;
            size_t cursor = 0U;
        };

        inline uint64_t modular(std::span<const uint8_t> bytes, size_t& position, bool signedValue)
        {
            uint64_t value = 0U;
            for (unsigned shift = 0U; shift < 63U; shift += 7U)
            {
                require(position < bytes.size(), "DWG: truncated modular integer");
                const auto byte = bytes[position++];
                const bool last = (byte & 0x80U) == 0U;
                value |= static_cast<uint64_t>(byte & (last && signedValue ? 0x3FU : 0x7FU)) << shift;
                if (last)
                {
                    return signedValue && (byte & 0x40U) ? uint64_t{0U} - value : value;
                }
            }
            throw std::runtime_error("DWG: modular integer exceeds range");
        }

        inline void appendModular(Bytes& bytes, uint64_t value, bool signedValue)
        {
            while (value >= (signedValue ? 64U : 128U))
            {
                bytes.push_back(static_cast<uint8_t>((value & 127U) | 128U));
                value >>= 7U;
            }
            bytes.push_back(static_cast<uint8_t>(value));
        }

        using ObjectMap = std::map<uint64_t, uint32_t>;

        inline ObjectMap parseMap(std::span<const uint8_t> bytes)
        {
            ObjectMap objects;
            size_t position = 0U;
            bool terminated = false;
            while (position + 4U <= bytes.size())
            {
                const size_t start = position;
                const auto length = static_cast<size_t>((bytes[position] << 8U) | bytes[position + 1U]);
                require(length >= 2U && length <= 2032U && length + 2U <= bytes.size() - position,
                        "DWG: invalid object map page size");
                require(crc16(bytes.subspan(start, length)) ==
                            static_cast<uint16_t>((bytes[start + length] << 8U) | bytes[start + length + 1U]),
                        "DWG: object map CRC mismatch");
                position += 2U;
                uint64_t handle = 0U;
                int64_t location = 0;
                const auto page = bytes.first(start + length);
                while (position < start + length)
                {
                    handle += modular(page, position, false);
                    location += std::bit_cast<int64_t>(modular(page, position, true));
                    require(handle != 0U && location >= 0 && static_cast<uint64_t>(location) <= UINT32_MAX,
                            "DWG: invalid object map entry");
                    require(objects.emplace(handle, static_cast<uint32_t>(location)).second,
                            "DWG: duplicate object map handle");
                }
                position += 2U;
                if (length == 2U)
                {
                    terminated = true;
                    break;
                }
            }
            require(terminated && position == bytes.size(), "DWG: incomplete object map");
            return objects;
        }

        inline Bytes encodeMap(const ObjectMap& objects)
        {
            Bytes result;
            Bytes page(2U, 0U);
            uint64_t previousHandle = 0U;
            int64_t previousLocation = 0;
            auto finish = [&]() {
                page[0] = static_cast<uint8_t>(page.size() >> 8U);
                page[1] = static_cast<uint8_t>(page.size());
                const auto crc = crc16(page);
                page.push_back(static_cast<uint8_t>(crc >> 8U));
                page.push_back(static_cast<uint8_t>(crc));
                result.insert(result.end(), page.begin(), page.end());
                page.assign(2U, 0U);
                previousHandle = 0U;
                previousLocation = 0;
            };
            for (const auto& [handle, location] : objects)
            {
                Bytes entry;
                appendModular(entry, handle - previousHandle, false);
                const int64_t delta = static_cast<int64_t>(location) - previousLocation;
                const bool negative = delta < 0;
                appendModular(entry, static_cast<uint64_t>(negative ? -delta : delta), true);
                if (negative)
                {
                    entry.back() |= 0x40U;
                }
                if (page.size() + entry.size() > 2032U)
                {
                    finish();
                    entry.clear();
                    appendModular(entry, handle, false);
                    appendModular(entry, location, true);
                }
                page.insert(page.end(), entry.begin(), entry.end());
                previousHandle = handle;
                previousLocation = location;
            }
            if (page.size() != 2U)
            {
                finish();
            }
            finish();
            return result;
        }

        inline Bytes objectFrame(BitOutput payload)
        {
            Bytes result;
            require(payload.bytes.size() < 0x40000000U, "DWG: object exceeds size limit");
            const auto size = static_cast<uint32_t>(payload.bytes.size());
            appendLittle(result, size < 0x8000U ? size : (size & 0x7FFFU) | 0x8000U, 2U);
            if (size >= 0x8000U)
            {
                appendLittle(result, size >> 15U, 2U);
            }
            result.insert(result.end(), payload.bytes.begin(), payload.bytes.end());
            appendLittle(result, crc16(result), 2U);
            return result;
        }

        struct Object
        {
            std::span<const uint8_t> payload;
            uint16_t type = 0U;
            uint32_t handleStart = 0U;
            uint64_t handle = 0U;
            uint32_t reactors = 0U;
            size_t commonEnd = 0U;
        };

        inline Object readObject(std::span<const uint8_t> bytes, size_t location)
        {
            const auto first = little(bytes, location, 2U);
            const size_t sizeBytes = first & 0x8000U ? 4U : 2U;
            const auto size = first & 0x8000U ? (first & 0x7FFFU) | (little(bytes, location + 2U, 2U) << 15U) : first;
            require(location + sizeBytes <= bytes.size() && size + 2U <= bytes.size() - location - sizeBytes,
                    "DWG: truncated object");
            require(crc16(bytes.subspan(location, sizeBytes + size)) == little(bytes, location + sizeBytes + size, 2U),
                    "DWG: object CRC mismatch");
            Object object;
            object.payload = bytes.subspan(location + sizeBytes, size);
            BitInput bits(object.payload);
            object.type = bits.shortValue();
            object.handleStart = static_cast<uint32_t>(bits.raw(4U));
            require(object.handleStart <= object.payload.size() * 8U, "DWG: invalid object handle stream");
            const auto ownHandle = bits.handle();
            require(ownHandle.code == 0U, "DWG: unsupported seed object handle code");
            object.handle = ownHandle.value;
            require(bits.shortValue() == 0U, "DWG: seed contains unsupported extended data");
            object.commonEnd = bits.cursor;
            return object;
        }

        inline BitOutput nonEntityPrefix(uint16_t type, uint64_t handle)
        {
            BitOutput bits;
            bits.shortValue(type);
            bits.raw(0U, 4U);
            bits.handle({0U, handle});
            bits.shortValue(0U); // no extended data
            bits.longValue(0U);  // no reactors
            return bits;
        }

        inline Bytes faceObject(const Face& face, uint64_t handle, uint64_t layer, uint64_t previous, uint64_t next)
        {
            BitOutput bits;
            bits.shortValue(0x1CU);
            const auto bitSizePosition = bits.cursor;
            bits.raw(0U, 4U);
            bits.handle({0U, handle});
            bits.shortValue(0U); // EED
            bits.bits(0U, 1U);   // graphics absent
            bits.bits(2U, 2U);   // model space
            bits.longValue(0U);  // reactors
            bits.bits(0U, 1U);   // explicit previous/next links
            bits.shortValue(face.color);
            bits.doubleValue(1.0); // linetype scale
            bits.bits(0U, 2U);     // bylayer linetype
            bits.bits(0U, 2U);     // bylayer plotstyle
            bits.shortValue(0U);   // visible
            bits.bits(29U, 8U);    // bylayer lineweight
            bits.bits(1U, 1U);     // no invisible-edge flags
            bits.bits(face.vertices[0].z == 0.0 ? 1U : 0U, 1U);
            bits.raw(std::bit_cast<uint64_t>(face.vertices[0].x), 8U);
            bits.raw(std::bit_cast<uint64_t>(face.vertices[0].y), 8U);
            if (face.vertices[0].z != 0.0)
            {
                bits.raw(std::bit_cast<uint64_t>(face.vertices[0].z), 8U);
            }
            for (unsigned i = 1U; i < 4U; ++i)
            {
                const auto& point = face.vertices[std::min(i, 2U)];
                const auto& prior = face.vertices[i - 1U];
                bits.defaultDouble(point.x, prior.x);
                bits.defaultDouble(point.y, prior.y);
                bits.defaultDouble(point.z, prior.z);
            }
            bits.patchRaw(bitSizePosition, bits.cursor, 4U);
            bits.handle({3U, 0U}); // null extension dictionary
            bits.handle({4U, previous});
            bits.handle({4U, next});
            bits.handle({5U, layer});
            return objectFrame(std::move(bits));
        }

        inline Bytes replaceBlockEntities(const Object& object, uint64_t first, uint64_t last)
        {
            BitInput data(object.payload);
            data.cursor = object.commonEnd;
            const auto reactors = data.longValue();
            const auto name = data.text();
            require(name == "*Model_Space" || name == "*MODEL_SPACE", "DWG: seed model-space block missing");
            (void)data.bits(1U);
            (void)data.shortValue();
            (void)data.bits(3U); // dependent, anonymous, attributes
            require(data.bits(2U) == 0U, "DWG: seed model space is an external reference");
            (void)data.bits(1U); // loaded
            data.doubles(3U);
            (void)data.text(); // reference path
            size_t inserts = 0U;
            while (data.bits(8U) != 0U)
            {
                ++inserts;
            }
            (void)data.text();
            const auto preview = data.longValue();
            for (uint32_t i = 0U; i < preview; ++i)
            {
                (void)data.bits(8U);
            }
            require(data.cursor == object.handleStart, "DWG: unsupported seed block fields");
            data.handles(1U + reactors + 3U); // control, reactors, dictionary, null, BLOCK
            const auto firstPosition = data.cursor;
            require(data.handle().value == 0U && data.handle().value == 0U, "DWG: seed model-space block is not empty");
            const auto tailPosition = data.cursor;
            data.handles(1U + inserts + 1U); // ENDBLK, inserts, layout
            BitOutput output;
            output.copy(object.payload, 0U, firstPosition);
            output.handle({4U, first});
            output.handle({4U, last});
            output.copy(object.payload, tailPosition, data.cursor);
            return objectFrame(std::move(output));
        }

        struct LayerProfile
        {
            uint64_t control = 0U;
            std::vector<Handle> references;
            uint16_t values = 0U;
        };

        inline uint64_t resolveHandle(uint64_t ownHandle, Handle handle)
        {
            if (handle.code <= 5U)
            {
                return handle.value;
            }
            if (handle.code == 6U)
            {
                return ownHandle + 1U;
            }
            if (handle.code == 8U)
            {
                return ownHandle - 1U;
            }
            if (handle.code == 10U)
            {
                return ownHandle + handle.value;
            }
            if (handle.code == 12U)
            {
                return ownHandle - handle.value;
            }
            throw std::runtime_error("DWG: unsupported relative handle code");
        }

        inline LayerProfile readLayerProfile(const Object& object)
        {
            BitInput data(object.payload);
            data.cursor = object.commonEnd;
            require(data.longValue() == 0U, "DWG: seed layer has reactors");
            require(data.text() == "0", "DWG: expected default seed layer");
            (void)data.bits(1U);
            (void)data.shortValue();
            (void)data.bits(1U);
            LayerProfile profile;
            profile.values = data.shortValue();
            (void)data.shortValue(); // indexed color
            require(data.cursor == object.handleStart, "DWG: unsupported seed layer data");
            const std::array<uint8_t, 6> absoluteCodes{4U, 3U, 5U, 5U, 5U, 5U};
            for (size_t i = 0U; i < 5U; ++i)
            {
                const auto reference = data.handle();
                profile.references.push_back({absoluteCodes[i], resolveHandle(object.handle, reference)});
            }
            if (object.payload.size() * 8U - data.cursor >= 8U)
            {
                const auto reference = data.handle();
                profile.references.push_back({absoluteCodes[5], resolveHandle(object.handle, reference)});
            }
            profile.control = profile.references[0].value;
            require(profile.references[0].code == 4U && profile.control != 0U,
                    "DWG: unsupported seed layer owner handle");
            return profile;
        }

        inline Bytes layerObject(uint64_t handle, const std::string& name, const LayerProfile& profile)
        {
            auto bits = nonEntityPrefix(0x33U, handle);
            bits.text(name);
            bits.bits(0U, 1U);
            bits.shortValue(0U); // no xref index
            bits.bits(0U, 1U);
            bits.shortValue(profile.values);
            bits.shortValue(7U); // default white
            bits.patchRaw(10U, bits.cursor, 4U);
            for (auto reference : profile.references)
            {
                bits.handle(reference);
            }
            return objectFrame(std::move(bits));
        }

        inline Bytes layerControlObject(const Object& original, const std::vector<uint64_t>& additional)
        {
            BitInput input(original.payload);
            input.cursor = original.commonEnd;
            require(input.longValue() == 0U, "DWG: seed layer table has reactors");
            const auto count = input.longValue();
            require(input.cursor == original.handleStart, "DWG: unsupported seed layer table data");
            const auto owner = input.handle();
            const auto dictionary = input.handle();
            std::vector<Handle> entries;
            for (uint32_t i = 0U; i < count; ++i)
            {
                entries.push_back(input.handle());
            }
            require(additional.size() <= UINT32_MAX - count, "DWG: layer table is too large");
            auto output = nonEntityPrefix(0x32U, original.handle);
            output.longValue(count + static_cast<uint32_t>(additional.size()));
            output.patchRaw(10U, output.cursor, 4U);
            output.handle(owner);
            output.handle(dictionary);
            for (auto entry : entries)
            {
                output.handle(entry);
            }
            for (auto entry : additional)
            {
                output.handle({2U, entry});
            }
            return objectFrame(std::move(output));
        }

        struct BitReplacement
        {
            size_t begin = 0U;
            size_t end = 0U;
            BitOutput replacement;
        };

        inline void skipUcs(BitInput& bits)
        {
            bits.doubles(9U);
            (void)bits.raw(8U);
            (void)bits.raw(8U);
            (void)bits.raw(8U);
            (void)bits.raw(8U);
            bits.doubles(10U);
            bits.handles(2U);
            bits.shorts(1U);
            bits.handles(1U);
            bits.doubles(18U);
        }

        inline Bytes headerVariables(std::span<const uint8_t> section, const Scene& scene, uint64_t nextHandle)
        {
            require(section.size() >= 38U, "DWG: header variables are truncated");
            const auto length = static_cast<size_t>(little(section, 16U, 4U));
            require(length + 38U == section.size(), "DWG: unsupported header variables framing");
            require(crc16(section.subspan(16U, 4U + length)) == little(section, 20U + length, 2U),
                    "DWG: header variables CRC mismatch");
            BitInput bits(section);
            bits.cursor = 160U;
            bits.doubles(4U);
            bits.texts(4U);
            bits.longs(2U);
            bits.handles(1U);
            (void)bits.bits(20U);
            bits.shorts(27U);
            bits.doubles(21U);
            bits.texts(1U);
            bits.longs(8U);
            bits.shorts(1U); // CECOLOR
            std::vector<BitReplacement> changes;
            auto replaceHandle = [&](uint64_t value) {
                const auto start = bits.cursor;
                (void)bits.handle();
                BitOutput replacement;
                replacement.handle({0U, value});
                changes.push_back({start, bits.cursor, std::move(replacement)});
            };
            replaceHandle(nextHandle);
            bits.handles(5U);
            bits.doubles(1U); // PSVPSCALE
            skipUcs(bits);    // paper space
            bits.doubles(3U); // model insertion base
            Point3d minimum{}, maximum{};
            if (!scene.faces.empty())
            {
                minimum = maximum = scene.faces.front().vertices.front();
                for (const auto& face : scene.faces)
                {
                    for (const auto& point : face.vertices)
                    {
                        minimum.x = std::min(minimum.x, point.x);
                        maximum.x = std::max(maximum.x, point.x);
                        minimum.y = std::min(minimum.y, point.y);
                        maximum.y = std::max(maximum.y, point.y);
                        minimum.z = std::min(minimum.z, point.z);
                        maximum.z = std::max(maximum.z, point.z);
                    }
                }
            }
            const auto extentsStart = bits.cursor;
            bits.doubles(6U);
            BitOutput extents;
            for (double value : {minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z})
            {
                extents.doubleValue(value);
            }
            changes.push_back({extentsStart, bits.cursor, std::move(extents)});
            (void)bits.raw(8U);
            (void)bits.raw(8U);
            (void)bits.raw(8U);
            (void)bits.raw(8U);
            bits.doubles(10U);
            bits.handles(2U);
            bits.shorts(1U);
            bits.handles(1U);
            bits.doubles(18U);
            bits.texts(2U);
            // R2000 dimension settings, from the documented field sequence.
            bits.doubles(9U);
            (void)bits.bits(6U);
            bits.shorts(3U);
            bits.doubles(9U);
            (void)bits.bits(1U);
            bits.shorts(1U);
            (void)bits.bits(4U);
            bits.shorts(14U); // colors then dimension shorts
            (void)bits.bits(2U);
            bits.shorts(4U);
            (void)bits.bits(1U);
            bits.shorts(1U);
            bits.handles(5U);
            bits.shorts(2U);
            bits.handles(13U); // tables, dictionaries, stacked annotation handles
            bits.shorts(2U);
            bits.texts(2U);
            bits.handles(3U);
            (void)bits.longValue(); // drawing flags
            const auto unitStart = bits.cursor;
            const auto oldUnits = bits.shortValue();
            require(oldUnits <= 24U && scene.insunits <= 24U, "DWG: invalid drawing unit code");
            BitOutput units;
            units.shortValue(scene.insunits);
            changes.push_back({unitStart, bits.cursor, std::move(units)});
            require(bits.cursor <= (20U + length) * 8U, "DWG: invalid header variable layout");
            BitOutput output;
            output.copy(section, 0U, 160U);
            size_t previous = 160U;
            for (const auto& change : changes)
            {
                output.copy(section, previous, change.begin);
                output.copy(change.replacement.bytes, 0U, change.replacement.cursor);
                previous = change.end;
            }
            output.copy(section, previous, (20U + length) * 8U);
            require(output.bytes.size() - 20U <= UINT32_MAX, "DWG: header variables exceed size limit");
            patchLittle(output.bytes, 16U, output.bytes.size() - 20U, 4U);
            appendLittle(output.bytes, crc16(std::span<const uint8_t>(output.bytes).subspan(16U)), 2U);
            output.bytes.insert(output.bytes.end(), section.end() - 16, section.end());
            return std::move(output.bytes);
        }

        struct Locator
        {
            uint8_t number = 0U;
            uint32_t address = 0U;
            uint32_t size = 0U;
            size_t recordPosition = 0U;
        };

        inline std::vector<Locator> locators(std::span<const uint8_t> bytes)
        {
            require(bytes.size() >= 25U && std::equal(bytes.begin(), bytes.begin() + 6, "AC1015"),
                    "DWG: writer requires an AC1015 seed");
            const auto count = static_cast<size_t>(little(bytes, 21U, 4U));
            require(count >= 3U && count <= 6U && 25U + count * 9U + 18U <= bytes.size(),
                    "DWG: unsupported seed section count");
            const std::array<uint16_t, 4> magic{0xA598U, 0x8101U, 0x3CC4U, 0x8461U};
            const auto crcPosition = 25U + count * 9U;
            require((crc16(bytes.first(crcPosition), 0U) ^ magic[count - 3U]) == little(bytes, crcPosition, 2U),
                    "DWG: seed file header CRC mismatch");
            std::vector<Locator> result;
            for (size_t i = 0U; i < count; ++i)
            {
                const auto at = 25U + i * 9U;
                Locator locator{bytes[at], static_cast<uint32_t>(little(bytes, at + 1U, 4U)),
                                static_cast<uint32_t>(little(bytes, at + 5U, 4U)), at};
                require(locator.address <= bytes.size() && locator.size <= bytes.size() - locator.address,
                        "DWG: seed section outside file");
                result.push_back(locator);
            }
            return result;
        }

        inline void validateLayerName(const std::string& name)
        {
            require(!name.empty() && name.size() <= 255U, "DWG: invalid layer name length");
            for (unsigned char value : name)
            {
                require(value >= 32U && value < 127U && std::string("<>/\\\":;?*|,=").find(value) == std::string::npos,
                        "DWG: layer name must contain valid ASCII characters");
            }
        }

        inline void updateAuxiliaryHeader(Bytes& result, const Locator& section, uint64_t nextHandle)
        {
            // The bounded R2000 auxiliary profile contains raw values, with no
            // CRC. The two raw day/millisecond pairs precede HANDSEED at byte 79
            // (ODA section 27). The generated empty defaults have no second
            // header; we explicitly reject a seed carrying that separate profile.
            require(section.size == 115U && section.address <= result.size() &&
                        section.size <= result.size() - section.address,
                    "DWG: unsupported auxiliary header profile");
            const auto at = static_cast<size_t>(section.address);
            require(result[at] == 0xFFU && result[at + 1U] == 0x77U && result[at + 2U] == 1U &&
                        little(result, at + 3U, 2U) == 23U,
                    "DWG: invalid R2000 auxiliary header signature");
            const auto value = nextHandle < 0x7FFFFFFFU ? nextHandle : 0xFFFFFFFFU;
            patchLittle(result, at + 79U, value, 4U);
            require(little(result, at + 79U, 4U) == value, "DWG: auxiliary HANDSEED patch failed");
        }
    } // namespace detail

    [[nodiscard]] inline Bytes makeR2000(const Scene& scene, std::span<const uint8_t> seed)
    {
        using namespace detail;
        auto sections = locators(seed);
        const Locator* mapSection = nullptr;
        const Locator* headerSection = nullptr;
        for (const auto& section : sections)
        {
            if (section.number == 0U)
            {
                headerSection = &section;
            }
            if (section.number == 2U)
            {
                mapSection = &section;
            }
        }
        require(mapSection && headerSection, "DWG: seed required sections missing");
        const auto originalHeaderEnd = static_cast<size_t>(headerSection->address) + headerSection->size;
        const auto originalMapAddress = static_cast<size_t>(mapSection->address);
        const auto originalMapEnd = static_cast<size_t>(mapSection->address) + mapSection->size;
        require(originalHeaderEnd <= mapSection->address && originalMapEnd <= seed.size() &&
                    little(seed, 13U, 4U) == 0U,
                "DWG: unsupported seed section ordering or preview");
        static constexpr std::array<uint8_t, 16> secondHeaderSentinel{
            0xD4U, 0x7BU, 0x21U, 0xCEU, 0x28U, 0x93U, 0x9FU, 0xBFU,
            0x53U, 0x24U, 0x40U, 0x09U, 0x12U, 0x3CU, 0xAAU, 0x01U,
        };
        require(std::search(seed.begin(), seed.end(), secondHeaderSentinel.begin(), secondHeaderSentinel.end()) ==
                    seed.end(),
                "DWG: seed second-header profile is unsupported");
        auto objects = parseMap(seed.subspan(mapSection->address, mapSection->size));
        require(!objects.empty(), "DWG: empty seed object map");
        Object modelSpace, defaultLayer, layerControl;
        uint64_t maximumHandle = objects.rbegin()->first;
        for (const auto& [handle, location] : objects)
        {
            require(location >= originalHeaderEnd && location < mapSection->address,
                    "DWG: seed object is outside the default-object region");
            const auto object = readObject(seed.first(mapSection->address), location);
            require(object.handle == handle, "DWG: seed object handle disagrees with map");
            if (object.type == 0x31U || object.type == 0x33U)
            {
                BitInput data(object.payload);
                data.cursor = object.commonEnd;
                (void)data.longValue();
                const auto name = data.text();
                if (object.type == 0x31U && (name == "*Model_Space" || name == "*MODEL_SPACE"))
                {
                    modelSpace = object;
                }
                if (object.type == 0x33U && name == "0")
                {
                    defaultLayer = object;
                }
            }
            if (object.type == 0x32U)
            {
                layerControl = object;
            }
            // Only BLOCK and ENDBLK entity objects may be present in the
            // empty template. This writer never silently merges seed geometry.
            const bool supportedNonEntity = (object.type >= 0x30U && object.type <= 0x49U) ||
                                            (object.type >= 0x4BU && object.type <= 0x4CU) ||
                                            (object.type >= 0x4FU && object.type <= 0x52U) ||
                                            (object.type >= 0x1F3U && object.type <= 0x1F6U) || object.type == 0x2AU;
            require(supportedNonEntity || object.type == 4U || object.type == 5U,
                    "DWG: seed contains geometry entities");
        }
        require(modelSpace.handle != 0U && defaultLayer.handle != 0U && layerControl.handle != 0U,
                "DWG: seed default tables missing");
        const auto layerProfile = readLayerProfile(defaultLayer);
        require(layerProfile.control == layerControl.handle, "DWG: seed layer table owner mismatch");
        std::unordered_set<std::string> layerNames;
        layerNames.reserve(scene.layers.size());
        for (size_t i = 0U; i < scene.layers.size(); ++i)
        {
            validateLayerName(scene.layers[i]);
            std::string key = scene.layers[i];
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return c >= 'a' && c <= 'z' ? c - 32 : c; });
            require(key != "0", "DWG: custom layer cannot duplicate layer 0");
            require(layerNames.emplace(std::move(key)).second, "DWG: duplicate custom layer name");
        }
        for (const auto& face : scene.faces)
        {
            require(face.color >= 1U && face.color <= 255U, "DWG: face color is not an ACI index");
            require(scene.layers.empty() ? face.layerIndex == 0U : face.layerIndex < scene.layers.size(),
                    "DWG: face layer index outside scene");
            for (const auto& point : face.vertices)
            {
                require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
                        "DWG: face has non-finite coordinates");
            }
        }
        require(scene.layers.size() <= UINT64_MAX - maximumHandle &&
                    scene.faces.size() < UINT64_MAX - maximumHandle - scene.layers.size(),
                "DWG: scene handle range exhausted");
        const auto nextHandle = maximumHandle + scene.layers.size() + scene.faces.size() + 1U;
        const auto header =
            headerVariables(seed.subspan(headerSection->address, headerSection->size), scene, nextHandle);
        // Serialize a single live header and map. Reusing the empty drawing's
        // table/object bytes is safe because their references use handles;
        // their physical addresses are adjusted in the new map below.
        Bytes result(seed.begin(), seed.begin() + headerSection->address);
        auto append = [&](const Bytes& bytes) -> uint32_t {
            require(result.size() <= UINT32_MAX && bytes.size() <= UINT32_MAX - result.size(),
                    "DWG: AC1015 file exceeds 4 GiB limit");
            const auto address = static_cast<uint32_t>(result.size());
            result.insert(result.end(), bytes.begin(), bytes.end());
            return address;
        };
        const auto headerAddress = append(header);
        const int64_t objectAddressDelta = static_cast<int64_t>(header.size()) - headerSection->size;
        const auto defaults = seed.subspan(originalHeaderEnd, mapSection->address - originalHeaderEnd);
        result.insert(result.end(), defaults.begin(), defaults.end());
        for (auto& [handle, location] : objects)
        {
            const auto adjusted = static_cast<int64_t>(location) + objectAddressDelta;
            require(adjusted >= 0 && static_cast<uint64_t>(adjusted) <= UINT32_MAX,
                    "DWG: relocated default-object address exceeds range");
            location = static_cast<uint32_t>(adjusted);
        }
        std::vector<uint64_t> layers;
        for (const auto& name : scene.layers)
        {
            const auto handle = ++maximumHandle;
            layers.push_back(handle);
            objects[handle] = append(layerObject(handle, name, layerProfile));
        }
        if (!layers.empty())
        {
            objects[layerControl.handle] = append(layerControlObject(layerControl, layers));
        }
        const uint64_t firstEntity = scene.faces.empty() ? 0U : maximumHandle + 1U;
        for (size_t i = 0U; i < scene.faces.size(); ++i)
        {
            const auto handle = ++maximumHandle;
            const auto previous = i == 0U ? 0U : handle - 1U;
            const auto next = i + 1U == scene.faces.size() ? 0U : handle + 1U;
            const auto layer = layers.empty() ? defaultLayer.handle : layers[scene.faces[i].layerIndex];
            objects[handle] = append(faceObject(scene.faces[i], handle, layer, previous, next));
        }
        objects[modelSpace.handle] =
            append(replaceBlockEntities(modelSpace, firstEntity, scene.faces.empty() ? 0U : maximumHandle));
        const auto map = encodeMap(objects);
        const auto mapAddress = append(map);
        const int64_t tailAddressDelta = static_cast<int64_t>(result.size()) - originalMapEnd;
        const auto tail = seed.subspan(originalMapEnd);
        require(result.size() <= UINT32_MAX && tail.size() <= UINT32_MAX - result.size(),
                "DWG: AC1015 file exceeds 4 GiB limit");
        result.insert(result.end(), tail.begin(), tail.end());
        for (auto& section : sections)
        {
            if (section.number == 0U)
            {
                section.address = headerAddress;
                section.size = static_cast<uint32_t>(header.size());
            }
            else if (section.number == 2U)
            {
                section.address = mapAddress;
                section.size = static_cast<uint32_t>(map.size());
            }
            else if (section.size != 0U)
            {
                const bool beforeMap = section.address >= originalHeaderEnd &&
                                       static_cast<size_t>(section.address) + section.size <= originalMapAddress;
                const bool afterMap = section.address >= originalMapEnd;
                require(beforeMap || afterMap, "DWG: seed sections overlap rewritten data");
                const auto address =
                    static_cast<int64_t>(section.address) + (beforeMap ? objectAddressDelta : tailAddressDelta);
                require(address >= 0 && static_cast<uint64_t>(address) <= UINT32_MAX,
                        "DWG: relocated section address exceeds range");
                section.address = static_cast<uint32_t>(address);
            }
            patchLittle(result, section.recordPosition + 1U, section.address, 4U);
            patchLittle(result, section.recordPosition + 5U, section.size, 4U);
            if (section.number == 5U && section.size != 0U)
            {
                updateAuxiliaryHeader(result, section, nextHandle);
            }
        }
        const auto crcPosition = 25U + sections.size() * 9U;
        const std::array<uint16_t, 4> magic{0xA598U, 0x8101U, 0x3CC4U, 0x8461U};
        patchLittle(result, crcPosition,
                    crc16(std::span<const uint8_t>(result).first(crcPosition), 0U) ^ magic[sections.size() - 3U], 2U);
        (void)locators(result);
        require(parseMap(std::span<const uint8_t>(result).subspan(mapAddress, map.size())) == objects,
                "DWG: generated object map failed validation");
        for (const auto& [handle, address] : objects)
        {
            require(readObject(result, address).handle == handle, "DWG: generated object failed validation");
        }
        return result;
    }

    inline void writeR2000(const std::filesystem::path& target, const Scene& scene, std::span<const uint8_t> seed)
    {
        const auto bytes = makeR2000(scene, seed);
        ciff::conversion::AtomicFile output(target);
        output.write(bytes);
        output.publish();
    }
} // namespace ciff::dwg::write

namespace ciff::dwg
{
    struct Color
    {
        uint8_t r = 128, g = 128, b = 128, a = 255;
    };
    [[nodiscard]] inline Color indexedColor(const uint16_t index)
    {
        // Classic ACI hue/shade palette. ACI 7 and some greys are displayed
        // differently by CAD consumers depending on their background settings.
        constexpr auto basic = std::array<Color, 10>{
            Color{0, 0, 0, 255},       Color{255, 0, 0, 255},    Color{255, 255, 0, 255}, Color{0, 255, 0, 255},
            Color{0, 255, 255, 255},   Color{0, 0, 255, 255},    Color{255, 0, 255, 255}, Color{255, 255, 255, 255},
            Color{128, 128, 128, 255}, Color{192, 192, 192, 255}};
        if (index < basic.size())
        {
            return basic[index];
        }
        if (index >= 250U && index <= 255U)
        {
            constexpr auto grey = std::array<uint8_t, 6>{51, 80, 105, 130, 190, 255};
            const auto value = grey[index - 250U];
            return {value, value, value, 255};
        }
        if (index > 255U)
        {
            throw std::invalid_argument("DWG indexed color is outside the ACI palette");
        }
        constexpr auto brightness = std::array<unsigned, 5>{255, 165, 127, 76, 38};
        const auto hue = static_cast<unsigned>((index - 10U) / 10U);
        const auto fraction = hue % 4U;
        const auto sector = hue / 4U;
        const auto intensity = brightness[(index % 10U) / 2U];
        const auto high = intensity;
        const auto rising = fraction * intensity / 4U;
        const auto falling = (4U - fraction) * intensity / 4U;
        auto channels = std::array<unsigned, 3>{};
        switch (sector)
        {
            case 0:
                channels = {high, rising, 0U};
                break;
            case 1:
                channels = {falling, high, 0U};
                break;
            case 2:
                channels = {0U, high, rising};
                break;
            case 3:
                channels = {0U, falling, high};
                break;
            case 4:
                channels = {rising, 0U, high};
                break;
            case 5:
                channels = {high, 0U, falling};
                break;
        }
        if ((index % 2U) != 0U)
        {
            for (auto& channel : channels)
            {
                channel = (channel + intensity) / 2U;
            }
        }
        return {static_cast<uint8_t>(channels[0]), static_cast<uint8_t>(channels[1]), static_cast<uint8_t>(channels[2]),
                255};
    }

    [[nodiscard]] inline uint16_t nearestColor(const Color color)
    {
        auto closest = uint16_t{7U};
        auto distance = (std::numeric_limits<unsigned>::max)();
        for (uint16_t index = 1U; index <= 255U; ++index)
        {
            const auto candidate = indexedColor(index);
            const auto red = static_cast<int>(color.r) - candidate.r;
            const auto green = static_cast<int>(color.g) - candidate.g;
            const auto blue = static_cast<int>(color.b) - candidate.b;
            const auto current = static_cast<unsigned>(red * red + green * green + blue * blue);
            if (current < distance)
            {
                distance = current;
                closest = index;
            }
        }
        return closest;
    }

    [[nodiscard]] inline std::string layerName(const size_t index, const std::string_view name)
    {
        auto result = std::string{"CIFF_"} + std::to_string(index) + "_";
        for (const auto character : name)
        {
            if (result.size() >= 255U)
            {
                break;
            }
            const auto value = static_cast<unsigned char>(character);
            result += value >= 32U && value < 127U &&
                              std::string_view{"<>/\\\":;?*|,="}.find(character) == std::string_view::npos
                          ? character
                          : '_';
        }
        return result;
    }

    struct ColorMapping
    {
        uint16_t map(Color color)
        {
            const auto key = (uint32_t{color.r} << 16U) | (uint32_t{color.g} << 8U) | color.b;
            const auto [entry, inserted] = colors.try_emplace(key, 0U);
            if (inserted)
                entry->second = nearestColor(color);
            const auto reduced = indexedColor(entry->second);
            approximated |= color.r != reduced.r || color.g != reduced.g || color.b != reduced.b;
            transparent |= color.a != 255U;
            return entry->second;
        }
        void report() const
        {
            if (approximated)
                std::cerr << "DWG 2000 output approximates RGB colors with the ACI palette.\n";
            if (transparent)
                std::cerr << "DWG 2000 face output does not preserve transparency.\n";
        }
        bool approximated = false, transparent = false;
        std::unordered_map<uint32_t, uint16_t> colors;
    };

    // CIFF placement fields and analytic frames are doubles. Keep them as
    // doubles for DWG instead of using the existing viewer's float matrix.
    struct Placement
    {
        tess::Frame frame;
        tess::V3 translation;
    };

    [[nodiscard]] inline Placement placement(const Box& primitive)
    {
        auto frame = tess::buildFrame(tess::toV3(primitive.normal), primitive.angle);
        if (primitive.delta.x < 0.0)
            frame.x = frame.x * -1.0;
        if (primitive.delta.y < 0.0)
            frame.y = frame.y * -1.0;
        if (primitive.delta.z < 0.0)
            frame.z = frame.z * -1.0;
        return {frame, tess::toV3(primitive.center)};
    }

    [[nodiscard]] inline Placement placement(const Cylinder& primitive)
    {
        const auto a = tess::toV3(primitive.centerA);
        const auto b = tess::toV3(primitive.centerB);
        return {tess::buildFrame(b - a, 0.0), (a + b) * 0.5};
    }

    [[nodiscard]] inline Placement placement(const Sphere& primitive)
    {
        return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, tess::toV3(primitive.center)};
    }

    [[nodiscard]] inline Placement placement(const CircularTorus& primitive)
    {
        return {tess::buildFrame(tess::toV3(primitive.normal), primitive.angle), tess::toV3(primitive.center)};
    }

    [[nodiscard]] inline Placement placement(const SphericalDish& primitive)
    {
        return {tess::buildFrame(tess::toV3(primitive.normal), 0.0), tess::toV3(primitive.center)};
    }

    [[nodiscard]] inline Placement placement(const GeneralCylinder& primitive)
    {
        const auto a = tess::toV3(primitive.centerA);
        const auto b = tess::toV3(primitive.centerB);
        return {tess::buildFrame(b - a, primitive.angle), (a + b) * 0.5};
    }

    template <class Primitive>
    [[nodiscard]] inline Mesh worldMesh(const Primitive& primitive)
    {
        auto mesh = ciff::TessellateCanonical(primitive);
        const auto transform = placement(primitive);
        const auto& frame = transform.frame;
        for (auto& point : mesh.vertices)
        {
            const auto world = frame.x * point.x + frame.y * point.y + frame.z * point.z + transform.translation;
            point = {world.x, world.y, world.z};
        }
        if (tess::dot(frame.x, tess::cross(frame.y, frame.z)) < 0.0)
            for (size_t index = 0; index + 2U < mesh.indices.size(); index += 3U)
                std::swap(mesh.indices[index + 1U], mesh.indices[index + 2U]);
        return mesh;
    }

    [[nodiscard]] inline Mesh surfaceMesh(const Read& data, const size_t geometryIndex)
    {
        const auto& geometry = data.getGeometry(geometryIndex);
        auto mesh = Mesh{};
        switch (geometry.primitive)
        {
            case Type::Mesh:
                mesh = data.getMesh(geometry.mesh);
                break;
            case Type::Box:
                mesh = worldMesh(data.boxes.at(geometry.primitiveIndex));
                break;
            case Type::Cylinder:
                mesh = worldMesh(data.cylinders.at(geometry.primitiveIndex));
                break;
            case Type::CircularTorus:
                mesh = worldMesh(data.circularToruses.at(geometry.primitiveIndex));
                break;
            case Type::Sphere:
                mesh = worldMesh(data.spheres.at(geometry.primitiveIndex));
                break;
            case Type::SphericalDish:
                mesh = worldMesh(data.sphericalDishes.at(geometry.primitiveIndex));
                break;
            case Type::GeneralCylinder:
                mesh = worldMesh(data.generalCylinders.at(geometry.primitiveIndex));
                break;
            default:
                throw std::runtime_error("CIFF: cannot tessellate unsupported geometry type for DWG");
        }
        mesh.color = geometry.color;
        return mesh;
    }

    [[nodiscard]] inline write::Scene makeScene(ciff::Read& data)
    {
        if (data.nodes.empty())
            throw std::runtime_error("CIFF scene has no nodes");
        auto scene = write::Scene{};
        scene.insunits = 0U; // The CIFF reader carries no source unit declaration.
        scene.layers.reserve(data.nodes.size());
        ColorMapping mapping;
        struct Stop
        {
            ~Stop()
            {
                ciff::bar::stop();
            }
        } stop;
        bar::start("Parse, tess and write DWG faces", data.nodes.size());
        for (size_t nodeIndex = 0; nodeIndex < data.nodes.size(); ++nodeIndex)
        {
            bar::step(nodeIndex);
            const auto& node = data.nodes[nodeIndex];
            if (nodeIndex == 0U ? node.parentIndex != 0U : node.parentIndex >= nodeIndex)
                throw std::runtime_error("CIFF node parent is not in hierarchy order");
            scene.layers.push_back(layerName(nodeIndex, node.name));
            for (const auto geometryIndex : node.geometries)
            {
                const auto mesh = surfaceMesh(data, geometryIndex);
                if (mesh.indices.size() % 3U != 0U)
                    throw std::runtime_error("CIFF mesh has incomplete triangles");
                if (!mesh.vertices.empty() && mesh.indices.empty())
                    throw std::runtime_error("CIFF point geometry cannot be represented by the DWG surface exporter");
                if (!mesh.colors.empty() && !mesh.hasColors())
                    throw std::runtime_error("CIFF vertex color count is invalid");
                const auto base = data.colors.at(mesh.color);
                for (size_t triangle = 0; triangle < mesh.indices.size() / 3U; ++triangle)
                {
                    auto face = write::Face{.layerIndex = nodeIndex};
                    for (size_t corner = 0; corner < 3U; ++corner)
                    {
                        const auto& point = mesh.vertices.at(mesh.indices[triangle * 3U + corner]);
                        face.vertices[corner] = {point.x, point.y, point.z};
                    }
                    auto color = Color{base.r, base.g, base.b, base.a};
                    if (mesh.hasColors())
                    {
                        const auto vertex = mesh.colors.at(mesh.indices[triangle * 3U]);
                        const auto modulate = [](uint8_t base, float channel) {
                            if (!std::isfinite(channel))
                                throw std::runtime_error("CIFF vertex color is not finite");
                            const auto value = static_cast<uint8_t>(std::clamp(channel * 255.0F, 0.0F, 255.0F));
                            return static_cast<uint8_t>((static_cast<uint16_t>(base) * value + 127U) / 255U);
                        };
                        color = {modulate(base.r, vertex.r), modulate(base.g, vertex.g), modulate(base.b, vertex.b),
                                 modulate(base.a, vertex.a)};
                    }
                    face.color = mapping.map(color);
                    scene.faces.push_back(face);
                }
            }
        }
        mapping.report();
        if (data.omittedPointGeometryCount)
            std::cerr << "DWG surface output omitted " << data.omittedPointGeometryCount
                      << " CIFF point geometry record(s).\n";
        return scene;
    }
    inline bool convert(ciff::Read& data)
    {
        try
        {
            if (cmd::async)
                throw std::invalid_argument("Async conversion is not supported for DWG output");
            const auto target = std::filesystem::path(data.target_cad);
            auto extension = target.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (target.empty() || target.filename().empty() || extension != ".dwg")
                throw std::invalid_argument("DWG target path must name a .dwg file");
            const auto scene = makeScene(data);
            const auto bytes = write::makeR2000(scene, drawingDefaults());
            if (WriteBuffer::enabled)
            {
                ciff::conversion::AtomicFile output(target);
                output.write(bytes);
                output.publish();
            }
            return true;
        }
        catch (const std::exception& error)
        {
            std::cerr << error.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Unknown error occurred while writing DWG." << std::endl;
        }
        return false;
    }
} // namespace ciff::dwg
