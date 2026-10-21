#include "stdafx.h"
#include "vfs_config.h"
#include "Utilities/StrUtil.h"

LOG_CHANNEL(vfs_log, "VFS");

cfg_vfs g_cfg_vfs{};

namespace
{
	struct cfg_disk : cfg::node
	{
		cfg::uint<1, cfg_vfs::max_disk_gb> size_gb{ this, "Size (GB)", 500 };
	};
}

std::string cfg_vfs::get(const cfg::string& _cfg, std::string_view emu_dir) const
{
	const std::string disk = active_disk.to_string();

	if (!disk.empty() && is_valid_disk_name(disk) &&
		(&_cfg == &dev_hdd0 || &_cfg == &dev_hdd1 || &_cfg == &dev_flash || &_cfg == &dev_flash2 || &_cfg == &dev_flash3))
	{
		constexpr std::string_view emu_var = "$(EmulatorDir)";
		const std::string cur = _cfg.to_string();

		// Only the default locations move with the disk, custom paths stay as configured
		if ((cur.empty() || cur == _cfg.def) && _cfg.def.starts_with(emu_var))
		{
			const std::string def = std::string(emu_var) + "disks/" + disk + "/" + _cfg.def.substr(emu_var.size());
			return get(def, def, emu_dir);
		}
	}

	return get(_cfg.to_string(), _cfg.def, emu_dir);
}

bool cfg_vfs::is_valid_disk_name(std::string_view name)
{
	if (name.empty() || name.size() > 64 || name.front() == '.' || name.back() == '.' || name.back() == ' ')
	{
		return false;
	}

	for (const char ch : name)
	{
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-' || ch == '.' || ch == ' '))
		{
			return false;
		}
	}

	return true;
}

std::string cfg_vfs::get_disks_root()
{
	std::string dir = g_cfg_vfs.emulator_dir.to_string();

	if (dir.empty())
	{
		dir = fs::get_config_dir();
	}

	if (!dir.empty() && dir.back() != fs::delim[0] && dir.back() != fs::delim[1])
	{
		dir += '/';
	}

	return fmt::replace_all(dir, "$(EmulatorDir)", fs::get_config_dir()) + "disks/";
}

std::vector<std::string> cfg_vfs::list_disks()
{
	std::vector<std::string> result;

	for (const auto& entry : fs::dir(get_disks_root()))
	{
		if (entry.is_directory && is_valid_disk_name(entry.name))
		{
			result.push_back(entry.name);
		}
	}

	return result;
}

u32 cfg_vfs::get_disk_size_gb(const std::string& name)
{
	if (!is_valid_disk_name(name))
	{
		return 0;
	}

	if (fs::file f{get_disks_root() + name + "/disk.yml", fs::read})
	{
		cfg_disk disk;
		disk.from_string(f.to_string());
		return disk.size_gb;
	}

	return 0;
}

bool cfg_vfs::set_disk_size_gb(const std::string& name, u32 size_gb)
{
	if (!is_valid_disk_name(name) || size_gb < 1 || size_gb > max_disk_gb)
	{
		return false;
	}

	cfg_disk disk;
	disk.size_gb.set(size_gb);

	fs::pending_file temp(get_disks_root() + name + "/disk.yml");

	if (!temp.file)
	{
		vfs_log.error("Could not save disk config of '%s' (error=%s)", name, fs::g_tls_error);
		return false;
	}

	temp.file.write(disk.to_string());
	return temp.commit();
}

bool cfg_vfs::create_disk(const std::string& name, u32 size_gb)
{
	if (!is_valid_disk_name(name) || size_gb < 1 || size_gb > max_disk_gb)
	{
		return false;
	}

	const std::string dir = get_disks_root() + name + "/";

	if (fs::is_dir(dir) || !fs::create_path(dir))
	{
		return false;
	}

	for (const char* dev : {"dev_hdd0", "dev_hdd1", "dev_flash", "dev_flash2", "dev_flash3"})
	{
		fs::create_path(dir + dev);
	}

	return set_disk_size_gb(name, size_gb);
}

u64 cfg_vfs::get_disk_limit() const
{
	const std::string disk = active_disk.to_string();

	if (disk.empty())
	{
		return 0;
	}

	return u64{get_disk_size_gb(disk)} * 1024 * 1024 * 1024;
}

std::string cfg_vfs::get(std::string_view _cfg, std::string_view def, std::string_view emu_dir) const
{
	std::string path = std::string(_cfg);

	// Fallback
	if (path.empty())
	{
		if (def.empty())
		{
			vfs_log.notice("VFS config called with empty path and empty default");
			return {};
		}

		path = def;
	}

	std::string _emu_dir; // Storage only

	if (emu_dir.empty())
	{
		// Optimization if provided arg
		_emu_dir = emulator_dir;

		if (_emu_dir.empty())
		{
			_emu_dir = fs::get_config_dir();
			ensure(!_emu_dir.empty());
		}

		// Check if path does not end with a delimiter
		if (_emu_dir.back() != fs::delim[0] && _emu_dir.back() != fs::delim[1])
		{
			_emu_dir += '/';
		}

		emu_dir = _emu_dir;
	}

	path = fmt::replace_all(path, "$(EmulatorDir)", emu_dir);

	// Check if path does not end with a delimiter
	if (path.empty())
	{
		vfs_log.error("VFS config path empty (_cfg='%s', def='%s', emu_dir='%s')", _cfg, def, emu_dir);
	}
	else if (path.back() != fs::delim[0] && path.back() != fs::delim[1])
	{
		path += '/';
	}

	return path;
}

cfg::device_info cfg_vfs::get_device(const cfg::device_entry& _cfg, std::string_view key, std::string_view emu_dir) const
{
	return get_device_info(_cfg, key, emu_dir);
}

cfg::device_info cfg_vfs::get_device_info(const cfg::device_entry& _cfg, std::string_view key, std::string_view emu_dir) const
{
	const auto& device_map = _cfg.get_map();

	if (auto it = device_map.find(key); it != device_map.cend())
	{
		// Make sure the path is properly resolved
		cfg::device_info info = it->second;
		const auto& def_map = _cfg.get_default();
		std::string def_path;

		if (auto def_it = def_map.find(key); def_it != def_map.cend())
		{
			def_path = def_it->second.path;
		}

		if (info.path.empty() && def_path.empty())
		{
			return info;
		}

		info.path = get(info.path, def_path, emu_dir);
		return info;
	}

	return {};
}

void cfg_vfs::load()
{
	const std::string path = cfg_vfs::get_path();

	if (fs::file cfg_file{path, fs::read})
	{
		vfs_log.notice("Loading VFS config. Path: %s", path);
		from_string(cfg_file.to_string());
	}
	else
	{
		vfs_log.notice("VFS config missing. Using default settings. Path: %s", path);
		from_default();
	}
}

void cfg_vfs::save() const
{
#ifdef _WIN32
	const std::string path_to_cfg = fs::get_config_dir(true);
	if (!fs::create_path(path_to_cfg))
	{
		vfs_log.error("Could not create path: %s", path_to_cfg);
	}
#endif

	fs::pending_file temp(cfg_vfs::get_path());

	if (!temp.file)
	{
		vfs_log.error("Could not save config: \"%s\" (error=%s)", cfg_vfs::get_path(), fs::g_tls_error);
		return;
	}

	temp.file.write(to_string());

	if (!temp.commit())
	{
		vfs_log.error("Could not save config: \"%s\" (error=%s)", cfg_vfs::get_path(), fs::g_tls_error);
	}
}

std::string cfg_vfs::get_path()
{
	return fs::get_config_dir(true) + "vfs.yml";
}
