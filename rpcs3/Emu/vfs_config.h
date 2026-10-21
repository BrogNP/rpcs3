#pragma once

#include "Utilities/Config.h"

#include <vector>

struct cfg_vfs : cfg::node
{
	std::string get(const cfg::string& _cfg, std::string_view emu_dir = {}) const;
	void load();
	void save() const;
	static std::string get_path();

	cfg::string emulator_dir{ this, "$(EmulatorDir)" }; // Default (empty): taken from fs::get_config_dir()
	cfg::string active_disk{ this, "Active Disk", "" }; // Empty: $(EmulatorDir)dev_*, else $(EmulatorDir)disks/<name>/dev_* (for the default dev_hdd0/hdd1/flash/flash2/flash3 paths)
	cfg::string dev_hdd0{ this, "/dev_hdd0/", "$(EmulatorDir)dev_hdd0/" };
	cfg::string dev_hdd1{ this, "/dev_hdd1/", "$(EmulatorDir)dev_hdd1/" };
	cfg::string dev_flash{ this, "/dev_flash/", "$(EmulatorDir)dev_flash/" };
	cfg::string dev_flash2{ this, "/dev_flash2/", "$(EmulatorDir)dev_flash2/" };
	cfg::string dev_flash3{ this, "/dev_flash3/", "$(EmulatorDir)dev_flash3/" };
	cfg::string dev_bdvd{ this, "/dev_bdvd/", "$(EmulatorDir)dev_bdvd/" }; // Only mounted in some special cases
	cfg::string games_dir{ this, "/games/", "$(EmulatorDir)games/" }; // Not mounted
	cfg::string app_home{ this, "/app_home/" }; // Not mounted

	cfg::device_entry dev_usb{ this, "/dev_usb***/",
		{
			{ "/dev_usb000", cfg::device_info{.path = "$(EmulatorDir)dev_usb000/"} }
		}};

	std::string get_dev_flash() const
	{
		return get(dev_flash);
	}

	std::string get_dev_flash2() const
	{
		return get(dev_flash2);
	}

	std::string get_dev_flash3() const
	{
		return get(dev_flash3);
	}

	// Virtual disks: $(EmulatorDir)disks/<name>/{dev_hdd0,dev_hdd1,dev_flash,dev_flash2,dev_flash3,disk.yml}
	static constexpr u32 max_disk_gb = 1024;
	static bool is_valid_disk_name(std::string_view name);
	static std::string get_disks_root();
	static std::vector<std::string> list_disks();
	static u32 get_disk_size_gb(const std::string& name); // 0 if unknown
	static bool set_disk_size_gb(const std::string& name, u32 size_gb);
	static bool create_disk(const std::string& name, u32 size_gb);
	u64 get_disk_limit() const; // Size limit of the active disk in bytes, 0 if no disk is active

	cfg::device_info get_device(const cfg::device_entry& _cfg, std::string_view key, std::string_view emu_dir = {}) const;

private:
	std::string get(std::string_view _cfg, std::string_view def, std::string_view emu_dir) const;
	cfg::device_info get_device_info(const cfg::device_entry& _cfg, std::string_view key, std::string_view emu_dir = {}) const;
};

extern cfg_vfs g_cfg_vfs;
