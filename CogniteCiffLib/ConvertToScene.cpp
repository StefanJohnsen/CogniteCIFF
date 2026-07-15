#include "ConvertToScene.h"
#include "SceneData.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CmdBar.h"
#include "Convert.h"
#include "PrimitiveInstanceCIFF.h"
#include "PrimitiveStatsCIFF.h"
#include "PrimitivesCIFF.h"
#include "ReadCIFF.h"

namespace
{
    constexpr std::size_t kProgressCallbackNodeInterval = 4096;

    using Matrix3x4 = ciff::primitive_instance::Matrix3x4;

    struct SceneConvert final : ciff::Convert
    {
        SceneConvert(ciff::Read& data, scene::SceneData& out, const cifflib::ConvertProgressCallback& cb)
            : ciff::Convert(data), sd(out), callback(cb)
        {
        }

        bool SetFile() override
        {
            source_file = data.source_cad;
            target_file.clear();
            return true;
        }

        void RunConvert()
        {
            SetFile();
            convert();
        }

        void WriteHeader() override {}

        void WriteNode(const ciff::Node& node) override
        {
            CloseCurrentNode();

            scene::Node n;
            n.parent        = sd.nodes.empty() ? -1 : static_cast<int32_t>(node.parentIndex);
            n.firstInstance = static_cast<uint32_t>(sd.instances.size());
            n.instanceCount = 0;
            n.name          = node.name;
            sd.nodes.push_back(std::move(n));
        }

        void WriteGeometry(const ciff::Node&, const size_t geometryIndex) override
        {
            const auto& geom = data.geometries[geometryIndex];
            const auto material = static_cast<uint32_t>(geom.color);
            auto form = ciff::primitive_instance::Make(data, geom);

            const auto preHash = form.hash;
            if (preHash != 0)
            {
                if (const auto it = formIndexByHash.find(preHash); it != formIndexByHash.end())
                {
                    primitiveStats.Record(geom, preHash);
                    EmitInstance(it->second, material, form.transform);
                    return;
                }
            }

            auto localMesh = ciff::primitive_instance::Tessellate(data, geom, form);
            if (localMesh.empty())
                return;

            const auto shapeHash = form.hash;
            if (shapeHash == 0)
                return;

            const auto formIndex = AddOrFindForm(shapeHash, std::move(localMesh));
            primitiveStats.Record(geom, shapeHash);
            EmitInstance(formIndex, material, form.transform);
        }

        void WriteMaterial(bool) override {}

        void WriteFooter() override
        {
            CloseCurrentNode();
            EmitMaterials();
            primitiveStats.Print(source_file);
        }

    private:
        void CloseCurrentNode()
        {
            if (sd.nodes.empty())
                return;

            const auto idx = static_cast<uint32_t>(sd.nodes.size() - 1);
            auto& current = sd.nodes.back();
            const auto end = static_cast<uint32_t>(sd.instances.size());
            current.instanceCount = end - current.firstInstance;

            for (auto k = current.firstInstance; k < end; ++k)
                sd.instances[k].nodeIndex = idx;

            const auto total = data.nodes.size();
            const auto completed = sd.nodes.size();
            const bool progressCallbackDue = completed % kProgressCallbackNodeInterval == 0 || completed == total;
            if (callback && progressCallbackDue && !callback(total, completed))
                throw std::runtime_error("Loading was cancelled by the user.");
        }

        uint32_t AddOrFindForm(const uint64_t shapeHash, const ciff::Mesh& mesh)
        {
            if (const auto it = formIndexByHash.find(shapeHash); it != formIndexByHash.end())
                return it->second;

            auto sceneMesh = BuildSceneMesh(mesh);
            sceneMesh.indices = mesh.indices;
            return AddForm(shapeHash, std::move(sceneMesh));
        }

        uint32_t AddOrFindForm(const uint64_t shapeHash, ciff::Mesh&& mesh)
        {
            if (const auto it = formIndexByHash.find(shapeHash); it != formIndexByHash.end())
                return it->second;

            auto sceneMesh = BuildSceneMesh(mesh);
            sceneMesh.indices = std::move(mesh.indices);
            return AddForm(shapeHash, std::move(sceneMesh));
        }

        static scene::Mesh BuildSceneMesh(const ciff::Mesh& mesh)
        {
            scene::Mesh sceneMesh;
            sceneMesh.positions.resize(mesh.vertices.size() * 3ULL);
            for (size_t i = 0; i < mesh.vertices.size(); ++i)
            {
                const auto& v = mesh.vertices[i];
                const auto base = i * 3ULL;
                sceneMesh.positions[base + 0] = static_cast<float>(v.x);
                sceneMesh.positions[base + 1] = static_cast<float>(v.y);
                sceneMesh.positions[base + 2] = static_cast<float>(v.z);
            }

            return sceneMesh;
        }

        uint32_t AddForm(const uint64_t shapeHash, scene::Mesh&& sceneMesh)
        {
            const auto index = static_cast<uint32_t>(sd.meshes.size());
            sd.meshes.push_back(std::move(sceneMesh));
            formIndexByHash.emplace(shapeHash, index);
            return index;
        }

        void EmitInstance(const uint32_t formIndex, const uint32_t materialIndex, const Matrix3x4& tx)
        {
            scene::Instance inst;
            inst.meshIndex     = formIndex;
            inst.materialIndex = materialIndex;
            std::memcpy(inst.transform, tx.data(), sizeof(inst.transform));
            sd.instances.push_back(inst);
        }

        void EmitMaterials()
        {
            sd.materials.clear();
            sd.materials.reserve(data.colors.size());
            for (const auto& c : data.colors)
            {
                scene::Material m;
                m.r = c.r;
                m.g = c.g;
                m.b = c.b;
                m.a = c.a;
                sd.materials.push_back(m);
            }
        }

        scene::SceneData&                              sd;
        const cifflib::ConvertProgressCallback&        callback;
        std::unordered_map<uint64_t, uint32_t>         formIndexByHash;
        ciff::primitive_stats::Stats                   primitiveStats;
    };
}

namespace cifflib
{
    ConvertToSceneResult ConvertToScene(
        const std::filesystem::path&    ciffPath,
        scene::SceneData&               out,
        const ConvertProgressCallback&  progressCallback)
    {
        ConvertToSceneResult result;

        try
        {
            bar::progress_idle = true;

            ciff::Read data(ciffPath.string(), std::string{});
            data.load();

            out = scene::SceneData{};
            out.upAxis             = scene::UpAxis::Z;
            out.frontAxis          = scene::FrontAxis::Y;
            out.mirrorXAxisInWorld = true;
            out.rootNode           = 0;

            SceneConvert convert(data, out, progressCallback);
            convert.RunConvert();

            if (out.meshes.empty() || out.instances.empty())
            {
                result.message = L"The CIFF file produced no drawable geometry.";
                return result;
            }

            result.success = true;
            return result;
        }
        catch (const std::exception& ex)
        {
            const std::string what(ex.what());
            result.message.assign(what.begin(), what.end());
            ::OutputDebugStringW((L"[CogniteCiffLib] " + result.message + L"\n").c_str());
            return result;
        }
    }
}
