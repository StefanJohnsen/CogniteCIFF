#pragma once

#include <iomanip>
#include <iostream>
#include <string>

#include "CmdArgs.h"
#include "ReadCIFF.h"

namespace statistics
{
	inline std::string formatWithDots(const size_t num)
	{
		std::string numStr = std::to_string(num);
		int insertPosition = static_cast<int>(numStr.length()) - 3;

		while (insertPosition > 0)
		{
			numStr.insert(insertPosition, ".");
			insertPosition -= 3;
		}

		return numStr;
	}

	inline constexpr int nameWidth = 28;
	inline constexpr int countWidth = 15;

	inline void print(const std::string& name, const std::string& col)
	{
		std::cout << "   " << std::left << std::setw(nameWidth) << name
			<< " : " << std::right << std::setw(countWidth) << col
			<< "   " << std::endl;
	}

	inline void print(const std::string& name, const std::string& col1, const std::string& col2, const std::string& col3)
	{
		std::cout << "   " << std::left << std::setw(nameWidth) << name
			<< " : " << std::right << std::setw(countWidth) << col1
			<< " | " << std::right << std::setw(countWidth) << col2
			<< " | " << std::right << std::setw(countWidth) << col3
			<< "   " << std::endl;
	}

	inline void print(const std::string& name, const ciff::Primitive& primitive, ciff::Primitive& total)
	{
		const auto col1 = formatWithDots(primitive.count);
		auto col2 = formatWithDots(primitive.points);
		auto col3 = formatWithDots(primitive.indices / 3);

		if (col2 == "0")
			col2 = col1 == "0" ? "-" : "parametric";

		if (col3 == "0")
			col3 = col1 == "0" ? "-" : "parametric";

		print(name, col1, col2, col3);

		total.count += primitive.count;
		total.points += primitive.points;
		total.indices += primitive.indices;
	}

	inline void printSeparator()
	{
		std::cout << std::string(3 + nameWidth + (3 + countWidth) * 3 + 3, '-') << std::endl;
	}

	inline void print(const ciff::Read& data)
	{
		if (!cmd::statistics)
			return;

		std::cout << std::endl;

		printSeparator();
		print("Primitive Type", "Meshes", "Points", "Triangles");
		printSeparator();

		ciff::Primitive total;
		print("FacetGroup", data.MESH.FacetGroup, total);

		printSeparator();
		print("Primitives total", total, total);
		printSeparator();

		if (!data.nodes.empty())
			print("Total CIFF nodes parsed", formatWithDots(data.nodes.size() - 1));

		print("Total geometry links", formatWithDots(data.geometries.size()));
		print("Total meshes parsed", formatWithDots(data.meshes.size()));
		print("Total colors parsed", formatWithDots(data.colors.size()));
		printSeparator();
	}
} // namespace statistics
