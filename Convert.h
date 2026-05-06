#pragma once

#include <exception>
#include <iostream>
#include <string>

#include "CmdBar.h"
#include "ReadCIFF.h"
#include "WriteBuffer.h"

namespace ciff
{
	struct Convert
	{
		explicit Convert(Read& data) : data(data)
		{
		}

		virtual ~Convert() = default;

		bool run()
		{
			SetFile();

			try
			{
				convert();
				return true;
			}
			catch (const std::exception& e)
			{
				std::cerr << e.what() << std::endl;
			}
			catch (...)
			{
				std::cerr << "Unknown error occurred." << std::endl;
			}

			return false;
		}

	protected:
		void convert()
		{
			if (data.nodes.empty())
				return;

			bar::start("Parse, tess and write geometry", data.nodes.size());
			WriteHeader();

			for (nodeIndex = 0; nodeIndex < data.nodes.size(); ++nodeIndex)
			{
				bar::step(nodeIndex);
				const auto& node = data.nodes[nodeIndex];

				if (nodeIndex == 0)
					WriteHead(node);
				else
					WriteModel(node);

				for (const auto geometryIndex : node.geometries)
					WriteGeometry(node, geometryIndex);
			}

			WriteMaterial(true);
			WriteFooter();
			bar::stop();
		}

	public:
		virtual bool SetFile()
		{
			if (write.good())
				return false;

			write.set(data.target_cad);
			source_file = data.source_cad;
			target_file = data.target_cad;
			return true;
		}

		virtual void WriteHeader() = 0;

		virtual void WriteHead(const Node& node)
		{
			WriteNode(node);
		}

		virtual void WriteModel(const Node& node)
		{
			WriteNode(node);
		}

		virtual void WriteNode(const Node& node) = 0;
		virtual void WriteGeometry(const Node& node, size_t geometryIndex) = 0;
		virtual void WriteMaterial(bool header) = 0;
		virtual void WriteFooter() = 0;

		[[nodiscard]] std::vector<rgb> getColors() const
		{
			return data.colors;
		}

		std::string source_file;
		std::string target_file;
		Read& data;
		WriteBuffer write;
		size_t nodeIndex = 0;
	};
} // namespace ciff
