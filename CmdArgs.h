#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cmd
{
	enum class Msg
	{
		NoFileSpecified,
		NoFilesSpecified,
		AcceptsOnlyOneFile,
		AcceptsOnlySourceAndTargetFiles,
		TooManyArguments,
		UnknownFlag,
		FlagRequiresValue,
		InvalidFormatExtension,
		SourceNotFound,
		SourceInvalidExtension,
		TargetFileUnknownDirectory,
		TargetDirectoryDoesNotExist,
		TargetDirectoryIsNotADirectory,
		TargetInvalidExtension,
		SourceAndTargetAreSame
	};

	struct cmd_flag
	{
		cmd_flag(std::string flag, const bool on = false)
			: _flag(std::move(flag)), on(on), defaultOn(on)
		{
		}

		void clear()
		{
			on = defaultOn;
		}

		operator bool() const
		{
			return on;
		}

		operator std::string() const
		{
			return _flag;
		}

		void operator=(const bool set)
		{
			on = set;
		}

		const std::string& name() const
		{
			return _flag;
		}

	private:
		const std::string _flag;
		bool on = false;
		bool defaultOn = false;
	};

	inline std::string text_version = "CogniteCIFFConverter  version: 0.1.0";

	inline const std::vector<std::string> source_ext = { "ciff" };
	inline const std::vector<std::string> target_ext = { "ciff", "fbx", "rvm", "3d", "gltf", "obj", "json", "dat", "txt", "nwd" };

	inline cmd_flag async{ "async" };
	inline cmd_flag help{ "help" };
	inline cmd_flag version{ "version" };
	inline cmd_flag bar{ "bar" };
	inline cmd_flag statistics{ "statistics" };
	inline cmd_flag speedtest{ "speedtest" };
	inline const std::string format_flag = "format";

	inline const std::vector<cmd_flag*> cmd_flags = { &async, &help, &version, &bar, &statistics, &speedtest };

	inline std::string join(const std::vector<std::string>& v, const char* delim)
	{
		std::string out;

		for (size_t i = 0; i < v.size(); ++i)
		{
			out += v[i];

			if (i + 1 < v.size())
				out += delim;
		}

		return out;
	}

	inline std::string text_help = []()
		{
			std::string s;

			s += "\nUsage: <source_file_or_directory> [target_file_or_directory] [options]\n\n";
			s += "Options:\n\n";
			s += "  -async        Run conversion in parallel worker threads\n";
			s += "                Currently supported only for ciff target format\n";
			s += "  -help         Show this help message\n";
			s += "  -version      Version\n";
			s += "  -bar          Show progress bar\n";
			s += "  -format <ext> Select default target format when target_file is omitted\n";
			s += "                Accepts ";
			s += join(target_ext, ", ");
			s += "\n";
			s += "                Also accepts --format <ext> and -format=<ext>\n";
			s += "  -statistics   Show statistics after processing\n";
			s += "  -speedtest    Run benchmark mode for comparison with other tools\n";
			s += "                Disables progress bar and statistics\n";
			s += "                Does not write an output file\n";
			s += "\n";
			s += "File extensions:\n\n";
			s += "  Source: ";
			s += join(source_ext, ", ");
			s += "\n";
			s += "  Target: ";
			s += join(target_ext, ", ");
			s += "\n\n";
			s += "Notes:\n\n";
			s += "  Paths are resolved relative to the current working directory.\n";
			s += "  If target_file is given without a directory, it is placed next to the source file.\n";
			s += "  If target_file is a directory, output is placed in that directory.\n";
			s += "  If source_file is a directory and target_file is omitted, matching source files are batch converted.\n";
			s += "  In that mode, -format controls the output extension for the generated files.\n";
			s += "  If target_file is omitted, the default target format is ciff unless -format is specified.\n";
			return s;
		}();

	inline std::string format(const Msg m,
		const std::string& s = std::string(),
		const std::filesystem::path& p = std::filesystem::path())
	{
		switch (m)
		{
		case Msg::NoFileSpecified:
			return "No file specified";
		case Msg::NoFilesSpecified:
			return "No files specified";
		case Msg::AcceptsOnlyOneFile:
			return "Accepts only one file";
		case Msg::AcceptsOnlySourceAndTargetFiles:
			return "Accepts only source and target files";
		case Msg::TooManyArguments:
			return "Too many arguments";
		case Msg::UnknownFlag:
			return "Unknown flag " + s;
		case Msg::FlagRequiresValue:
			return "Flag requires a value " + s;
		case Msg::InvalidFormatExtension:
			return "Format is not a valid target extension: " + s;
		case Msg::SourceNotFound:
			return "Could not find the source file " + p.string();
		case Msg::SourceInvalidExtension:
			return "Source file is not a valid extension: " + p.string();
		case Msg::TargetFileUnknownDirectory:
			return "Target file has unknown directory " + p.string();
		case Msg::TargetDirectoryDoesNotExist:
			return "Target directory does not exist " + p.string();
		case Msg::TargetDirectoryIsNotADirectory:
			return "Target directory is not a directory " + p.string();
		case Msg::TargetInvalidExtension:
			return "Target file is not a valid extension: " + p.string();
		case Msg::SourceAndTargetAreSame:
			return "Source and target files are the same";
		default:
			return "Unknown error";
		}
	}

	inline std::filesystem::path source;
	inline std::filesystem::path target;
	inline std::string target_format;

	inline std::string defaultExt(const std::vector<std::string>& list)
	{
		return list.empty() ? std::string() : list.front();
	}

	inline std::string defaultTargetExt()
	{
		if (!target_format.empty())
			return target_format;

		return defaultExt(target_ext);
	}

	inline std::string tolower(std::string txt)
	{
		std::transform(txt.begin(), txt.end(), txt.begin(),
			[](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return txt;
	}

	inline std::string normalizeExtension(std::string ext)
	{
		ext = tolower(std::move(ext));

		if (!ext.empty() && ext.front() == '.')
			ext.erase(ext.begin());

		return ext;
	}

	inline std::string getExtension(const std::filesystem::path& file)
	{
		return normalizeExtension(file.extension().string());
	}

	inline bool err(const std::string& msg)
	{
		std::cerr << "Error: " << msg << "\n";
		return false;
	}

	inline bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)
	{
		const auto normalize = [](std::filesystem::path path)
		{
			path = path.lexically_normal();
			path.make_preferred();
			return tolower(path.string());
		};

		std::error_code ecA;
		std::error_code ecB;
		std::error_code ecEq;

		if (std::filesystem::exists(a, ecA) && std::filesystem::exists(b, ecB) &&
			std::filesystem::equivalent(a, b, ecEq))
			return true;

		ecA.clear();
		ecB.clear();

		const auto absA = std::filesystem::absolute(a, ecA);
		const auto absB = std::filesystem::absolute(b, ecB);

		if (!ecA && !ecB && normalize(absA) == normalize(absB))
			return true;

		ecA.clear();
		ecB.clear();

		const auto weakA = std::filesystem::weakly_canonical(a, ecA);
		const auto weakB = std::filesystem::weakly_canonical(b, ecB);

		if (!ecA && !ecB && normalize(weakA) == normalize(weakB))
			return true;

		return normalize(a) == normalize(b);
	}

	class CmdArgumentParser
	{
	public:
		CmdArgumentParser() = default;

		std::filesystem::path source() const
		{
			return _source;
		}

		std::filesystem::path target() const
		{
			return _target;
		}

		bool parse(const int argc, char* argv[])
		{
			check();

			std::vector<std::string> flags;
			std::vector<std::string> files;

			for (int i = 1; i < argc; ++i)
			{
				std::string arg = argv[i] ? argv[i] : "";

				if (arg.empty())
					continue;

				if (parseFormatFlag(arg))
				{
					const auto value = extractInlineFlagValue(arg);

					if (!setTargetFormat(value))
						return false;

					continue;
				}

				if (arg == "-" + format_flag || arg == "--" + format_flag)
				{
					if (i + 1 >= argc)
						return err(Msg::FlagRequiresValue, arg);

					if (!setTargetFormat(argv[++i] ? argv[i] : ""))
						return false;

					continue;
				}

				if (arg.front() == '-')
					flags.emplace_back(arg);
				else
					files.emplace_back(arg);
			}

			if (!parseFlags(flags, files.size()))
				return false;

			return parseSourceTargetFiles(files);
		}

	private:
		static bool parseFormatFlag(const std::string& arg)
		{
			const auto shortPrefix = "-" + format_flag + "=";
			const auto longPrefix = "--" + format_flag + "=";
			return arg.rfind(shortPrefix, 0) == 0 || arg.rfind(longPrefix, 0) == 0;
		}

		static std::string extractInlineFlagValue(const std::string& arg)
		{
			const auto pos = arg.find('=');
			return pos == std::string::npos ? std::string() : arg.substr(pos + 1);
		}

		static bool setTargetFormat(const std::string& value)
		{
			const auto ext = normalizeExtension(value);

			if (ext.empty())
				return err(Msg::FlagRequiresValue, "-" + format_flag);

			if (!contains(target_ext, ext))
				return err(Msg::InvalidFormatExtension, ext);

			cmd::target_format = ext;
			return true;
		}

		template <class T, class V>
		static bool contains(const T& list, const V& value)
		{
			return std::find(list.begin(), list.end(), value) != list.end();
		}

		static bool err(const Msg m,
			const std::string& s = std::string(),
			const std::filesystem::path& p = std::filesystem::path())
		{
			return cmd::err(format(m, s, p));
		}

		static void info(const std::string& msg, const int code)
		{
			std::cout << msg << "\n";
			std::exit(code);
		}

		void check()
		{
			cmd::source.clear();
			cmd::target.clear();
			cmd::target_format.clear();

			for (auto* f : cmd_flags)
				f->clear();

			if (cmd::source_ext.empty())
				throw std::runtime_error("Error: source_ext is not defined.");
			if (!contains(cmd_flags, &help))
				throw std::runtime_error("Error: cmd_flags must contain 'help' flag.");
			if (!contains(cmd_flags, &version))
				throw std::runtime_error("Error: cmd_flags must contain 'version' flag.");
			if (cmd_flags.size() < 2)
				throw std::runtime_error("Error: cmd_flags is not defined.");
		}

		bool parseFlags(const std::vector<std::string>& flags, const size_t countFiles)
		{
			size_t countFlags = 0;

			for (const auto& flag : flags)
			{
				for (auto* f : cmd_flags)
				{
					const std::string s = *f;

					if (flag == "-" + s || flag == "--" + s)
					{
						*f = true;
						++countFlags;
						goto next_flag;
					}
				}

				return err(Msg::UnknownFlag, flag);

			next_flag:
				;
			}

			if (help || version)
			{
				if (countFlags > 1 || countFiles != 0)
					return err(Msg::TooManyArguments);

				if (help)
					info(text_help, 0);

				info(text_version, 0);
			}

			return true;
		}

		bool parseSourceTargetFiles(const std::vector<std::string>& files)
		{
			if (files.empty())
				return err(Msg::NoFilesSpecified);

			if (files.size() > 2)
				return err(Msg::AcceptsOnlySourceAndTargetFiles);

			_source = files.front();

			if (_source.parent_path().empty())
				_source = std::filesystem::current_path() / _source;

			if (!std::filesystem::exists(_source))
				return err(Msg::SourceNotFound, {}, _source);

			if (std::filesystem::is_directory(_source))
			{
				if (files.size() == 1)
				{
					cmd::source = _source;
					cmd::target.clear();
					return true;
				}

				_target = files.back();

				if (_target.parent_path().empty())
					_target = std::filesystem::current_path() / _target;

				if (!std::filesystem::exists(_target))
					return err(Msg::TargetDirectoryDoesNotExist, {}, _target);

				if (!std::filesystem::is_directory(_target))
					return err(Msg::TargetDirectoryIsNotADirectory, {}, _target);

				cmd::source = _source;
				cmd::target = _target;
				return true;
			}

			if (!contains(source_ext, getExtension(_source)))
				return err(Msg::SourceInvalidExtension, {}, _source);

			_target = _source;
			_target.replace_extension(defaultTargetExt());

			if (files.size() == 2)
			{
				_target = files.back();

				if (_target.parent_path().empty())
					_target = _source.parent_path() / _target;

				if (!_target.parent_path().empty() && !std::filesystem::is_directory(_target.parent_path()))
				{
					if (_target.has_extension())
						return err(Msg::TargetFileUnknownDirectory, {}, _target);

					return err(Msg::TargetDirectoryDoesNotExist, {}, _target);
				}

				if (!_target.has_extension())
				{
					if (!std::filesystem::exists(_target))
						return err(Msg::TargetDirectoryDoesNotExist, {}, _target);

					if (!std::filesystem::is_directory(_target))
						return err(Msg::TargetDirectoryIsNotADirectory, {}, _target);

					_target = _target / _source.filename();
					_target.replace_extension(defaultTargetExt());
				}
			}

			if (samePath(_source, _target))
				return err(Msg::SourceAndTargetAreSame);

			if (!contains(target_ext, getExtension(_target)))
				return err(Msg::TargetInvalidExtension, {}, _target);

			cmd::source = _source;
			cmd::target = _target;
			return true;
		}

		std::filesystem::path _source;
		std::filesystem::path _target;
	};

	inline bool parse(const int argc, char* argv[])
	{
		CmdArgumentParser command;
		return command.parse(argc, argv);
	}
}
