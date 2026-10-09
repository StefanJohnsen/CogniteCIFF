#pragma once

#include <sstream>
#include <string>

#include "Convert.h"
#include "Write.h"

namespace ciff::json
{
	using ciff::write;
	struct Convert final : ciff::Convert
	{
		bool first = true;

		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			const std::string head = "{\n  \"nodes\": [\n";
			json::write(write, head);
		}

		void WriteNode(const ciff::Node& node) override
		{
			std::ostringstream line;

			if (!first)
				line << ",\n";

			first = false;

			line << "    {\"name\": \"" << node.name
				<< "\", \"color\": " << node.color
				<< ", \"geometries\": " << node.geometries.size() << "}";

			const auto str = line.str();
			json::write(write, str);
		}

		void WriteGeometry(const ciff::Node&, size_t) override
		{
		}

		void WriteMaterial(bool) override
		{
		}

		void WriteFooter() override
		{
			const std::string tail = "\n  ]\n}\n";
			json::write(write, tail);
		}
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
}
