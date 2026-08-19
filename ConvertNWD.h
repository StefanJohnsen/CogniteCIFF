#pragma once

#include "Convert.h"
#include "MeshNormals.h"
#include "ProcessCIFF.h"
#include "WriteNWD.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace nwd
{
    struct Convert final : ciff::Convert
    {
        explicit Convert(ciff::Read& data) : ciff::Convert(data)
        {
        }
        bool run()
        {
            try
            {
                if (!SetFile())
                    return false;
                if (data.nodes.empty())
                    throw std::runtime_error("CIFF scene has no nodes");
                ciff::Convert::convert();
                return true;
            }
            catch (const std::exception& error)
            {
                std::cerr << error.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "Unknown error occurred." << std::endl;
            }
            return false;
        }
        bool SetFile() override
        {
            if (!target_file.empty())
                return false;
            const auto target = std::filesystem::path{data.target_cad};
            auto extension = target.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char v) { return static_cast<char>(std::tolower(v)); });
            if (target.empty() || target.filename().empty() || extension != ".nwd")
                throw std::invalid_argument("NWD target path must name an .nwd file");
            source_file = data.source_cad;
            target_file = data.target_cad;
            return true;
        }
        void WriteHeader() override
        {
            scene_ = {};
            scene_.nodes.reserve(data.nodes.size());
            scene_.meshes.reserve(data.geometries.size());
            scene_.instances.reserve(data.geometries.size());
        }
        void WriteNode(const ciff::Node& node) override
        {
            const auto noParent = (std::numeric_limits<size_t>::max)();
            scene_.nodes.push_back(nwd::write::Node{
                .name = node.name, .parent = node.parentIndex == nodeIndex ? noParent : node.parentIndex});
        }
        void WriteGeometry(const ciff::Node&, const size_t geometryIndex) override
        {
            const auto source = ciff::TessellateGeometry(data, geometryIndex);
            const auto mesh = ciff::normal_processing::FinalizeMeshNormals(source);
            if (mesh.empty())
                return;
            if (mesh.positions.size() % 3U != 0U || mesh.normals.size() != mesh.positions.size() ||
                mesh.indices.size() % 3U != 0U)
                throw std::runtime_error("CIFF final mesh has incomplete triangle geometry");
            auto output = nwd::write::Mesh{};
            const auto points = mesh.positions.size() / 3U;
            output.vertices.reserve(points);
            output.normals.reserve(points);
            for (size_t i = 0; i < points; ++i)
            {
                output.vertices.push_back(
                    {mesh.positions[i * 3U], mesh.positions[i * 3U + 1U], mesh.positions[i * 3U + 2U]});
                output.normals.push_back({mesh.normals[i * 3U], mesh.normals[i * 3U + 1U], mesh.normals[i * 3U + 2U]});
            }
            output.indices = mesh.indices;
            const auto meshIndex = scene_.meshes.size();
            scene_.meshes.push_back(std::move(output));
            const auto appearanceIndex = appearance(source.color);
            scene_.instances.push_back(nwd::write::Instance{.mesh = meshIndex,
                                                            .node = nodeIndex,
                                                            .appearance = appearanceIndex,
                                                            .transparent = data.colors.at(source.color).a < 255U});
        }
        void WriteMaterial(bool) override
        {
        }
        void WriteFooter() override
        {
            if (WriteBuffer::enabled)
                nwd::write::writeSemantic(std::filesystem::path{target_file}, scene_);
            else
                static_cast<void>(nwd::write::makeSemantic(scene_));
        }

      private:
        uint32_t appearance(size_t colorIndex)
        {
            if (const auto found = appearances_.find(colorIndex); found != appearances_.end())
                return found->second;
            const auto& c = data.colors.at(colorIndex);
            auto value = nwd::write::Appearance{};
            value.material.diffuse = {c.r / 255.0F, c.g / 255.0F, c.b / 255.0F};
            value.material.shininess = 0.2F;
            value.material.transparency = 1.0F - c.a / 255.0F;
            const auto index = static_cast<uint32_t>(scene_.appearances.size());
            scene_.appearances.push_back(std::move(value));
            appearances_.emplace(colorIndex, index);
            return index;
        }
        nwd::write::Scene scene_;
        std::unordered_map<size_t, uint32_t> appearances_;
    };
    inline bool convert(ciff::Read& data)
    {
        return Convert(data).run();
    }
} // namespace nwd
