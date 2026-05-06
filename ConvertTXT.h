#pragma once

#include <iomanip>
#include <sstream>
#include <string>

#include "Convert.h"

namespace text
{
	struct Convert final : ciff::Convert
	{
		size_t depth = 0;

		explicit Convert(ciff::Read& data) : ciff::Convert(data)
		{
		}

		void WriteHeader() override
		{
		}

		void WriteNode(const ciff::Node& node) override
		{
			std::ostringstream line;
			line << std::string(depth * 2, ' ') << node.name
				<< " [color=" << node.color
				<< ", geometries=" << node.geometries.size() << "]\n";

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
		}
	};

	inline bool convert(ciff::Read& data)
	{
		return Convert(data).run();
	}
} // namespace text
