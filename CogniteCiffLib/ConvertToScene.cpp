#include "ConvertToScene.h"
#include "SceneData.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "CmdBar.h"
#include "Convert.h"
#include "PrimitivesCIFF.h"
#include "ProcessCIFF.h"
#include "ReadCIFF.h"
#include "ShapeCIFF.h"
#include "TessCIFF.h"

namespace
{
    using Matrix3x4 = ciff::shape::Matrix3x4;

    Matrix3x4 IdentityMatrix() noexcept
    {
        return Matrix3x4{
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, 0.0f,
        };
    }

    uint64_t PrimitiveHash(const ciff::Read& data, const ciff::Geometry& geom)
    {
        switch (geom.primitive)
        {
            case ciff::Type::Box:             return ciff::shape::hashOf(data.boxes[geom.primitiveIndex]);
            case ciff::Type::Cylinder:        return ciff::shape::hashOf(data.cylinders[geom.primitiveIndex]);
            case ciff::Type::CircularTorus:   return ciff::shape::hashOf(data.circularToruses[geom.primitiveIndex]);
            case ciff::Type::Sphere:          return ciff::shape::hashOf(data.spheres[geom.primitiveIndex]);
            case ciff::Type::SphericalDish:   return ciff::shape::hashOf(data.sphericalDishes[geom.primitiveIndex]);
            case ciff::Type::GeneralCylinder: return ciff::shape::hashOf(data.generalCylinders[geom.primitiveIndex]);
            default:                          return 0;
        }
    }

    Matrix3x4 PrimitiveTransform(const ciff::Read& data, const ciff::Geometry& geom)
    {
        switch (geom.primitive)
        {
            case ciff::Type::Box:             return ciff::shape::instanceTransform(data.boxes[geom.primitiveIndex]);
            case ciff::Type::Cylinder:        return ciff::shape::instanceTransform(data.cylinders[geom.primitiveIndex]);
            case ciff::Type::CircularTorus:   return ciff::shape::instanceTransform(data.circularToruses[geom.primitiveIndex]);
            case ciff::Type::Sphere:          return ciff::shape::instanceTransform(data.spheres[geom.primitiveIndex]);
            case ciff::Type::SphericalDish:   return ciff::shape::instanceTransform(data.sphericalDishes[geom.primitiveIndex]);
            case ciff::Type::GeneralCylinder: return ciff::shape::instanceTransform(data.generalCylinders[geom.primitiveIndex]);
            default:                          return IdentityMatrix();
        }
    }

    ciff::Mesh PrimitiveTessellate(const ciff::Read& data, const ciff::Geometry& geom)
    {
        switch (geom.primitive)
        {
            case ciff::Type::Box:             return ciff::tess::tessellate(data.boxes[geom.primitiveIndex]);
            case ciff::Type::Cylinder:        return ciff::tess::tessellate(data.cylinders[geom.primitiveIndex]);
            case ciff::Type::CircularTorus:   return ciff::tess::tessellate(data.circularToruses[geom.primitiveIndex]);
            case ciff::Type::Sphere:          return ciff::tess::tessellate(data.spheres[geom.primitiveIndex]);
            case ciff::Type::SphericalDish:   return ciff::tess::tessellate(data.sphericalDishes[geom.primitiveIndex]);
            case ciff::Type::GeneralCylinder: return ciff::tess::tessellate(data.generalCylinders[geom.primitiveIndex]);
            default:                          return data.getMesh(geom.mesh);
        }
    }

