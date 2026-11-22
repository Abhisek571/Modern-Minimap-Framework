#include "BakerFiles.h"

#include <TlHelp32.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace PhotoBaker
{
	namespace
	{
		std::string Lower(std::string_view a_s)
		{
			std::string out(a_s);
			for (auto& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		std::string_view Trim(std::string_view a_s)
		{
			const auto b = a_s.find_first_not_of(" \t");
			if (b == std::string_view::npos)
				return {};
			const auto e = a_s.find_last_not_of(" \t");
			return a_s.substr(b, e - b + 1);
		}

		bool ReadSmallFile(const fs::path& a_path, std::string& a_out)
		{
			// Every share mode: the plugin renames its temp file over this one while we read.
			const HANDLE h = CreateFileW(a_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return false;
			char  buf[65536];
			DWORD got = 0;
			const bool ok = ReadFile(h, buf, sizeof(buf), &got, nullptr) != FALSE;
			CloseHandle(h);
			if (!ok)
				return false;
			a_out.assign(buf, got);
			return true;
		}

		// ---------------------------------------------------------------- tiny JSON reader for the status file

		std::size_t ValueStart(const std::string& a_text, std::string_view a_key)
		{
			const std::string pattern = "\"" + std::string(a_key) + "\"";
			auto              pos = a_text.find(pattern);
			if (pos == std::string::npos)
				return pos;
			pos = a_text.find_first_not_of(" \t\r\n", pos + pattern.size());
			if (pos == std::string::npos || a_text[pos] != ':')
				return std::string::npos;
			return a_text.find_first_not_of(" \t\r\n", pos + 1);
		}

		void AppendUtf8(std::string& a_out, unsigned a_cp)
		{
			if (a_cp < 0x80) {
				a_out += static_cast<char>(a_cp);
			} else if (a_cp < 0x800) {
				a_out += static_cast<char>(0xC0 | (a_cp >> 6));
				a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
			} else {
				a_out += static_cast<char>(0xE0 | (a_cp >> 12));
				a_out += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
				a_out += static_cast<char>(0x80 | (a_cp & 0x3F));
			}
		}

		bool JsonString(const std::string& a_text, std::string_view a_key, std::string& a_out)
		{
			auto pos = ValueStart(a_text, a_key);
			if (pos == std::string::npos || a_text[pos] != '"')
				return false;
			std::string out;
			for (++pos; pos < a_text.size(); ++pos) {
				const char c = a_text[pos];
				if (c == '"') {
					a_out = std::move(out);
					return true;
				}
				if (c != '\\' || pos + 1 >= a_text.size()) {
					out += c;
					continue;
				}
				const char e = a_text[++pos];
				switch (e) {
				case 'n':
					out += '\n';
					break;
				case 't':
					out += '\t';
					break;
				case 'r':
				case 'b':
				case 'f':
					break;
				case 'u':
					if (pos + 4 < a_text.size()) {
						AppendUtf8(out, static_cast<unsigned>(std::strtoul(a_text.substr(pos + 1, 4).c_str(), nullptr, 16)));
						pos += 4;
					}
					break;
				default:
					out += e;  // \" \\ \/
					break;
				}
			}
			return false;  // cut off: the caller keeps the last good status
		}

		bool JsonNumber(const std::string& a_text, std::string_view a_key, double& a_out)
		{
			const auto pos = ValueStart(a_text, a_key);
			if (pos == std::string::npos)
				return false;
			char*        end = nullptr;
			const double v = std::strtod(a_text.c_str() + pos, &end);
			if (end == a_text.c_str() + pos)
				return false;
			a_out = v;
			return true;
		}

		// ---------------------------------------------------------------- game folder


		fs::path ParentProcessExe()
		{
			const DWORD self = GetCurrentProcessId();
			DWORD       parent = 0;
			const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snap == INVALID_HANDLE_VALUE)
				return {};
			PROCESSENTRY32W e{ sizeof(e) };
			for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
				if (e.th32ProcessID == self) {
					parent = e.th32ParentProcessID;
					break;
				}
			}
			CloseHandle(snap);
			if (!parent)
				return {};
			const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent);
			if (!h)
				return {};
			wchar_t buf[MAX_PATH * 4];
			DWORD   size = static_cast<DWORD>(std::size(buf));
			fs::path out;
			if (QueryFullProcessImageNameW(h, 0, buf, &size))
				out = std::wstring(buf, size);
			CloseHandle(h);
			return out;
		}

		// A portable MO2 instance keeps ModOrganizer.ini next to ModOrganizer.exe; its gamePath is the game the
		// instance runs (often a "Stock Game" copy that the registry knows nothing about).
		fs::path Mo2GamePath()
		{
			const auto mo2 = ParentProcessExe();
			if (mo2.empty())
				return {};
			std::ifstream in(mo2.parent_path() / "ModOrganizer.ini", std::ios::binary);
			std::string   line;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (Lower(line).rfind("gamepath=", 0) != 0)
					continue;
				std::string v = line.substr(9);
				if (v.rfind("@ByteArray(", 0) == 0 && v.size() > 12 && v.back() == ')')
					v = v.substr(11, v.size() - 12);
				std::string path;  // Qt writes backslashes doubled
				for (std::size_t i = 0; i < v.size(); ++i) {
					path += v[i];
					if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\')
						++i;
				}
				return fs::path(Widen(path));
			}
			return {};
		}

		fs::path RegistryGamePath()
		{
			wchar_t buf[MAX_PATH * 2];
			DWORD   size = sizeof(buf);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition", L"Installed Path", RRF_RT_REG_SZ, nullptr,
					buf, &size) != ERROR_SUCCESS)
				return {};
			return fs::path(buf);
		}

		bool Exists(const fs::path& a_p)
		{
			std::error_code ec;
			return fs::exists(a_p, ec);
		}
	}

	std::wstring Widen(std::string_view a_utf8)
	{
		if (a_utf8.empty())
			return {};
		const int    n = MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), nullptr, 0);
		std::wstring out(static_cast<std::size_t>(n), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, a_utf8.data(), static_cast<int>(a_utf8.size()), out.data(), n);
		return out;
	}

	bool ReadStatus(const fs::path& a_path, BakeStatus& a_out)
	{
		std::string text;
		if (!ReadSmallFile(a_path, text))
			return false;
		BakeStatus s;
		double     found = -1, done = 0, eta = 0, pid = 0;
		if (!JsonString(text, "state", s.state))
			return false;
		JsonString(text, "phase", s.phase);
		JsonString(text, "message", s.message);
		JsonNumber(text, "found", found);
		JsonNumber(text, "done", done);
		JsonNumber(text, "etaSeconds", eta);
		JsonNumber(text, "pid", pid);
		s.found = static_cast<int>(found);
		s.done = static_cast<int>(done);
		s.etaSeconds = eta;
		s.pid = static_cast<DWORD>(pid);
		a_out = std::move(s);
		return true;
	}

	bool SetIniValues(const fs::path& a_path, std::string_view a_section, const std::vector<std::pair<std::string, std::string>>& a_values, std::wstring& a_error)
	{
		std::string text;
		{
			std::ifstream in(a_path, std::ios::binary);
			std::ostringstream s;
			s << in.rdbuf();
			text = s.str();
		}
		const std::string eol = text.find("\r\n") != std::string::npos || text.empty() ? "\r\n" : "\n";
		std::vector<std::string> lines;
		{
			std::istringstream in(text);
			std::string        line;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				lines.push_back(std::move(line));
			}
		}
		const auto wanted = Lower(a_section);
		// Section bounds: [begin, end) are its lines after the header.
		std::size_t begin = std::string::npos, end = lines.size();
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const auto t = Trim(lines[i]);
			if (t.size() < 2 || t.front() != '[' || t.back() != ']')
				continue;
			if (begin != std::string::npos) {
				end = i;
				break;
			}
			if (Lower(Trim(t.substr(1, t.size() - 2))) == wanted)
				begin = i + 1;
		}
		if (begin == std::string::npos) {
			if (!lines.empty() && !Trim(lines.back()).empty())
				lines.emplace_back();
			lines.push_back("[" + std::string(a_section) + "]");
			begin = end = lines.size();
		}
		for (const auto& [key, value] : a_values) {
			const auto k = Lower(key);
			bool       set = false;
			for (std::size_t i = begin; i < end; ++i) {
				const auto t = Trim(lines[i]);
				const auto eq = t.find('=');
				if (t.empty() || t.front() == ';' || t.front() == '#' || eq == std::string_view::npos)
					continue;
				if (Lower(Trim(t.substr(0, eq))) == k) {
					lines[i] = key + " = " + value;
					set = true;
					break;
				}
			}
			if (!set) {
				// After the section's last non-blank line, so the blank line before the next section stays.
				std::size_t at = end;
				while (at > begin && Trim(lines[at - 1]).empty())
					--at;
				lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), key + " = " + value);
				++end;
			}
		}
		std::string out;
		for (const auto& line : lines)
			out += line + eol;
		// Written in place, like the plugin's own ini writer: under MO2 that edits the file in the mod that
		// provides it, where a temp file plus rename could end up split between that mod and overwrite.
		std::error_code ec;
		fs::create_directories(a_path.parent_path(), ec);
		std::ofstream file(a_path, std::ios::binary | std::ios::trunc);
		file.write(out.data(), static_cast<std::streamsize>(out.size()));
		if (!file) {
			a_error = L"Could not write " + a_path.wstring();
			return false;
		}
		return true;
	}

	fs::path AppFolder()
	{
		wchar_t     buf[MAX_PATH * 4];
		const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
		return n ? fs::path(std::wstring(buf, n)).parent_path() : fs::path{};
	}

	bool IsGameFolder(const fs::path& a_dir) { return !a_dir.empty() && Exists(a_dir / "SkyrimSE.exe"); }

	bool StartedFromMo2()
	{
		const auto parent = ParentProcessExe().filename().wstring();
		return _wcsicmp(parent.c_str(), L"ModOrganizer.exe") == 0;
	}

	fs::path FindGameFolder(std::wstring& a_how)
	{
		if (StartedFromMo2()) {
			if (const auto p = Mo2GamePath(); IsGameFolder(p)) {
				a_how = L"from Mod Organizer 2";
				return p;
			}
		}
		// The working folder first (MO2's "Start in", or a shortcut's), then the app's own folder; both with parents,
		// so the app also works from a "Tools" sub folder of the game.
		std::error_code ec;
		for (auto [start, how] : { std::pair{ fs::current_path(ec), L"from the working folder" }, std::pair{ AppFolder(), L"next to this app" } }) {
			for (int up = 0; up < 4 && !start.empty(); ++up) {
				if (IsGameFolder(start)) {
					a_how = how;
					return start;
				}
				if (start == start.parent_path())
					break;
				start = start.parent_path();
			}
		}
		if (const auto p = RegistryGamePath(); IsGameFolder(p)) {
			a_how = L"from the registry";
			return p;
		}
		return {};
	}

	DWORD FindProcess(const wchar_t* a_exe, const FILETIME* a_after)
	{
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE)
			return 0;
		DWORD           found = 0;
		PROCESSENTRY32W e{ sizeof(e) };
		for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
			if (_wcsicmp(e.szExeFile, a_exe) != 0)
				continue;
			if (!a_after) {
				found = e.th32ProcessID;
				break;
			}
			// Only a game started by this run: an older one still shutting down must not be mistaken for it.
			if (const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID)) {
				FILETIME created{}, exited{}, kernel{}, user{};
				if (GetProcessTimes(h, &created, &exited, &kernel, &user) && CompareFileTime(&created, a_after) >= 0)
					found = e.th32ProcessID;
				CloseHandle(h);
			}
		}
		CloseHandle(snap);
		return found;
	}

	MoveResult MoveNewTiles(const fs::path& a_from, const fs::path& a_to, const FILETIME& a_newerThan)
	{
		MoveResult       r;
		WIN32_FIND_DATAW fd{};
		const HANDLE     find = FindFirstFileExW((a_from / "*.dds").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
		if (find == INVALID_HANDLE_VALUE)
			return r;
		std::error_code ec;
		fs::create_directories(a_to, ec);
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			// Older files are the photos that came with the mod (or an earlier bake): never touch them.
			if (CompareFileTime(&fd.ftLastWriteTime, &a_newerThan) <= 0)
				continue;
			const auto src = a_from / fd.cFileName;
			const auto dst = a_to / fd.cFileName;
			if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
				++r.failed;
				continue;
			}
			if (DeleteFileW(src.c_str()))
				++r.moved;
			else
				++r.copiedOnly;
		} while (FindNextFileW(find, &fd));
		FindClose(find);
		return r;
	}
}
