#pragma once

// ConvertToScene - public API of CogniteCiffLib.
//
// Reads a Cognite CIFF file from disk and fills a renderer-agnostic
// scene::SceneData. Intended to be linked into 3DViewer (and any other
// in-process consumer) via CogniteCiffLib.lib.
//
// IMPORTANT: scene::SceneData is intentionally forward-declared here so
// that 3DViewer can keep its own copy of SceneData.h without an ODR clash.
// The copies of SceneData.h must remain struct-identical.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace scene { struct SceneData; }

namespace cifflib
{
    struct ConvertToSceneResult
    {
        bool         success = false;
        std::wstring message;
    };

    using ConvertProgressCallback = std::function<bool(std::size_t total, std::size_t current)>;

    ConvertToSceneResult ConvertToScene(
        const std::filesystem::path&    ciffPath,
        scene::SceneData&               out,
        const ConvertProgressCallback&  progressCallback = {});

} // namespace cifflib
