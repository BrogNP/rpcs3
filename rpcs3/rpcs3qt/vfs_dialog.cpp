#include "vfs_dialog.h"
#include "vfs_dialog_tab.h"
#include "vfs_dialog_usb_tab.h"
#include "gui_settings.h"

#include <QTabWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QInputDialog>

#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"

vfs_dialog::vfs_dialog(std::shared_ptr<gui_settings> _gui_settings, QWidget* parent)
	: QDialog(parent), m_gui_settings(std::move(_gui_settings))
{
	setWindowTitle(tr("Virtual File System"));
	setObjectName("vfs_dialog");

	QTabWidget* tabs = new QTabWidget();
	tabs->setUsesScrollButtons(false);

	g_cfg_vfs.load();

	// Create tabs
	vfs_dialog_tab* emulator_tab = new vfs_dialog_tab("$(EmulatorDir)", gui::fs_emulator_dir_list, &g_cfg_vfs.emulator_dir, m_gui_settings, this);
	vfs_dialog_tab* dev_hdd0_tab = new vfs_dialog_tab("dev_hdd0", gui::fs_dev_hdd0_list, &g_cfg_vfs.dev_hdd0, m_gui_settings, this);
	vfs_dialog_tab* dev_hdd1_tab = new vfs_dialog_tab("dev_hdd1", gui::fs_dev_hdd1_list, &g_cfg_vfs.dev_hdd1, m_gui_settings, this);
	vfs_dialog_tab* dev_flash_tab = new vfs_dialog_tab("dev_flash", gui::fs_dev_flash_list, &g_cfg_vfs.dev_flash, m_gui_settings, this);
	vfs_dialog_tab* dev_flash2_tab = new vfs_dialog_tab("dev_flash2", gui::fs_dev_flash2_list, &g_cfg_vfs.dev_flash2, m_gui_settings, this);
	vfs_dialog_tab* dev_flash3_tab = new vfs_dialog_tab("dev_flash3", gui::fs_dev_flash3_list, &g_cfg_vfs.dev_flash3, m_gui_settings, this);
	vfs_dialog_tab* dev_bdvd_tab = new vfs_dialog_tab("dev_bdvd", gui::fs_dev_bdvd_list, &g_cfg_vfs.dev_bdvd, m_gui_settings, this);
	vfs_dialog_usb_tab* dev_usb_tab = new vfs_dialog_usb_tab(&g_cfg_vfs.dev_usb, m_gui_settings, this);
	vfs_dialog_tab* games_tab = new vfs_dialog_tab("games", gui::fs_games_list, &g_cfg_vfs.games_dir, m_gui_settings, this);

	tabs->addTab(emulator_tab, "$(EmulatorDir)");
	tabs->addTab(dev_hdd0_tab, "dev_hdd0");
	tabs->addTab(dev_hdd1_tab, "dev_hdd1");
	tabs->addTab(dev_flash_tab, "dev_flash");
	tabs->addTab(dev_flash2_tab, "dev_flash2");
	tabs->addTab(dev_flash3_tab, "dev_flash3");
	tabs->addTab(dev_bdvd_tab, "dev_bdvd");
	tabs->addTab(dev_usb_tab, "dev_usb");
	tabs->addTab(games_tab, "games");

	// Disks tab: each disk has its own dev_hdd0/dev_hdd1/dev_flash/dev_flash2/dev_flash3 (so different firmwares can live side by side)
	QWidget* disks_tab = new QWidget();
	QComboBox* disk_combo = new QComboBox();
	QSpinBox* disk_size = new QSpinBox();
	QPushButton* disk_new = new QPushButton(tr("New Disk..."));
	disk_size->setRange(1, static_cast<int>(cfg_vfs::max_disk_gb));
	disk_size->setSuffix(tr(" GB"));

	const auto refresh_disks = [disk_combo, disk_size](const QString& select)
	{
		disk_combo->clear();
		disk_combo->addItem(tr("Default ($(EmulatorDir))"), QString());

		for (const std::string& name : cfg_vfs::list_disks())
		{
			disk_combo->addItem(QString::fromStdString(name), QString::fromStdString(name));
		}

		const int idx = disk_combo->findData(select);
		disk_combo->setCurrentIndex(idx < 0 ? 0 : idx);
		disk_size->setEnabled(disk_combo->currentIndex() > 0);
	};

	refresh_disks(QString::fromStdString(g_cfg_vfs.active_disk.to_string()));

	const auto load_disk_size = [disk_combo, disk_size]()
	{
		const std::string name = disk_combo->currentData().toString().toStdString();
		disk_size->setEnabled(!name.empty());

		if (!name.empty())
		{
			disk_size->setValue(static_cast<int>(std::max(1u, cfg_vfs::get_disk_size_gb(name))));
		}
	};

	load_disk_size();
	connect(disk_combo, &QComboBox::currentIndexChanged, this, load_disk_size);

	connect(disk_new, &QPushButton::clicked, this, [this, disk_combo, disk_size, refresh_disks]()
	{
		bool ok = false;
		const QString name = QInputDialog::getText(this, tr("New Disk"), tr("Disk name (letters, digits, space, - _ .):"), QLineEdit::Normal, QString(), &ok).trimmed();

		if (!ok || name.isEmpty())
			return;

		if (!cfg_vfs::create_disk(name.toStdString(), static_cast<u32>(disk_size->isEnabled() ? disk_size->value() : 500)))
		{
			QMessageBox::warning(this, tr("New Disk"), tr("Could not create the disk (invalid name or it already exists)."));
			return;
		}

		refresh_disks(name);
	});

	QFormLayout* disks_layout = new QFormLayout();
	QLabel* disks_info = new QLabel(tr("Each disk has its own dev_hdd0, dev_hdd1, dev_flash, dev_flash2 and dev_flash3 in $(EmulatorDir)disks/<name>/, so a different firmware can be installed on each. Only directories left at their default location move with the disk. Changing the disk takes effect on the next boot."));
	disks_info->setWordWrap(true);
	disks_layout->addRow(disks_info);
	disks_layout->addRow(tr("Active disk:"), disk_combo);
	disks_layout->addRow(tr("Size limit (max 1 TB):"), disk_size);
	disks_layout->addRow(disk_new);
	disks_tab->setLayout(disks_layout);
	tabs->insertTab(0, disks_tab, tr("Disks"));
	tabs->setCurrentIndex(0);

	// Create buttons
	QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Save | QDialogButtonBox::RestoreDefaults);
	buttons->button(QDialogButtonBox::RestoreDefaults)->setText(tr("Reset Directories"));
	buttons->button(QDialogButtonBox::Save)->setDefault(true);

	connect(buttons, &QDialogButtonBox::clicked, this, [this, buttons, tabs, disk_combo, disk_size](QAbstractButton* button)
	{
		if (button == buttons->button(QDialogButtonBox::RestoreDefaults))
		{
			if (QMessageBox::question(this, tr("Confirm Reset"), tr("Reset all file system directories?")) != QMessageBox::Yes)
				return;

			for (int i = 0; i < tabs->count(); ++i)
			{
				if (tabs->tabText(i) == tr("Disks"))
				{
					continue;
				}

				if (tabs->tabText(i) == "dev_usb")
				{
					static_cast<vfs_dialog_usb_tab*>(tabs->widget(i))->reset();
				}
				else
				{
					static_cast<vfs_dialog_tab*>(tabs->widget(i))->reset();
				}
			}
		}
		else if (button == buttons->button(QDialogButtonBox::Save))
		{
			{
				const std::string disk = disk_combo->currentData().toString().toStdString();

				if (!disk.empty())
				{
					cfg_vfs::set_disk_size_gb(disk, static_cast<u32>(disk_size->value()));
				}

				g_cfg_vfs.active_disk.from_string(disk);
			}

			for (int i = 0; i < tabs->count(); ++i)
			{
				if (tabs->tabText(i) == tr("Disks"))
				{
					continue;
				}

				if (tabs->tabText(i) == "dev_usb")
				{
					static_cast<vfs_dialog_usb_tab*>(tabs->widget(i))->set_settings();
				}
				else if (tabs->tabText(i) == "games")
				{
					// Check if the games folder changed. If changed, reconciliate the game list for the old folder before setting the new one
					if (const std::string games_dir = rpcs3::utils::get_games_dir(); games_dir != static_cast<vfs_dialog_tab*>(tabs->widget(i))->get_selected_path())
					{
						// Remove the detected serials (title id) belonging to old folder from the game list in memory and also in "games.yml" file
						Emu.RemoveGamesFromDir(games_dir);
					}

					static_cast<vfs_dialog_tab*>(tabs->widget(i))->set_settings(); // set new folder
				}
				else
				{
					static_cast<vfs_dialog_tab*>(tabs->widget(i))->set_settings();
				}
			}

			g_cfg_vfs.save();

			// Recreate folder structure for new VFS paths
			if (Emu.IsStopped())
			{
				Emu.Init();
			}

			accept();
		}
		else if (button == buttons->button(QDialogButtonBox::Close))
		{
			reject();
		}
	});

	QVBoxLayout* vbox = new QVBoxLayout;
	vbox->addWidget(tabs);
	vbox->addWidget(buttons);

	setLayout(vbox);

	buttons->button(QDialogButtonBox::Save)->setFocus();
}
