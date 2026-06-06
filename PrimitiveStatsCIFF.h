#pragma once

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "PrimitivesCIFF.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ciff::primitive_stats
{
	inline constexpr bool enablePrimitiveStats = true;

	struct TypeStats
	{
		uint64_t instances = 0;
		std::unordered_set<uint64_t> forms;
	};

	struct Stats
	{
		std::array<TypeStats, static_cast<size_t>(Type::Size)> byType;

		void Record(const Geometry& geometry, const uint64_t hash)
		{
			if (!enablePrimitiveStats || hash == 0)
				return;

			const auto index = static_cast<size_t>(geometry.primitive);
			if (index >= byType.size())
				return;

			auto& stats = byType[index];
			stats.instances++;
			stats.forms.insert(hash);
		}

		[[nodiscard]] std::string Format(const std::string& source) const
		{
			if (!enablePrimitiveStats)
				return {};

			std::ostringstream out;
			out << "\nCIFF primitive sharing stats";
			if (!source.empty())
				out << " (" << source << ")";
			out << "\n";
			out << std::left << std::setw(22) << "Type"
				<< std::right << std::setw(14) << "Instances"
				<< std::setw(14) << "Forms"
				<< std::setw(12) << "Ratio" << "\n";

			uint64_t totalInstances = 0;
			uint64_t totalForms = 0;

			for (size_t i = 0; i < byType.size(); ++i)
			{
				const auto& stats = byType[i];
				if (stats.instances == 0)
					continue;

				const auto forms = static_cast<uint64_t>(stats.forms.size());
				const auto ratio = forms == 0 ? 0.0 : static_cast<double>(stats.instances) / static_cast<double>(forms);

				totalInstances += stats.instances;
				totalForms += forms;

				out << std::left << std::setw(22) << to_string(static_cast<Type>(i))
					<< std::right << std::setw(14) << stats.instances
					<< std::setw(14) << forms
					<< std::setw(11) << std::fixed << std::setprecision(2) << ratio << "x\n";
			}

			const auto totalRatio = totalForms == 0 ? 0.0 : static_cast<double>(totalInstances) / static_cast<double>(totalForms);

			out << std::left << std::setw(22) << "Total"
				<< std::right << std::setw(14) << totalInstances
				<< std::setw(14) << totalForms
				<< std::setw(11) << std::fixed << std::setprecision(2) << totalRatio << "x\n\n";

			return out.str();
		}

		void Print(const std::string& source) const
		{
			const auto text = Format(source);
			if (text.empty())
				return;

			std::cout << text;

#if defined(_WIN32)
			OutputDebugStringA(text.c_str());
#endif
		}
	};
}