    std::vector<float> ComputeNormals(const ciff::Mesh& mesh)
    {
        const auto n = static_cast<std::size_t>(mesh.points());
        std::vector<float> normals(n * 3ULL, 0.0f);

        for (uint32_t t = 0; t < mesh.triangles(); ++t)
        {
            const auto i0 = mesh.indices[3 * t + 0];
            const auto i1 = mesh.indices[3 * t + 1];
            const auto i2 = mesh.indices[3 * t + 2];

            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
                continue;

            const auto& a = mesh.vertices[i0];
            const auto& b = mesh.vertices[i1];
            const auto& c = mesh.vertices[i2];

            const float ux = static_cast<float>(b.x - a.x);
            const float uy = static_cast<float>(b.y - a.y);
            const float uz = static_cast<float>(b.z - a.z);
            const float vx = static_cast<float>(c.x - a.x);
            const float vy = static_cast<float>(c.y - a.y);
            const float vz = static_cast<float>(c.z - a.z);

            const float nx = uy * vz - uz * vy;
            const float ny = uz * vx - ux * vz;
            const float nz = ux * vy - uy * vx;

            normals[3 * i0 + 0] += nx; normals[3 * i0 + 1] += ny; normals[3 * i0 + 2] += nz;
            normals[3 * i1 + 0] += nx; normals[3 * i1 + 1] += ny; normals[3 * i1 + 2] += nz;
            normals[3 * i2 + 0] += nx; normals[3 * i2 + 1] += ny; normals[3 * i2 + 2] += nz;
        }

        for (std::size_t i = 0; i + 2 < normals.size(); i += 3)
        {
            const float x = normals[i + 0];
            const float y = normals[i + 1];
            const float z = normals[i + 2];
            const float len = std::sqrt(x * x + y * y + z * z);
            if (len > 1e-12f)
            {
                normals[i + 0] = x / len;
                normals[i + 1] = y / len;
                normals[i + 2] = z / len;
            }
            else
            {
                normals[i + 0] = 0.0f;
                normals[i + 1] = 0.0f;
                normals[i + 2] = 1.0f;
            }
        }

        return normals;
    }

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

            const auto total = data.nodes.size();
            const auto current = sd.nodes.size();
            if (callback && !callback(total, current))
                cancelled = true;
        }

        void WriteGeometry(const ciff::Node& node, const size_t geometryIndex) override
        {
            const auto& geom = data.geometries[geometryIndex];
            const auto material = static_cast<uint32_t>(geom.color);

            const auto preHash = PrimitiveHash(data, geom);
            const auto tx = PrimitiveTransform(data, geom);

            if (preHash != 0)
            {
                if (const auto it = formIndexByHash.find(preHash); it != formIndexByHash.end())
                {
                    EmitInstance(it->second, material, tx);
                    return;
                }
            }

            auto localMesh = PrimitiveTessellate(data, geom);
            if (localMesh.empty())
                return;

            const auto shapeHash = preHash != 0 ? preHash : ciff::shape::hashOf(localMesh);
            if (shapeHash == 0)
                return;

            const auto formIndex = AddOrFindForm(shapeHash, localMesh);
            EmitInstance(formIndex, material, tx);
        }

        void WriteMaterial(bool) override {}

        void WriteFooter() override
        {
            CloseCurrentNode();
            EmitMaterials();

            const auto total = data.nodes.size();
            if (callback)
                (void)callback(total, total);
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
        }

        uint32_t AddOrFindForm(const uint64_t shapeHash, const ciff::Mesh& mesh)
        {
            if (const auto it = formIndexByHash.find(shapeHash); it != formIndexByHash.end())
                return it->second;

            const auto index = static_cast<uint32_t>(sd.meshes.size());
            formIndexByHash.emplace(shapeHash, index);

            scene::Mesh sceneMesh;
            sceneMesh.positions.reserve(mesh.vertices.size() * 3ULL);
            for (const auto& v : mesh.vertices)
            {
                sceneMesh.positions.push_back(static_cast<float>(v.x));
                sceneMesh.positions.push_back(static_cast<float>(v.y));
                sceneMesh.positions.push_back(static_cast<float>(v.z));
            }

            sceneMesh.normals = ComputeNormals(mesh);
            sceneMesh.indices = mesh.indices;

            sd.meshes.push_back(std::move(sceneMesh));
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
        bool                                           cancelled = false;
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
