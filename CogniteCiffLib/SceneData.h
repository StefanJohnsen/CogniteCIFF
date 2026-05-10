#pragma once

// SceneData.h
//
// Shared scene contract between converters (FBX/RVM/CIFF) and 3DViewer.
// Header-only. No DirectX, no Windows, no I/O. Pure data.
//
// NOTE: This is a copy of the same file that lives in 3DViewer/Scene/
// and in the other *Lib folders. The copies must stay in sync.

#include <cstdint>
#include <string>
#include <vector>

namespace scene
{
    enum class UpAxis : std::uint8_t { Y = 1, Z = 2 };

    enum class FrontAxis : std::uint8_t { X = 0, Y = 1, Z = 2, NegX = 3, NegY = 4, NegZ = 5 };

    constexpr std::uint32_t kInvalidIndex = UINT32_MAX;

    struct Mesh
    {
        std::vector<float>         positions;
        std::vector<float>         normals;
        std::vector<std::uint32_t> indices;
    };

    struct Instance
    {
        std::uint32_t meshIndex     = kInvalidIndex;
        std::uint32_t materialIndex = kInvalidIndex;
        std::uint32_t nodeIndex     = kInvalidIndex;
        float         transform[12] = { 1, 0, 0,  0, 1, 0,  0, 0, 1,  0, 0, 0 };
    };

    struct Material
    {
        std::uint8_t  r = 255;
        std::uint8_t  g = 255;
        std::uint8_t  b = 255;
        std::uint8_t  a = 255;
        std::uint32_t textureIndex = kInvalidIndex;
    };

    struct Texture
    {
        std::string               name;
        std::string               format;
        std::vector<std::uint8_t> data;
    };

    struct Node
    {
        std::int32_t  parent        = -1;
        std::uint32_t firstInstance = 0;
        std::uint32_t instanceCount = 0;
        std::string   name;
    };

    struct SceneData
    {
        UpAxis    upAxis             = UpAxis::Z;
        FrontAxis frontAxis          = FrontAxis::NegY;
        bool      mirrorXAxisInWorld = false;

        std::vector<Mesh>     meshes;
        std::vector<Instance> instances;
        std::vector<Material> materials;
        std::vector<Texture>  textures;
        std::vector<Node>     nodes;
        std::uint32_t         rootNode = 0;
    };

} // namespace scene
