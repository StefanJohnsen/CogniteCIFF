#include "ConvertDWG.h"
#include "FileType.h"

#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void require(const bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    template <class Action>
    void rejects(Action action)
    {
        bool rejected = false;
        try
        {
            action();
        }
        catch (const std::exception&)
        {
            rejected = true;
        }
        require(rejected, "Malformed surface input was accepted");
    }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        require(argc == 2, "Expected test output directory");
        const auto folder = std::filesystem::path(argv[1]);
        std::filesystem::create_directories(folder);
        require(ciff::targetType("surface.dwg") == ciff::AutodeskDWG, "Missing DWG target dispatch");
        require(ciff::mode("surface.dwg", false) == "wb", "DWG output is not binary");
        auto data = ciff::Read("", (folder / "mirrored-red.dwg").string());
        data.colors = {{255, 255, 255, 255}};
        auto mesh = ciff::Mesh{};
        mesh.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh.indices = {0, 1, 2};
        mesh.colors = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
        const auto reflected = ciff::tess::Matrix3x4{-1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 20, 30};
        ciff::tess::transform(mesh, reflected);
        data.meshes.push_back(mesh);
        data.geometries.push_back(ciff::Geometry{.color = 0, .mesh = 0});
        data.nodes.push_back(ciff::Node{.name = "Root"});
        data.nodes.push_back(ciff::Node{.name = "Red, mirrored/triangle", .parentIndex = 0, .geometries = {0}});
        ciff::bar::idle(true);
        const auto scene = ciff::dwg::makeScene(data);
        require(scene.insunits == 0 && scene.faces.size() == 1 && scene.layers.size() == 2,
                "Unitless coordinates or hierarchy lost");
        const auto& face = scene.faces.front();
        require(face.color == 1 && face.layerIndex == 1, "Vertex color or layer lost");
        require(face.vertices[0].x == 10 && face.vertices[0].y == 20 && face.vertices[0].z == 30,
                "World coordinates changed during export");
        require(face.vertices[1].x == 10 && face.vertices[1].y == 21 && face.vertices[2].x == 9 &&
                    face.vertices[2].y == 20,
                "Reflected winding changed during export");
        require(scene.layers[1].find(',') == std::string::npos, "Forbidden layer character retained");
        require(ciff::dwg::convert(data), "Native DWG export failed");
        auto box = ciff::Box{};
        box.delta = {2, 4, 6};
        box.center = {100, 200, 300};
        box.normal = {0, 0, 1};
        data.boxes.push_back(box);
        data.geometries.push_back(ciff::Geometry{.color = 0, .primitive = ciff::Type::Box});
        data.nodes[1].geometries = {1};
        const auto boxes = ciff::dwg::makeScene(data);
        require(boxes.faces.size() == 12, "Parametric box was not tessellated");
        for (const auto& boxFace : boxes.faces)
            for (const auto point : boxFace.vertices)
                require(point.x >= 99 && point.x <= 101 && point.y >= 198 && point.y <= 202 && point.z >= 297 &&
                            point.z <= 303,
                        "Box world placement changed");
        // Large fractional analytic centers must not pass through float
        // viewer matrices. Also verify outward winding for signed dimensions.
        data.boxes[0].center = {100000000.125, 200000000.25, 300000000.5};
        data.boxes[0].delta.x = -2;
        const auto precise = ciff::dwg::makeScene(data);
        auto lower = ciff::Point{INFINITY, INFINITY, INFINITY};
        auto upper = ciff::Point{-INFINITY, -INFINITY, -INFINITY};
        for (const auto& boxFace : precise.faces)
        {
            for (const auto point : boxFace.vertices)
            {
                lower.x = std::min(lower.x, point.x);
                lower.y = std::min(lower.y, point.y);
                lower.z = std::min(lower.z, point.z);
                upper.x = std::max(upper.x, point.x);
                upper.y = std::max(upper.y, point.y);
                upper.z = std::max(upper.z, point.z);
            }
            const auto a = boxFace.vertices[0];
            const auto b = boxFace.vertices[1];
            const auto c = boxFace.vertices[2];
            const auto normal = ciff::tess::cross({b.x - a.x, b.y - a.y, b.z - a.z}, {c.x - a.x, c.y - a.y, c.z - a.z});
            const auto outward = ciff::tess::V3{a.x - data.boxes[0].center.x, a.y - data.boxes[0].center.y,
                                                a.z - data.boxes[0].center.z};
            require(ciff::tess::dot(normal, outward) > 0, "Analytic reflection lost outward winding");
        }
        require(lower.x == 99999999.125 && lower.y == 199999998.25 && lower.z == 299999997.5 &&
                    upper.x == 100000001.125 && upper.y == 200000002.25 && upper.z == 300000003.5,
                "Analytic double placement was rounded to float");
        ciff::dwg::write::writeR2000(folder / "high-offset-box.dwg", precise, ciff::dwg::drawingDefaults());
        data.nodes[1].geometries = {0};
        data.meshes[0].indices[2] = 10;
        rejects([&] { (void)ciff::dwg::makeScene(data); });
        data.meshes[0].indices = {0, 1, 2};
        data.meshes[0].vertices[0].x = std::numeric_limits<double>::infinity();
        require(!ciff::dwg::convert(data), "Nonfinite coordinates were exported");
        data.meshes[0].vertices[0].x = 10;
        data.meshes[0].indices.clear();
        rejects([&] { (void)ciff::dwg::makeScene(data); });
        data.nodes[1].geometries.clear();
        data.target_cad = (folder / "empty.dwg").string();
        require(ciff::dwg::convert(data), "Empty surface scene failed");
        const auto target = folder / "preserved.dwg";
        {
            std::ofstream output(target);
            output << "preserve";
        }
        data.target_cad = target.string();
        ciff::cmd::async = true;
        require(!ciff::dwg::convert(data), "Explicit async DWG export was accepted");
        ciff::cmd::async = false;
        std::ifstream input(target);
        std::string preserved;
        input >> preserved;
        require(preserved == "preserve", "Failed export replaced its target");
        std::cout << "CIFF DWG world placement, winding, vertex colors, units and rejection checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
