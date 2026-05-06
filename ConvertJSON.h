#pragma once

#include <sstream>
#include <string>

#include "Convert.h"

namespace json
{
	struct Convert final : ciff::Convert
	{
		bool first = true;

		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
			const std::string head = "{\n  \"nodes\": [\n";
			write.write(head.c_str(), head.size());
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
			write.write(str.c_str(), str.size());
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
			write.write(tail.c_str(), tail.size());
		}
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace json
