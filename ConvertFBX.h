/*----------------------------------------------------------------
  ConvertFBX.h

  Binary Autodesk FBX 7.5 (7500) exporter for the CIFF scene format.
  Builds a minimal but standards-conformant binary FBX file from
  scratch (no FBX SDK).

  Layout written:
    FBXHeaderExtension
    GlobalSettings (Z-up coordinate system)
    Documents
    References
    Definitions (Model, Geometry, Material counts)
    Objects:
      - Geometry per ciff::Mesh
      - Model per ciff::Node
      - Material per ciff::rgb color
    Connections:
      - OO Model       -> Model 0 (root) or Model parent
      - OO Geometry    -> Model
      - OO Material    -> Model
    Takes (empty)

  Arrays are written uncompressed (Encoding=0) for simplicity.
  This produces larger files but loads cleanly in Autodesk SDK,
  Blender, Maya, 3ds Max, FBX Review, etc.
----------------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Convert.h"
#include "NormalsCIFF.h"
#include "ProcessCIFF.h"
#include "WriteBuffer.h"

namespace fbx
{
	using namespace std;

	// Uniquify object IDs across the whole file.
	struct IdGen
	{
		static int64_t Next()
		{
			static int64_t id = 1000000;
			return ++id;
		}
		static void Reset()
		{
			// nothing to do; ids are monotonically increasing across files
		}
	};

	// ---- low-level binary FBX writers ----
	struct Bin
	{
		static void U8(WriteBuffer& w, uint8_t v) { w.write(v); }
		static void U16(WriteBuffer& w, uint16_t v) { w.write(v); }
		static void U32(WriteBuffer& w, uint32_t v) { w.write(v); }
		static void U64(WriteBuffer& w, uint64_t v) { w.write(v); }
		static void I16(WriteBuffer& w, int16_t v) { w.write(v); }
		static void I32(WriteBuffer& w, int32_t v) { w.write(v); }
		static void I64(WriteBuffer& w, int64_t v) { w.write(v); }
		static void F32(WriteBuffer& w, float v) { w.write(v); }
		static void F64(WriteBuffer& w, double v) { w.write(v); }
		static void Bytes(WriteBuffer& w, const void* p, size_t n)
		{
			w.write(static_cast<const char*>(p), n);
		}
	};

	struct Property
	{
		// Scalar properties
		static void Y(WriteBuffer& w, int16_t v) { Bin::U8(w, 'Y'); Bin::I16(w, v); }
		static void C(WriteBuffer& w, bool v) { Bin::U8(w, 'C'); Bin::U8(w, v ? 1 : 0); }
		static void I(WriteBuffer& w, int32_t v) { Bin::U8(w, 'I'); Bin::I32(w, v); }
		static void F(WriteBuffer& w, float v) { Bin::U8(w, 'F'); Bin::F32(w, v); }
		static void D(WriteBuffer& w, double v) { Bin::U8(w, 'D'); Bin::F64(w, v); }
		static void L(WriteBuffer& w, int64_t v) { Bin::U8(w, 'L'); Bin::I64(w, v); }

		static void S(WriteBuffer& w, const string& s)
		{
			Bin::U8(w, 'S');
			Bin::U32(w, static_cast<uint32_t>(s.size()));
			if (!s.empty())
				Bin::Bytes(w, s.data(), s.size());
		}

		static void R(WriteBuffer& w, const void* data, size_t n)
		{
			Bin::U8(w, 'R');
			Bin::U32(w, static_cast<uint32_t>(n));
			if (n) Bin::Bytes(w, data, n);
		}

		// Array properties (uncompressed)
		template <typename T, char Code>
		static void Array(WriteBuffer& w, const T* data, size_t n)
		{
			Bin::U8(w, static_cast<uint8_t>(Code));
			Bin::U32(w, static_cast<uint32_t>(n));         // ArrayLength
			Bin::U32(w, 0);                                 // Encoding = raw
			Bin::U32(w, static_cast<uint32_t>(n * sizeof(T))); // CompressedLength = byte size
			if (n) Bin::Bytes(w, data, n * sizeof(T));
		}

		static void ArrayI32(WriteBuffer& w, const vector<int32_t>& v) { Array<int32_t, 'i'>(w, v.data(), v.size()); }
		static void ArrayF64(WriteBuffer& w, const vector<double>& v)  { Array<double,  'd'>(w, v.data(), v.size()); }
		static void ArrayF32(WriteBuffer& w, const vector<float>& v)   { Array<float,   'f'>(w, v.data(), v.size()); }
	};

	// One open node in the binary FBX node tree. RAII closes it via End().
	struct NodeScope
	{
		WriteBuffer* w = nullptr;
		size_t header = 0;     // position of EndOffset
		size_t propCountPos = 0;
		size_t propLenPos = 0;
		size_t propsBegin = 0;
		uint64_t propCount = 0;

		static NodeScope Begin(WriteBuffer& wb, const string& name)
		{
			NodeScope s;
			s.w = &wb;
			s.header = wb.tell();

			// EndOffset (uint64), PropertyCount (uint64), PropertyListLen (uint64), NameLen (uint8), Name
			Bin::U64(wb, 0);
			s.propCountPos = wb.tell();
			Bin::U64(wb, 0);
			s.propLenPos = wb.tell();
			Bin::U64(wb, 0);
			Bin::U8(wb, static_cast<uint8_t>(name.size()));
			if (!name.empty())
				Bin::Bytes(wb, name.data(), name.size());
			s.propsBegin = wb.tell();
			return s;
		}

		void IncProp() { ++propCount; }

		void EndProperties()
		{
			const auto here = w->tell();
			const auto propLen = static_cast<uint64_t>(here - propsBegin);
			w->overwriteAt(propCountPos, propCount);
			w->overwriteAt(propLenPos, propLen);
		}

		// Close the node. If hasChildren is true, a NULL terminator record
		// is written. In FBX 7.5+ (version >= 7500) the node header uses
		// three uint64 fields, so the NULL terminator is 25 zero bytes
		// (3*8 + 1). In FBX 7.4 and earlier it would be 13 bytes (3*4 + 1).
		// We always emit 7500, hence 25.
		void End(bool hasChildren)
		{
			if (hasChildren)
			{
				static const uint8_t nullRecord[25] = {};
				Bin::Bytes(*w, nullRecord, 25);
			}
			const auto end = w->tell();
			w->overwriteAt(header, static_cast<uint64_t>(end));
		}
	};

	// Convenience helpers to write a simple "Type: <value>" property.
	struct P
	{
		static void Add(NodeScope& s, int16_t v)        { Property::Y(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, bool v)           { Property::C(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, int32_t v)        { Property::I(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, float v)          { Property::F(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, double v)         { Property::D(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, int64_t v)        { Property::L(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, const string& v)  { Property::S(*s.w, v); s.IncProp(); }
		static void Add(NodeScope& s, const char* v)    { Property::S(*s.w, string(v ? v : "")); s.IncProp(); }
		static void AddArray(NodeScope& s, const vector<int32_t>& v) { Property::ArrayI32(*s.w, v); s.IncProp(); }
		static void AddArray(NodeScope& s, const vector<double>& v)  { Property::ArrayF64(*s.w, v); s.IncProp(); }
		static void AddArray(NodeScope& s, const vector<float>& v)   { Property::ArrayF32(*s.w, v); s.IncProp(); }
	};

	// Single-shot leaf: opens a node, writes properties, closes (no children).
	struct Leaf
	{
		template <typename Fn>
		static void Write(WriteBuffer& w, const string& name, Fn&& addProps)
		{
			auto s = NodeScope::Begin(w, name);
			addProps(s);
			s.EndProperties();
			s.End(false);
		}

		static void Empty(WriteBuffer& w, const string& name)
		{
			auto s = NodeScope::Begin(w, name);
			s.EndProperties();
			s.End(false);
		}
	};

	// FBX "P" property entry inside a Properties70 block.
	// P: "Name", "Type", "TypeFlags", "Flag", value(s)
	struct P70
	{
		static void Int(WriteBuffer& w, const string& name, int32_t v)
		{
			Leaf::Write(w, "P", [&](NodeScope& s) {
				P::Add(s, name);
				P::Add(s, string("int"));
				P::Add(s, string("Integer"));
				P::Add(s, string(""));
				P::Add(s, v);
			});
		}

		static void Enum(WriteBuffer& w, const string& name, int32_t v)
		{
			Leaf::Write(w, "P", [&](NodeScope& s) {
				P::Add(s, name);
				P::Add(s, string("enum"));
				P::Add(s, string(""));
				P::Add(s, string(""));
				P::Add(s, v);
			});
		}

		static void Number(WriteBuffer& w, const string& name, double v)
		{
			Leaf::Write(w, "P", [&](NodeScope& s) {
				P::Add(s, name);
				P::Add(s, string("double"));
				P::Add(s, string("Number"));
				P::Add(s, string(""));
				P::Add(s, v);
			});
		}

		static void Vec3(WriteBuffer& w, const string& name, const string& type,
		                 const string& subtype, double x, double y, double z)
		{
			Leaf::Write(w, "P", [&](NodeScope& s) {
				P::Add(s, name);
				P::Add(s, type);
				P::Add(s, subtype);
				P::Add(s, string(""));
				P::Add(s, x);
				P::Add(s, y);
				P::Add(s, z);
			});
		}

		static void Color(WriteBuffer& w, const string& name, double r, double g, double b)
		{
			Vec3(w, name, "Color", "", r, g, b);
		}

		static void Str(WriteBuffer& w, const string& name, const string& v)
		{
			Leaf::Write(w, "P", [&](NodeScope& s) {
				P::Add(s, name);
				P::Add(s, string("KString"));
				P::Add(s, string(""));
				P::Add(s, string(""));
				P::Add(s, v);
			});
		}
	};

	// ---- FBX section writers ----

	struct Header
	{
		static void Write(WriteBuffer& w)
		{
			static const char Magic[21] = "Kaydara FBX Binary  ";
			Bin::Bytes(w, Magic, 21);
			Bin::U8(w, 0x1A);
			Bin::U8(w, 0x00);
			Bin::U32(w, 7500); // FBX 2016/2017 binary
		}

		static void Footer(WriteBuffer& w)
		{
			// Standard "all zeros" 13-byte NULL terminator already follows the last node.
			// FBX file footer (160 bytes) - magic block matches typical SDK output.
			static const uint8_t footer[] = {
				// 16-byte signature padding
				0xFA, 0xBC, 0xAB, 0x09, 0xD0, 0xC8, 0xD4, 0x66, 0xB1, 0x76, 0xFB, 0x83, 0x1C, 0xF7, 0x26, 0x7E,
			};
			Bin::Bytes(w, footer, sizeof(footer));

			// 4 zero bytes (alignment padding) + version int32 + ~120 zero bytes.
			static const uint8_t zeros4[4] = {};
			Bin::Bytes(w, zeros4, 4);
			Bin::U32(w, 7500);
			static const uint8_t pad[120] = {};
			Bin::Bytes(w, pad, 120);

			// Trailing 16-byte block (FBX SDK signature)
			static const uint8_t trail[16] = {
				0xF8, 0x5A, 0x8C, 0x6A, 0xDE, 0xF5, 0xD9, 0x7E,
				0xEC, 0xE9, 0x0C, 0xE3, 0x75, 0x8F, 0x29, 0x0B,
			};
			Bin::Bytes(w, trail, 16);
		}
	};

	struct HeaderExtension
	{
		static void Write(WriteBuffer& w)
		{
			auto s = NodeScope::Begin(w, "FBXHeaderExtension");
			s.EndProperties();

			Leaf::Write(w, "FBXHeaderVersion", [](NodeScope& n) { P::Add(n, int32_t(1003)); });
			Leaf::Write(w, "FBXVersion",        [](NodeScope& n) { P::Add(n, int32_t(7500)); });
			Leaf::Write(w, "EncryptionType",    [](NodeScope& n) { P::Add(n, int32_t(0)); });

			// CreationTimeStamp
			{
				auto t = NodeScope::Begin(w, "CreationTimeStamp");
				t.EndProperties();
				Leaf::Write(w, "Version", [](NodeScope& n) { P::Add(n, int32_t(1000)); });

				time_t now = time(nullptr);
				tm local{};
#if defined(_WIN32)
				localtime_s(&local, &now);
#else
				localtime_r(&now, &local);
#endif
				Leaf::Write(w, "Year",        [&](NodeScope& n) { P::Add(n, int32_t(1900 + local.tm_year)); });
				Leaf::Write(w, "Month",       [&](NodeScope& n) { P::Add(n, int32_t(1 + local.tm_mon)); });
				Leaf::Write(w, "Day",         [&](NodeScope& n) { P::Add(n, int32_t(local.tm_mday)); });
				Leaf::Write(w, "Hour",        [&](NodeScope& n) { P::Add(n, int32_t(local.tm_hour)); });
				Leaf::Write(w, "Minute",      [&](NodeScope& n) { P::Add(n, int32_t(local.tm_min)); });
				Leaf::Write(w, "Second",      [&](NodeScope& n) { P::Add(n, int32_t(local.tm_sec)); });
				Leaf::Write(w, "Millisecond", [](NodeScope& n)  { P::Add(n, int32_t(0)); });

				t.End(true);
			}

			Leaf::Write(w, "Creator", [](NodeScope& n) { P::Add(n, string("Falcon Coding CogniteCIFF")); });

			s.End(true);
		}
	};

	struct GlobalSettings
	{
		static void Write(WriteBuffer& w)
		{
			auto s = NodeScope::Begin(w, "GlobalSettings");
			s.EndProperties();

			Leaf::Write(w, "Version", [](NodeScope& n) { P::Add(n, int32_t(1000)); });

			auto p70 = NodeScope::Begin(w, "Properties70");
			p70.EndProperties();

			P70::Int(w, "UpAxis", 2);          // Z
			P70::Int(w, "UpAxisSign", 1);
			P70::Int(w, "FrontAxis", 1);       // -Y
			P70::Int(w, "FrontAxisSign", -1);
			P70::Int(w, "CoordAxis", 0);       // X
			P70::Int(w, "CoordAxisSign", 1);
			P70::Int(w, "OriginalUpAxis", 2);
			P70::Int(w, "OriginalUpAxisSign", 1);
			P70::Number(w, "UnitScaleFactor", 1.0);
			P70::Number(w, "OriginalUnitScaleFactor", 1.0);

			p70.End(true);
			s.End(true);
		}
	};

	struct Documents
	{
		static void Write(WriteBuffer& w, int64_t docId)
		{
			auto s = NodeScope::Begin(w, "Documents");
			s.EndProperties();

			Leaf::Write(w, "Count", [](NodeScope& n) { P::Add(n, int32_t(1)); });

			auto d = NodeScope::Begin(w, "Document");
			P::Add(d, docId);
			P::Add(d, string("Scene"));
			P::Add(d, string("Scene"));
			d.EndProperties();

			auto p70 = NodeScope::Begin(w, "Properties70");
			p70.EndProperties();
			P70::Int(w, "ActiveAnimStackName", 0); // dummy
			p70.End(true);

			Leaf::Write(w, "RootNode", [](NodeScope& n) { P::Add(n, int64_t(0)); });

			d.End(true);
			s.End(true);
		}
	};

	struct References
	{
		static void Write(WriteBuffer& w)
		{
			auto s = NodeScope::Begin(w, "References");
			s.EndProperties();
			s.End(false);
		}
	};

	struct Definitions
	{
		static void Write(WriteBuffer& w, int32_t modelCount, int32_t geometryCount, int32_t materialCount)
		{
			auto s = NodeScope::Begin(w, "Definitions");
			s.EndProperties();

			Leaf::Write(w, "Version", [](NodeScope& n) { P::Add(n, int32_t(100)); });
			Leaf::Write(w, "Count",   [&](NodeScope& n) { P::Add(n, int32_t(3)); });

			auto emit = [&](const char* objType, int32_t count, const char* subType)
			{
				auto o = NodeScope::Begin(w, "ObjectType");
				P::Add(o, string(objType));
				o.EndProperties();
				Leaf::Write(w, "Count", [&](NodeScope& n) { P::Add(n, count); });

				if (subType)
				{
					auto pt = NodeScope::Begin(w, "PropertyTemplate");
					P::Add(pt, string(subType));
					pt.EndProperties();
					pt.End(false);
				}
				o.End(true);
			};

			emit("Model",    modelCount,    "FbxNode");
			emit("Geometry", geometryCount, "FbxMesh");
			emit("Material", materialCount, "FbxSurfacePhong");

			s.End(true);
		}
	};

	struct ObjectsSection
	{
		// For each ciff Geometry index we record the FBX Geometry id we emitted.
		// For each ciff Node index we record the FBX Model id we emitted.
		std::vector<int64_t> geometryIds;
		std::vector<int64_t> modelIds;
		std::vector<int64_t> materialIds;

		void WriteGeometries(WriteBuffer& w, ciff::Read& data)
		{
			geometryIds.assign(data.geometries.size(), 0);

			for (size_t gi = 0; gi < data.geometries.size(); ++gi)
			{
				const auto sourceMesh = ciff::TessellateGeometry(data, gi);
				const auto mesh = ciff::normal_processing::FinalizeMesh(sourceMesh);
				if (mesh.empty()) continue;

				const int64_t id = IdGen::Next();
				geometryIds[gi] = id;

				auto g = NodeScope::Begin(w, "Geometry");
				P::Add(g, id);
				P::Add(g, string("Geometry::") + std::to_string(gi));
				P::Add(g, string("Mesh"));
				g.EndProperties();

				Leaf::Write(w, "GeometryVersion", [](NodeScope& n) { P::Add(n, int32_t(124)); });

				// Vertices: flat double array of x,y,z
				vector<double> verts;
				verts.reserve(mesh.positions.size());
				for (const auto value : mesh.positions)
					verts.push_back(static_cast<double>(value));
				Leaf::Write(w, "Vertices", [&](NodeScope& n) { P::AddArray(n, verts); });

				// PolygonVertexIndex: int32 array, last index of each polygon is XOR'd with -1 (i.e. ~v).
				vector<int32_t> idx;
				idx.reserve(mesh.indices.size());
				for (uint32_t t = 0; t < mesh.triangles(); ++t)
				{
					const auto a = static_cast<int32_t>(mesh.indices[3 * t + 0]);
					const auto b = static_cast<int32_t>(mesh.indices[3 * t + 1]);
					const auto c = static_cast<int32_t>(mesh.indices[3 * t + 2]);
					idx.push_back(a);
					idx.push_back(b);
					idx.push_back(~c); // end-of-polygon marker
				}
				Leaf::Write(w, "PolygonVertexIndex", [&](NodeScope& n) { P::AddArray(n, idx); });

				// The finalized mesh has one normal per split vertex. Emit the
				// corresponding value for every polygon corner.
				{
					auto le = NodeScope::Begin(w, "LayerElementNormal");
					P::Add(le, int32_t(0));
					le.EndProperties();
					Leaf::Write(w, "Version", [](NodeScope& n) { P::Add(n, int32_t(101)); });
					Leaf::Write(w, "Name", [](NodeScope& n) { P::Add(n, string("")); });
					Leaf::Write(w, "MappingInformationType", [](NodeScope& n) {
						P::Add(n, string("ByPolygonVertex"));
					});
					Leaf::Write(w, "ReferenceInformationType", [](NodeScope& n) {
						P::Add(n, string("Direct"));
					});

					vector<double> normals;
					normals.reserve(mesh.indices.size() * 3);
					for (const auto index : mesh.indices)
					{
						const auto base = static_cast<size_t>(index) * 3;
						normals.push_back(mesh.normals[base]);
						normals.push_back(mesh.normals[base + 1]);
						normals.push_back(mesh.normals[base + 2]);
					}
					Leaf::Write(w, "Normals", [&](NodeScope& n) { P::AddArray(n, normals); });
					le.End(true);
				}

				// LayerElementMaterial (AllSame, mapping to material 0 of the model)
				{
					auto le = NodeScope::Begin(w, "LayerElementMaterial");
					P::Add(le, int32_t(0));
					le.EndProperties();
					Leaf::Write(w, "Version",            [](NodeScope& n) { P::Add(n, int32_t(101)); });
					Leaf::Write(w, "Name",               [](NodeScope& n) { P::Add(n, string("")); });
					Leaf::Write(w, "MappingInformationType",   [](NodeScope& n) { P::Add(n, string("AllSame")); });
					Leaf::Write(w, "ReferenceInformationType", [](NodeScope& n) { P::Add(n, string("IndexToDirect")); });
					Leaf::Write(w, "Materials", [](NodeScope& n) {
						vector<int32_t> v{ 0 };
						P::AddArray(n, v);
					});
					le.End(true);
				}

				// Layer (binds normal and material elements)
				{
					auto layer = NodeScope::Begin(w, "Layer");
					P::Add(layer, int32_t(0));
					layer.EndProperties();
					Leaf::Write(w, "Version", [](NodeScope& n) { P::Add(n, int32_t(100)); });
					{
						auto le = NodeScope::Begin(w, "LayerElement");
						le.EndProperties();
						Leaf::Write(w, "Type", [](NodeScope& n) { P::Add(n, string("LayerElementNormal")); });
						Leaf::Write(w, "TypedIndex", [](NodeScope& n) { P::Add(n, int32_t(0)); });
						le.End(true);
					}
					{
						auto le = NodeScope::Begin(w, "LayerElement");
						le.EndProperties();
						Leaf::Write(w, "Type",        [](NodeScope& n) { P::Add(n, string("LayerElementMaterial")); });
						Leaf::Write(w, "TypedIndex",  [](NodeScope& n) { P::Add(n, int32_t(0)); });
						le.End(true);
					}
					layer.End(true);
				}

				g.End(true);
			}
		}

		void WriteModels(WriteBuffer& w, ciff::Read& data)
		{
			modelIds.assign(data.nodes.size(), 0);

			for (size_t ni = 0; ni < data.nodes.size(); ++ni)
			{
				const auto& node = data.nodes[ni];
				const int64_t id = IdGen::Next();
				modelIds[ni] = id;

				auto m = NodeScope::Begin(w, "Model");
				P::Add(m, id);
				P::Add(m, string("Model::") + node.name);
				P::Add(m, string("Mesh"));
				m.EndProperties();

				Leaf::Write(w, "Version", [](NodeScope& n) { P::Add(n, int32_t(232)); });

				auto p70 = NodeScope::Begin(w, "Properties70");
				p70.EndProperties();
				P70::Vec3(w, "Lcl Translation", "Lcl Translation", "", 0.0, 0.0, 0.0);
				P70::Vec3(w, "Lcl Rotation",    "Lcl Rotation",    "", 0.0, 0.0, 0.0);
				P70::Vec3(w, "Lcl Scaling",     "Lcl Scaling",     "", 1.0, 1.0, 1.0);
				p70.End(true);

				Leaf::Write(w, "Shading", [](NodeScope& n) { P::Add(n, true); });
				Leaf::Write(w, "Culling", [](NodeScope& n) { P::Add(n, string("CullingOff")); });

				m.End(true);
			}
		}

		void WriteMaterials(WriteBuffer& w, ciff::Read& data)
		{
			materialIds.assign(data.colors.size(), 0);

			for (size_t mi = 0; mi < data.colors.size(); ++mi)
			{
				const auto& c = data.colors[mi];
				const int64_t id = IdGen::Next();
				materialIds[mi] = id;

				const double r = c.r / 255.0;
				const double g = c.g / 255.0;
				const double b = c.b / 255.0;
				const double opacity = c.a / 255.0;

				auto mat = NodeScope::Begin(w, "Material");
				P::Add(mat, id);
				P::Add(mat, string("Material::material_") + std::to_string(mi));
				P::Add(mat, string(""));
				mat.EndProperties();

				Leaf::Write(w, "Version",      [](NodeScope& n) { P::Add(n, int32_t(102)); });
				Leaf::Write(w, "ShadingModel", [](NodeScope& n) { P::Add(n, string("Phong")); });
				Leaf::Write(w, "MultiLayer",   [](NodeScope& n) { P::Add(n, int32_t(0)); });

				auto p70 = NodeScope::Begin(w, "Properties70");
				p70.EndProperties();
				P70::Color(w, "DiffuseColor", r, g, b);
				P70::Number(w, "Opacity", opacity);
				p70.End(true);

				mat.End(true);
			}
		}
	};

	struct Connections
	{
		static void OO(WriteBuffer& w, int64_t child, int64_t parent)
		{
			Leaf::Write(w, "C", [&](NodeScope& n) {
				P::Add(n, string("OO"));
				P::Add(n, child);
				P::Add(n, parent);
			});
		}

		static void Write(WriteBuffer& w, ciff::Read& data, const ObjectsSection& objs, int64_t /*docId*/)
		{
			auto s = NodeScope::Begin(w, "Connections");
			s.EndProperties();

			// Models -> parent Model (or Scene root = 0)
			for (size_t ni = 0; ni < data.nodes.size(); ++ni)
			{
				const auto childId = objs.modelIds[ni];
				if (childId == 0) continue;

				int64_t parentId = 0;
				if (ni > 0)
				{
					const auto parentIdx = data.nodes[ni].parentIndex;
					if (parentIdx < objs.modelIds.size())
						parentId = objs.modelIds[parentIdx];
				}
				OO(w, childId, parentId);
			}

			// Geometries -> Model
			for (size_t ni = 0; ni < data.nodes.size(); ++ni)
			{
				const auto modelId = objs.modelIds[ni];
				if (modelId == 0) continue;

				for (const auto gi : data.nodes[ni].geometries)
				{
					if (gi >= objs.geometryIds.size()) continue;
					const auto geomId = objs.geometryIds[gi];
					if (geomId != 0)
						OO(w, geomId, modelId);
				}
			}

			// Materials -> Model (the material referenced by node.color)
			for (size_t ni = 0; ni < data.nodes.size(); ++ni)
			{
				const auto modelId = objs.modelIds[ni];
				if (modelId == 0) continue;
				if (data.nodes[ni].geometries.empty()) continue;

				const auto colorIdx = data.nodes[ni].color;
				if (colorIdx < objs.materialIds.size())
				{
					const auto matId = objs.materialIds[colorIdx];
					if (matId != 0)
						OO(w, matId, modelId);
				}
			}

			s.End(true);
		}
	};

	struct Takes
	{
		static void Write(WriteBuffer& w)
		{
			auto s = NodeScope::Begin(w, "Takes");
			s.EndProperties();
			Leaf::Write(w, "Current", [](NodeScope& n) { P::Add(n, string("")); });
			s.End(true);
		}
	};

	struct Convert final : ciff::Convert
	{
		ObjectsSection objects;
		int64_t docId = 0;

		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			IdGen::Reset();
			docId = IdGen::Next();

			Header::Write(write);
			HeaderExtension::Write(write);
			GlobalSettings::Write(write);
			Documents::Write(write, docId);
			References::Write(write);

			Definitions::Write(write,
				static_cast<int32_t>(data.nodes.size()),
				static_cast<int32_t>(data.geometries.size()),
				static_cast<int32_t>(data.colors.size()));

			// Open Objects section. We will close it in WriteFooter (after streaming all geometries/models).
			objectsScope.emplace(NodeScope::Begin(write, "Objects"));
			objectsScope->EndProperties();

			// Emit all geometries and models up-front (we can do this because CIFF data is fully loaded).
			objects.WriteGeometries(write, data);
			objects.WriteModels(write, data);
			objects.WriteMaterials(write, data);
		}

		void WriteNode(const ciff::Node&) override
		{
			// All nodes are emitted up-front in WriteHeader via ObjectsSection.
		}

		void WriteGeometry(const ciff::Node&, size_t) override
		{
			// All geometries are emitted up-front in WriteHeader via ObjectsSection.
		}

		void WriteMaterial(bool) override
		{
			// All materials are emitted up-front in WriteHeader via ObjectsSection.
		}

		void WriteFooter() override
		{
			// Close Objects
			objectsScope->End(true);
			objectsScope.reset();

			Connections::Write(write, data, objects, docId);
			Takes::Write(write);

			// Final NULL record + binary footer
			static const uint8_t nullRecord[13] = {};
			Bin::Bytes(write, nullRecord, 13);
			Header::Footer(write);
		}

	private:
		std::optional<NodeScope> objectsScope;
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace fbxexport
