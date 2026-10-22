#include "stdafx.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/System.h"
#include "sys_event.h"
#include "sys_fs.h"
#include "util/shared_ptr.hpp"

#include "sys_storage.h"
#include "sys_event.h"

LOG_CHANNEL(sys_storage);

namespace
{
	// EXPERIMENT: our writes never persist to real storage, so fsck/newfs always reads back
	// zeros where it expects a real UFS2-derived filesystem, and its error-recovery path has a
	// bug that crashes RPCS3 ("alloc 1065: exit with 38" in TTY output). Fake a minimal,
	// internally-consistent superblock + BSD disklabel + cylinder group 0 so those checks pass.
	// Structures/algorithms (UFS2 superblock, BSD disklabel + dkcksum) are public BSD formats,
	// not proprietary - PS3 firmware source strings confirm this is derived from stock BSD
	// newfs_cfs/fsck_cfs code (real FreeBSD sysctl names like "vfs.ffs.adjrefcnt" appear in it).
	constexpr u32 UFS2_SB_SECTOR    = 128;  // SBLOCK_UFS2 = byte 65536 = sector 128
	constexpr u32 DISKLABEL_SECTOR  = 0xc0; // empirically observed read address in RPCS3 log
	constexpr u32 CG0_SECTOR        = 400;  // arbitrary, must agree with fs_cblkno below

	constexpr u32 FS_BSIZE = 8192;
	constexpr u32 FS_FSIZE = 1024;
	constexpr u32 FS_FRAG  = 8;
	constexpr u32 SECTORS_PER_FRAG = FS_FSIZE / 512; // 2

	void write_be32(u8* buf, u32 off, u32 val)
	{
		buf[off + 0] = static_cast<u8>(val >> 24);
		buf[off + 1] = static_cast<u8>(val >> 16);
		buf[off + 2] = static_cast<u8>(val >> 8);
		buf[off + 3] = static_cast<u8>(val);
	}

	void write_be16(u8* buf, u32 off, u16 val)
	{
		buf[off + 0] = static_cast<u8>(val >> 8);
		buf[off + 1] = static_cast<u8>(val);
	}

	// Geometry of the fake filesystem: one cylinder group, 16 MB, 2048 inodes. Only what the firmware fsck needs to see a coherent,
	// clean-looking UFS2 (the real hdd0 content lives in the host directory and is never touched).
	constexpr u32 FS_IPG          = 2048;
	constexpr u32 FS_FPG          = 16384; // fragments (also fs_size)
	constexpr u32 FS_INOPB        = FS_BSIZE / 256;
	constexpr u32 SBLKNO_FRAG     = 32;
	constexpr u32 CBLKNO_FRAG     = CG0_SECTOR / SECTORS_PER_FRAG; // 200
	constexpr u32 IBLKNO_FRAG     = CBLKNO_FRAG + FS_FRAG; // 208
	constexpr u32 DBLKNO_FRAG     = IBLKNO_FRAG + FS_IPG / FS_INOPB * FS_FRAG; // 720
	constexpr u32 ROOT_DIR_FRAG   = DBLKNO_FRAG; // root directory: one fragment at the start of the data area
	constexpr u32 CSUM_FRAG       = DBLKNO_FRAG + FS_FRAG; // cylinder group summary (fs_csaddr), one fragment
	constexpr u32 ALT_SB_SECTOR   = SBLKNO_FRAG * SECTORS_PER_FRAG;
	constexpr u32 INODE_SECTOR    = IBLKNO_FRAG * SECTORS_PER_FRAG; // inode 0/1; inode 2 (root) is in the next sector
	constexpr u32 ROOT_DIR_SECTOR = ROOT_DIR_FRAG * SECTORS_PER_FRAG;
	constexpr u32 CSUM_SECTOR     = CSUM_FRAG * SECTORS_PER_FRAG;

	void write_be64(u8* buf, u32 off, u64 val)
	{
		write_be32(buf, off, static_cast<u32>(val >> 32));
		write_be32(buf, off + 4, static_cast<u32>(val));
	}

	// Free space accounting shared by the superblock summary and cylinder group 0
	constexpr u32 FS_NDIR   = 1;
	constexpr u32 FS_NIFREE = FS_IPG - 3; // inodes 0, 1 (reserved) and 2 (root) used
	constexpr u32 FS_NFFREE = 2 * (FS_FRAG - 1); // rest of the two partly used blocks
	constexpr u32 FS_NBFREE = (FS_FPG - (CSUM_FRAG + FS_FRAG)) / FS_FRAG;
	constexpr u32 FS_DSIZE  = FS_FPG - DBLKNO_FRAG;

	// UFS2 superblock (struct fs, FreeBSD layout, big endian). fs_magic at offset 1372 is fixed regardless of layout.
	void fake_ufs2_superblock(u8* buf)
	{
		write_be32(buf, 8, SBLKNO_FRAG);                    // fs_sblkno
		write_be32(buf, 12, CBLKNO_FRAG);                   // fs_cblkno -> cgtod(fs,0) == CG0_SECTOR
		write_be32(buf, 16, IBLKNO_FRAG);                   // fs_iblkno
		write_be32(buf, 20, DBLKNO_FRAG);                   // fs_dblkno
		write_be32(buf, 36, FS_FPG);                        // fs_old_size
		write_be32(buf, 40, FS_DSIZE);                      // fs_old_dsize
		write_be32(buf, 44, 1);                             // fs_ncg
		write_be32(buf, 48, FS_BSIZE);                      // fs_bsize
		write_be32(buf, 52, FS_FSIZE);                      // fs_fsize
		write_be32(buf, 56, FS_FRAG);                       // fs_frag
		write_be32(buf, 60, 8);                             // fs_minfree
		write_be32(buf, 72, ~(FS_BSIZE - 1));               // fs_bmask
		write_be32(buf, 76, ~(FS_FSIZE - 1));               // fs_fmask
		write_be32(buf, 80, 13);                            // fs_bshift
		write_be32(buf, 84, 10);                            // fs_fshift
		write_be32(buf, 88, 8);                             // fs_maxcontig
		write_be32(buf, 92, FS_BSIZE / 4);                  // fs_maxbpg
		write_be32(buf, 96, 3);                             // fs_fragshift
		write_be32(buf, 100, 1);                            // fs_fsbtodb (log2(fsize / 512))
		write_be32(buf, 104, 2048);                         // fs_sbsize
		write_be32(buf, 116, FS_BSIZE / 8);                 // fs_nindir
		write_be32(buf, 120, FS_INOPB);                     // fs_inopb
		write_be32(buf, 128, 1);                            // fs_optim (FS_OPTSPACE)
		write_be32(buf, 152, CSUM_FRAG);                    // fs_old_csaddr
		write_be32(buf, 156, 1024);                         // fs_cssize
		write_be32(buf, 160, FS_BSIZE);                     // fs_cgsize
		write_be32(buf, 184, FS_IPG);                       // fs_ipg
		write_be32(buf, 188, FS_FPG);                       // fs_fpg
		buf[208] = 0;                                       // fs_fmod
		buf[209] = 1;                                       // fs_clean
		buf[210] = 0;                                       // fs_ronly
		write_be32(buf, 872, FS_BSIZE);                     // fs_maxbsize
		write_be64(buf, 1000, 65536);                       // fs_sblockloc
		write_be64(buf, 1008, FS_NDIR);                     // fs_cstotal.cs_ndir
		write_be64(buf, 1016, FS_NBFREE);                   // cs_nbfree
		write_be64(buf, 1024, FS_NIFREE);                   // cs_nifree
		write_be64(buf, 1032, FS_NFFREE);                   // cs_nffree
		write_be64(buf, 1080, FS_FPG);                      // fs_size
		write_be64(buf, 1088, FS_DSIZE);                    // fs_dsize
		write_be64(buf, 1096, CSUM_FRAG);                   // fs_csaddr
		write_be32(buf, 1196, 16384);                       // fs_avgfilesize
		write_be32(buf, 1200, 64);                          // fs_avgfpdir
		write_be32(buf, 1312, 2);                           // fs_flags = FS_DOSOFTDEP: the boot time "fsck -F" answers 5 (no background check possible) without it
		write_be32(buf, 1320, 120);                         // fs_maxsymlinklen
		write_be32(buf, 1324, 2);                           // fs_old_inodefmt (FS_44INODEFMT)
		write_be64(buf, 1328, 0x7fffffffffffull);           // fs_maxfilesize
		write_be64(buf, 1336, FS_BSIZE - 1);                // fs_qbmask
		write_be64(buf, 1344, FS_FSIZE - 1);                // fs_qfmask
		write_be32(buf, 1372, 0x19540119);                  // fs_magic (FS_UFS2_MAGIC)
	}

	// Classic BSD struct disklabel, with a real dkcksum()-compatible checksum so validation
	// (XOR of all u16 words from d_magic through d_partitions[d_npartitions], expecting 0
	// once d_checksum itself is included) actually passes rather than just matching the magic.
	void fake_disklabel(u8* buf)
	{
		constexpr u32 d_npartitions = 1;
		constexpr u32 total_len = 148 + d_npartitions * 16; // up to end of d_partitions[0]

		write_be32(buf, 0, 0x82564557);       // d_magic
		write_be32(buf, 40, 512);             // d_secsize
		write_be32(buf, 132, 0x82564557);     // d_magic2
		write_be16(buf, 136, 0);              // d_checksum (0 while computing)
		write_be16(buf, 138, d_npartitions);  // d_npartitions

		// partition[0] ('c', whole disk) - left all-zero before, which made whatever reads
		// the disklabel derive a bogus block number ("CANNOT READ BLK: 128") from a degenerate
		// entry instead of an actual read failure.
		write_be32(buf, 148 + 0, 0x24FAEA98); // p_size (sectors, matches hdd0's declared size)
		write_be32(buf, 148 + 4, 0);          // p_offset
		write_be32(buf, 148 + 8, FS_FSIZE);   // p_fsize
		buf[148 + 12] = 7;                    // p_fstype = FS_BSDFFS
		buf[148 + 13] = FS_FRAG;              // p_frag
		write_be16(buf, 148 + 14, 16);        // p_cpg

		u16 sum = 0;
		for (u32 off = 0; off < total_len; off += 2)
		{
			sum = static_cast<u16>(sum ^ ((u16{buf[off]} << 8) | buf[off + 1]));
		}
		write_be16(buf, 136, sum);
	}

	// Cylinder group 0 as a full block (struct cg + inode-used and fragment-free bitmaps), matching the superblock above
	void fake_cylinder_group0_full(u8* buf)
	{
		constexpr u32 iusedoff = 168; // start of cg_space, as computed by the firmware fsck
		constexpr u32 freeoff = iusedoff + FS_IPG / 8;
		constexpr u32 nextfreeoff = freeoff + FS_FPG / 8;

		write_be32(buf, 4, 0x00090255);                     // cg_magic
		write_be32(buf, 12, 0);                             // cg_cgx
		write_be32(buf, 20, FS_FPG);                        // cg_ndblk
		write_be32(buf, 24, FS_NDIR);                       // cg_cs.cs_ndir
		write_be32(buf, 28, FS_NBFREE);                     // cs_nbfree
		write_be32(buf, 32, FS_NIFREE);                     // cs_nifree
		write_be32(buf, 36, FS_NFFREE);                     // cs_nffree
		write_be32(buf, 52 + 4 * (FS_FRAG - 1), 2);         // cg_frsum[7]: two runs of 7 free fragments
		write_be32(buf, 92, iusedoff);                      // cg_iusedoff
		write_be32(buf, 96, freeoff);                       // cg_freeoff
		write_be32(buf, 100, nextfreeoff);                  // cg_nextfreeoff
		write_be32(buf, 116, FS_IPG);                       // cg_niblk
		write_be32(buf, 120, FS_IPG);                       // cg_initediblk

		buf[iusedoff] = 0x07;                               // inodes 0, 1, 2 in use (LSB = lowest number)

		// Fragment map: bit set = free. In use: the metadata area, the root directory fragment and the cs summary fragment
		for (u32 frag = ROOT_DIR_FRAG + 1; frag < FS_FPG; frag++)
		{
			if (frag != CSUM_FRAG)
			{
				buf[freeoff + frag / 8] |= static_cast<u8>(1u << (frag % 8));
			}
		}
	}

	// UFS2 inode 2 (root directory), 256 bytes
	void fake_root_inode(u8* buf)
	{
		write_be16(buf, 0, 040755);                         // di_mode
		write_be16(buf, 2, 2);                              // di_nlink
		write_be32(buf, 12, FS_BSIZE);                      // di_blksize
		write_be64(buf, 16, 1024);                          // di_size (two directory blocks of 512 bytes)
		write_be64(buf, 24, FS_FSIZE / 512);                // di_blocks (512 byte units)
		for (u32 t = 32; t <= 56; t += 8)
		{
			write_be64(buf, t, 1262304000);                 // atime/mtime/ctime/birthtime
		}
		write_be64(buf, 112, ROOT_DIR_FRAG);                // di_db[0]
	}

	// Root directory fragment ("." and ".." in the first 512 byte block, empty second block)
	void fake_root_dir(u8* buf)
	{
		write_be32(buf, 0, 2);                              // "." : d_ino
		write_be16(buf, 4, 12);                             // d_reclen
		buf[6] = 4;                                         // d_type = DT_DIR
		buf[7] = 1;                                         // d_namlen
		buf[8] = '.';

		write_be32(buf, 12, 2);                             // ".."
		write_be16(buf, 16, 500);
		buf[18] = 4;
		buf[19] = 2;
		buf[20] = '.';
		buf[21] = '.';

		write_be16(buf, 512 + 4, 512);                      // second block: unused entry covering the block
	}

	// UFS2 cylinder group descriptor (struct cg) for cg 0 - only cg_magic is required to
	// clear the specific check RPCS3's crash trace showed comparing against CG_MAGIC (0x90255).
	void fake_cylinder_group0(u8* buf)
	{
		write_be32(buf, 4, 0x00090255); // cg_magic
	}
}

void sys_storage_fake_hdd_read_generated(u64 offset, u8* buf, u64 size);

// Raw reads of the (virtual) HDD device as done by the firmware fsck (fcntl ops 3/4 on the /dev_hdd0 directory handle).
// There is no real UFS2 image behind dev_hdd0, so present the minimal structures the fsck validates: everything else reads as zeros.
namespace
{
	// Sectors written by the firmware fsck ("repairs"); kept so later reads see them
	shared_mutex g_fake_hdd_mutex;
	std::map<u64, std::array<u8, 512>> g_fake_hdd_overlay;
}

void sys_storage_fake_hdd_write(u64 offset, const u8* buf, u64 size)
{
	std::lock_guard lock(g_fake_hdd_mutex);

	for (u64 pos = offset; pos < offset + size;)
	{
		const u64 sector = pos / 512;
		const u64 end = std::min((sector + 1) * 512, offset + size);

		auto it = g_fake_hdd_overlay.find(sector);

		if (it == g_fake_hdd_overlay.end())
		{
			// Start from the generated content so partial sector writes keep the rest
			std::array<u8, 512> base{};
			sys_storage_fake_hdd_read_generated(sector * 512, base.data(), 512);
			it = g_fake_hdd_overlay.emplace(sector, base).first;
		}

		std::memcpy(it->second.data() + (pos - sector * 512), buf + (pos - offset), end - pos);

		// There is nothing on this virtual disk that a repair could leave half done: the superblock (primary and the alternate copy)
		// always stays marked clean, otherwise the firmware's boot time quick check keeps asking for another full fsck forever
		for (const u64 sb_base : {u64{UFS2_SB_SECTOR} * 512, u64{ALT_SB_SECTOR} * 512})
		{
			const u64 clean_off = sb_base + 209; // fs_clean

			if (clean_off / 512 == sector)
			{
				it->second[clean_off % 512] = 1;
			}
		}

		pos = end;
	}
}

void sys_storage_fake_hdd_read(u64 offset, u8* buf, u64 size)
{
	sys_storage_fake_hdd_read_generated(offset, buf, size);

	std::lock_guard lock(g_fake_hdd_mutex);

	for (u64 pos = offset; pos < offset + size;)
	{
		const u64 sector = pos / 512;
		const u64 end = std::min((sector + 1) * 512, offset + size);

		if (const auto it = g_fake_hdd_overlay.find(sector); it != g_fake_hdd_overlay.end())
		{
			std::memcpy(buf + (pos - offset), it->second.data() + (pos - sector * 512), end - pos);
		}

		pos = end;
	}
}

void sys_storage_fake_hdd_read_generated(u64 offset, u8* buf, u64 size)
{
	std::memset(buf, 0, size);

	const u64 first = offset / 512;
	const u64 last = (offset + size + 511) / 512;

	static const auto render = [](u32 sector, u8* out)
	{
		u8 blk[FS_BSIZE]{};

		// Primary superblock (byte 65536) and the alternate copy fsck compares it with (fs_sblkno of cylinder group 0)
		if (sector >= UFS2_SB_SECTOR && sector < UFS2_SB_SECTOR + FS_BSIZE / 512)
		{
			fake_ufs2_superblock(blk);
			std::memcpy(out, blk + (sector - UFS2_SB_SECTOR) * 512, 512);
		}
		else if (sector >= ALT_SB_SECTOR && sector < ALT_SB_SECTOR + FS_BSIZE / 512)
		{
			fake_ufs2_superblock(blk);
			std::memcpy(out, blk + (sector - ALT_SB_SECTOR) * 512, 512);
		}
		else if (sector >= CG0_SECTOR && sector < CG0_SECTOR + FS_BSIZE / 512)
		{
			fake_cylinder_group0_full(blk);
			std::memcpy(out, blk + (sector - CG0_SECTOR) * 512, 512);
		}
		else if (sector == INODE_SECTOR + 1)
		{
			fake_root_inode(blk);
			std::memcpy(out, blk, 512);
		}
		else if (sector == ROOT_DIR_SECTOR || sector == ROOT_DIR_SECTOR + 1)
		{
			fake_root_dir(blk);
			std::memcpy(out, blk + (sector - ROOT_DIR_SECTOR) * 512, 512);
		}
		else if (sector == CSUM_SECTOR)
		{
			write_be32(blk, 0, FS_NDIR);
			write_be32(blk, 4, FS_NBFREE);
			write_be32(blk, 8, FS_NIFREE);
			write_be32(blk, 12, FS_NFFREE);
			std::memcpy(out, blk, 512);
		}
		else if (sector == DISKLABEL_SECTOR)
		{
			fake_disklabel(blk);
			std::memcpy(out, blk, 512);
		}
	};

	for (u64 sector = first; sector < last; sector++)
	{
		u8 tmp[512]{};
		render(static_cast<u32>(sector), tmp);

		const u64 sec_beg = sector * 512;
		const u64 beg = std::max(sec_beg, offset);
		const u64 end = std::min(sec_beg + 512, offset + size);
		std::memcpy(buf + (beg - offset), tmp + (beg - sec_beg), end - beg);
	}
}

namespace
{
	auto log_callback(ppu_thread& ppu)
	{	
		sys_storage.todo("Callstack:\n%s", ppu.dump_callstack());
	}

	struct storage_manager_impl
	{
		storage_manager_impl() noexcept {}
		storage_manager_impl(const storage_manager_impl&) = delete;
		int operator=(const storage_manager_impl&) = delete;

		bool send_event(u64 device_id, u64 data1, u64 data2, u64 data3)
		{
			id_manager::g_process = 0;
			std::vector<shared_ptr<lv2_storage_medium_event_port>> ports;

			idm::select<lv2_storage_medium_event_port>([&](u32 id, u32 proc, lv2_storage_medium_event_port& port)
			{
				// Check port status
				if (port.savable() && (!port.device_id || port.device_id == device_id))
				{
					// Detached ports can be removed
					ports.emplace_back(ensure(idm::get_unlocked<lv2_storage_medium_event_port>(idm::id_index(id, proc))));
				}
			});

			bool send = false;
			u64 dummy_kernel_port_address = 0x800000000062d4c0;

			for (auto& port : ports)
			{
				dummy_kernel_port_address += 0x100;

				if (port->medium_port->send(dummy_kernel_port_address, data1, data2, data3) == CELL_OK)
				{
					send = true;
				}
			}

			return send;
		}

		lf_fifo<atomic_t<u32>, 8> cmd_queue;

		bool pop(u32& out)
		{
			const u32 pos = cmd_queue.peek();

			// Clean command buffer for command tail
			out = cmd_queue[pos].exchange(0);

			if (!out)
			{
				return false;
			}

			// Free
			cmd_queue.pop_end(1);
			return true;
		}

		void push(u32 cmd)
		{
			const u32 pos = cmd_queue.push_begin();

			// Write single command
			cmd_queue[pos] = cmd;

			thread_ctrl::notify(static_cast<named_thread<storage_manager_impl>&>(*this));
		}

		void operator()() noexcept
		{
			while (Emu.IsPausedOrReady())
			{
				thread_ctrl::wait_for(2500);
			}

			// Send startup(?) events
			thread_ctrl::wait_for(2500000);

			while (!send_event(0x0101000000000006, 0x0000000000000101, 0x0000000000000000, 0x0101000000000006))
			{
				thread_ctrl::wait_for(2500000);
			}

			while (thread_ctrl::state() != thread_state::aborting)
			{
				u32 cmd_val = 0;

				if (!pop(cmd_val))
				{
					thread_ctrl::wait();
					continue;
				}

				if (cmd_val == 1)
				{
					sys_storage.notice("storage_manager(): Received insert event");

					send_event(0x0101000000000006, 0x0000000000000003, 0x000000000000ff71, 0x0101000000000006);
				}

				if (cmd_val == 2)
				{
					sys_storage.notice("storage_manager(): Received eject event");

					send_event(0x0101000000000006, 0x0000000000000004, 0x0000000000000000, 0x0101000000000006);
					send_event(0x0101000000000006, 0x0000000000000008, 0x0000000000000000, 0x0101000000000006);
					send_event(0x0101000000000006, 0x0000000000000007, 0x0000000000000000, 0x0101000000000006);
					//send_event(0x0101000000000006, 0x0000000000000102, 0x0000000000000000, 0x0101000000000006);
				}

				// Cooldown
				thread_ctrl::wait_for(250000);
			}
		}

		void send_bdvd_insert()
		{
			push(1);
		}

		void send_bdvd_eject()
		{
			push(2);
		}

		static constexpr auto thread_name = "VSH Storage Events"sv;
	};

	using storage_manager = named_thread<storage_manager_impl>;
}

extern void signal_sys_storage_about_BDVD_insert()
{
	ensure(g_fxo->try_get<storage_manager>())->send_bdvd_insert();
}

extern void signal_sys_storage_about_BDVD_eject()
{
	ensure(g_fxo->try_get<storage_manager>())->send_bdvd_eject();
}

lv2_storage::lv2_storage(utils::serial& ar) noexcept
	: lv2_obj{1}
	, device_id(ar)
	, mode(ar)
	, flags(ar)
{
	lv2_event_queue::load_ptr(ar, async_port, "lv2_storage");}

void lv2_storage::save(utils::serial& ar)
{
	ar(device_id, mode, flags);
	lv2_event_queue::save_ptr(ar, async_port.load().get());
}

lv2_storage_medium_event_port::lv2_storage_medium_event_port(utils::serial& ar) noexcept
	: device_id(ar)
{
	lv2_event_queue::load_ptr(ar, medium_port, "lv2_storage_medium_event_port");
}

void lv2_storage_medium_event_port::save(utils::serial& ar)
{
	ar(device_id);
	lv2_event_queue::save_ptr(ar, medium_port.get());
}

bool lv2_storage_medium_event_port::savable() const
{
	return lv2_obj::check(medium_port);
}

error_code sys_storage_open(ppu_thread& ppu, u64 device, u64 mode, vm::ptr<u32> fd, u64 flags)
{
	sys_storage.todo("sys_storage_open(device=0x%x, mode=0x%x, fd=*0x%x, flags=0x%x)", device, mode, fd, flags);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	if (device == 0)
	{
		return CELL_ENOENT;
	}

	if (!fd)
	{
		return CELL_EFAULT;
	}

	[[maybe_unused]] u64 storage_id = device & 0xFFFFF00FFFFFFFF;
	fs::file file;

	thread_local u32 weird = 0;

	// if (device == 0x0101000000000006)
	// {
	// 	if (weird < 1)
	// 	{
	// 		weird++;
	// 		return CELL_ENOEXEC;
	// 	}
	// }

	if (const u32 id = idm::make<lv2_obj, lv2_storage>(device, std::move(file), mode, flags))
	{
		*fd = id;
		sys_storage.notice("sys_storage_open(): Handle=0x%x", id);
		return CELL_OK;
	}

	return CELL_EAGAIN;
}

error_code sys_storage_close(u32 fd)
{
	sys_storage.todo("sys_storage_close(fd=0x%x)", fd);

	ensure(idm::remove<lv2_obj, lv2_storage>(fd));

	return CELL_OK;
}

error_code sys_storage_read(u32 fd, u32 mode, u32 start_sector, u32 num_sectors, vm::ptr<void> bounce_buf, vm::ptr<u32> sectors_read, u64 flags)
{
	log_callback(*cpu_thread::get_current<ppu_thread>());
	sys_storage.todo("sys_storage_read(fd=0x%x, mode=0x%x, start_sector=0x%x, num_sectors=0x%x, bounce_buf=*0x%x, sectors_read=*0x%x, flags=0x%x)", fd, mode, start_sector, num_sectors, bounce_buf, sectors_read, flags);

	if (!bounce_buf || !sectors_read)
	{
		return CELL_EFAULT;
	}

	std::memset(bounce_buf.get_ptr(), 0, num_sectors * 0x200ull);
	const auto handle = idm::get_unlocked<lv2_obj, lv2_storage>(fd);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	// EXPERIMENT: our writes never actually persist, so fsck/newfs always reads back zeros where
	// it expects a real filesystem. Fake the three structures it validates in sequence so it
	// doesn't hit the "corrupted" path (see comment block above sys_storage_open for details).
	// Scoped strictly to the real ATA_HDD device - matching on sector number alone previously
	// clobbered unrelated reads (other devices/partitions legitimately using the same sector
	// numbers), which crashed RPCS3 during boot.
	if ((handle->device_id & 0xFFFFF00FFFFFFFF) == ATA_HDD)
	{
		if (start_sector == UFS2_SB_SECTOR && num_sectors * 0x200ull >= 1376)
		{
			sys_storage.todo("sys_storage_read(): faking UFS2 superblock at sector %d", UFS2_SB_SECTOR);
			fake_ufs2_superblock(vm::_ptr<u8>(bounce_buf.addr()));
		}
		else if (start_sector == DISKLABEL_SECTOR && num_sectors >= 1)
		{
			sys_storage.todo("sys_storage_read(): faking disklabel at sector %d", DISKLABEL_SECTOR);
			fake_disklabel(vm::_ptr<u8>(bounce_buf.addr()));
		}
		else if (start_sector == CG0_SECTOR && num_sectors * 0x200ull >= 8)
		{
			sys_storage.todo("sys_storage_read(): faking cylinder group 0 at sector %d", CG0_SECTOR);
			fake_cylinder_group0(vm::_ptr<u8>(bounce_buf.addr()));
		}
	}

	if (handle->file)
	{
		handle->file.seek(start_sector * 0x200ull);
		const u64 size = num_sectors * 0x200ull;
		const u64 result = lv2_file::op_read(handle->file, bounce_buf, size);
		num_sectors = ::narrow<u32>(result / 0x200ull);
	}

	*sectors_read = num_sectors;

	return CELL_OK;
}

error_code sys_storage_write(u32 fd, u32 mode, u32 start_sector, u32 num_sectors, vm::ptr<void> data, vm::ptr<u32> sectors_wrote, u64 flags)
{
	sys_storage.todo("sys_storage_write(fd=0x%x, mode=0x%x, start_sector=0x%x, num_sectors=0x%x, data=*=0x%x, sectors_wrote=*0x%x, flags=0x%llx)", fd, mode, start_sector, num_sectors, data, sectors_wrote, flags);

	if (!sectors_wrote)
	{
		return CELL_OK;
	}

	const auto handle = idm::get_unlocked<lv2_obj, lv2_storage>(fd);

	if (!handle)
	{
		return CELL_OK;
	}

	*sectors_wrote = num_sectors;

	return CELL_OK;
}

error_code sys_storage_send_device_command(u32 dev_handle, u64 cmd, vm::ptr<void> in, u64 inlen, vm::ptr<void> out, u64 outlen)
{
	sys_storage.todo("sys_storage_send_device_command(dev_handle=0x%x, cmd=0x%llx, in=*0x%, inlen=0x%x, out=*0x%x, outlen=0x%x)", dev_handle, cmd, in, inlen, out, outlen);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	return CELL_OK;
}

error_code sys_storage_async_configure(u32 fd, u32 io_buf, u32 equeue_id, u32 unk)
{
	sys_storage.todo("sys_storage_async_configure(fd=0x%x, io_buf=0x%x, equeue_id=0x%x, unk=*0x%x)", fd, io_buf, equeue_id, unk);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	const auto handle = idm::get_unlocked<lv2_obj, lv2_storage>(fd);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	if (auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_id))
	{
		handle->async_port.store(queue);
	}
	else
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_storage_async_send_device_command(u32 dev_handle, u64 cmd, vm::ptr<void> in, u64 inlen, vm::ptr<void> out, u64 outlen, u64 operation_name)
{
	sys_storage.todo("sys_storage_async_send_device_command(dev_handle=0x%x, cmd=0x%llx, in=*0x%x, inlen=0x%x, out=*0x%x, outlen=0x%x, operation_name=0x%x)", dev_handle, cmd, in, inlen, out, outlen, operation_name);
	sys_storage.todo("sys_storage_async_send_device_command(): BUF: %s", std::span<u8>(vm::get_super_ptr(in.addr()), inlen));
	log_callback(*cpu_thread::get_current<ppu_thread>());

	auto& manager = g_fxo->get<storage_manager>();

	const auto handle = idm::get_unlocked<lv2_obj, lv2_storage>(dev_handle);

	if (!handle)
	{
		return CELL_ESRCH;
	}

	std::vector<u8> data_in(inlen);

	if (inlen && !vm::try_access(in.addr(), data_in.data(), inlen, false))
	{
		fmt::throw_exception("Failed to read input data!");
	}

	std::memset(out.get_ptr(), 0, outlen);

	struct input_output
	{
		u32 ID; // So we can identify the command later and fill response_base accordingly
		std::vector<u8> data_in; // Data input (masked)
		std::vector<u8> data_mask; // Input mask
		std::vector<u8> response_base; // Dat to be memcpy'ed to output, prior to possibly doing more modifications
		u64 response_event_data2; // Event data sent
		u64 response_event_data3; // Event data sent
	};

	static const std::vector<input_output> inputs_outputs
	{
		input_output
		{
			1, 
			std::vector<u8> // input
			{
				0x51, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x22, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x0C,
				0x00, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x23,
				0x00, 0x00, 0x00, 0x03,
				0x00, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x00
			},
			std::vector<u8> // input mask
			{
				0xFF, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0xFF, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0x00
			},
			std::vector<u8> // output base
			{
				0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00
			},
			0x8000000002050000, 0,
		},

		input_output
		{
			2, 
			std::vector<u8> // input
			{
				0xA4, 0x00, 0x00, 0x00,
		 		0x00, 0x00, 0x00, 0xE0,
		 		0x00, 0x08, 0x03, 0x00,
		 		0x00, 0x00, 0x00, 0x00,
		 		0x00, 0x00, 0x00, 0x00,
		 		0x00, 0x00, 0x00, 0x00,
		 		0x00, 0x00, 0x00, 0x00,
		 		0x00, 0x00, 0x00, 0x00,
		 		0x00, 0x00, 0x00, 0x0C,
		 		0x00, 0x00, 0x00, 0x01,
		 		0x00, 0x00, 0x00, 0x08,
		 		0x00, 0x00, 0x00, 0x03,
		 		0x00, 0x00, 0x00, 0x01,
		 		0x00, 0x00, 0x00, 0x00
			},
			std::vector<u8> // input mask
			{
				0xFF, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0xFF, 0xFF, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0x00
			},
			std::vector<u8> // output base
			{
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			},
			0, 0,
		},

		input_output
		{
			3,
			std::vector<u8> // input
			{
				0xAD, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x73, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x0C,
				0x00, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x73,
				0x00, 0x00, 0x00, 0x03,
				0x00, 0x00, 0x00, 0x01,
				0x00, 0x00, 0x00, 0x00
			},
			std::vector<u8> // input mask
			{
				0xFF, 0xFF, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0xFF, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,//
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0xFF,
				0x00, 0x00, 0x00, 0x00
			},
			std::vector<u8> // outout base
			{
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			    0x00, 0x00, 0x00,
			},
			0, 0
		}
	};

	std::vector<u8> data_for_comparison;

	u32 found_ID = umax;
	u64 response_event_data2 = 0;
	u64 response_event_data3 = 0;

	for (const input_output& info : inputs_outputs)
	{
		if (data_in.size() != info.data_in.size())
		{
			continue;
		}

		data_for_comparison.resize(data_in.size());
		std::memcpy(data_for_comparison.data(), data_in.data(), data_in.size());

		for (u32 index = 0; index < data_for_comparison.size(); index++)
		{
			data_for_comparison[index] &= info.data_mask[index];
		}

		if (data_for_comparison == data_in)
		{
			if (info.response_base.size() != outlen)
			{
				fmt::throw_exception("Misidentification of input data type! ID=x%d (response size: %d)", info.ID, info.response_base.size());
			}

			found_ID = info.ID;
			response_event_data2 = info.response_event_data2;
			response_event_data3 = info.response_event_data3;
			ensure(vm::try_access(out.addr(), const_cast<u8*>(info.response_base.data()), outlen, true));
		}
	}

	if (auto q = handle->async_port.load())
	{
		q->send(0, operation_name, response_event_data2, response_event_data3);
	}

	return CELL_OK;
}

error_code sys_storage_async_read()
{
	sys_storage.todo("sys_storage_async_read()");

	return CELL_OK;
}

error_code sys_storage_async_write()
{
	sys_storage.todo("sys_storage_async_write()");

	return CELL_OK;
}

error_code sys_storage_async_cancel()
{
	sys_storage.todo("sys_storage_async_cancel()");

	return CELL_OK;
}

error_code sys_storage_get_device_info(u64 device, vm::ptr<StorageDeviceInfo> buffer)
{
	sys_storage.todo("sys_storage_get_device_info(device=0x%x, buffer=*0x%x)", device, buffer);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	if (!buffer)
	{
		return CELL_EFAULT;
	}

	memset(buffer.get_ptr(), 0, sizeof(StorageDeviceInfo));

	u64 storage = device & 0xFFFFF00FFFFFFFF;
	u32 dev_num = (device >> 32) & 0xFF;

	if (storage == ATA_HDD) // dev_hdd?
	{
		if (dev_num > 2)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->one1 = 1;
		buffer->one2 = 1;
		buffer->flag5 = 1;

		// set partition size based on dev_num
		// stole these sizes from kernel dump, unknown if they are 100% correct
		// vsh reports only 2 partitions even though there is 3 sizes
		switch (dev_num)
		{
		case 0:
			buffer->sector_count = 0x2542EAB0; // possibly total size
			break;
		case 1:
			buffer->sector_count = 0x24FAEA98; // which makes this hdd0
			break;
		case 2:
			buffer->sector_count = 0x3FFFF8; // and this one hdd1
			break;
		}
	}
	else if (storage == BDVD_DRIVE) //	dev_bdvd?
	{
		if (dev_num > 0)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());

		const bool connected = true;
		if (!connected)
		{
			buffer->sector_count = 0;
			buffer->sector_size = 0x7FFFFFFF;
		}
		else
		{
			buffer->sector_count = 0x1EC4B00;
			buffer->sector_size = 0x800;
		}
// [000] | 75 6E 6E 61 | 6D 65 64 00 |
// [008] | 00 00 00 00 | 00 00 00 00 |
// [010] | 00 00 00 00 | 00 00 00 00 |
// [018] | 00 00 00 00 | 00 00 00 00 |
// [020] | 00 00 00 00 | 00 00 00 00 |
// [028] | 00 00 00 00 | 01 EC 4B 00 |
// [030] | 00 00 02 00 | 00 00 00 01 |
// [038] | 01 01 01 00 | 01 01 00 01 |



// [000] | 75 6E 6E 61 | 6D 65 64 00 |
// [008] | 00 00 00 00 | 00 00 00 00 |
// [010] | 00 00 00 00 | 00 00 00 00 |
// [018] | 00 00 00 00 | 00 00 00 00 |
// [020] | 00 00 00 00 | 00 00 00 00 |
// [028] | 00 00 00 00 | 00 62 83 E0 |
// [030] | 00 00 08 00 | 00 00 00 01 |
// [038] | 00 01 01 00 | 00 00 00 01 |

		buffer->one = 1;
		buffer->connected = connected;
		buffer->one1 = 1;
		buffer->one2 = 1;
		buffer->flag3 = 0;
		//buffer->flags4 = 0;
		buffer->flag5 = 1;

		static const unsigned char dump2[0x40] =
		{
			0x75, 0x6E, 0x6E, 0x61, 0x6D, 0x65, 0x64, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x83, 0xE0,
			0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01,
			0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01
		};

		static const unsigned char data_mode_8[0x40] =
		{
		    0x75, 0x6E, 0x6E, 0x61, 0x6D, 0x65, 0x64, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x83, 0xE0,
		    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x01,
		    0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01
		};


		std::memcpy(&*buffer, data_mode_8, sizeof(data_mode_8));
	//	buffer->sector_size = 0x200;
	}
	else if (storage == USB_MASS_STORAGE_1(0))
	{
		if (dev_num > 0)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		/*buffer->sector_count = 0x4D955;*/
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->one1 = 1;
		buffer->one2 = 1;
		buffer->flag5 = 1;
	}
	else if (storage == NAND_FLASH)
	{
		if (dev_num > 6)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->one1 = 1;
		buffer->one2 = 1;
		buffer->flag5 = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x80000;
			break;
		case 1: buffer->sector_count = 0x75F8;
			break;
		case 2: buffer->sector_count = 0x63E00;
			break;
		case 3: buffer->sector_count = 0x8000;
			break;
		case 4: buffer->sector_count = 0x400;
			break;
		case 5: buffer->sector_count = 0x2000;
			break;
		case 6: buffer->sector_count = 0x200;
			break;
		}
	}
	else if (storage == NOR_FLASH)
	{
		if (dev_num > 3)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x200;
		buffer->one = 1;
		buffer->one1 = 0;
		buffer->one2 = 1;
		buffer->flag5 = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x8000;
			break;
		case 1: buffer->sector_count = 0x77F8;
			break;
		case 2: buffer->sector_count = 0x100; // offset, 0x20000
			break;
		case 3: buffer->sector_count = 0x400;
			break;
		}
	}
	else if (storage == NAND_UNK)
	{
		if (dev_num > 1)
		{
			return not_an_error(-5);
		}

		std::string u = "unnamed";
		memcpy(buffer->name, u.c_str(), u.size());
		buffer->sector_size = 0x800;
		buffer->one = 1;
		buffer->one1 = 0;
		buffer->one2 = 1;
		buffer->flag5 = 1;

		// see ata_hdd for explanation
		switch (dev_num)
		{
		case 0: buffer->sector_count = 0x7FFFFFFF;
			break;
		}
	}
	else
	{
		sys_storage.error("sys_storage_get_device_info(device=0x%x, buffer=*0x%x)", device, buffer);
	}

	return CELL_OK;
}

error_code sys_storage_get_device_config(vm::ptr<u32> storages, vm::ptr<u32> devices)
{
	sys_storage.todo("sys_storage_get_device_config(storages=*0x%x, devices=*0x%x)", storages, devices);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	if (storages) *storages = 6; else return CELL_EFAULT;
	if (devices)  *devices = 17; else return CELL_EFAULT;

	return CELL_OK;
}

error_code sys_storage_report_devices(u32 storages, u32 start, u32 devices, vm::ptr<u64> device_ids)
{
	sys_storage.todo("sys_storage_report_devices(storages=0x%x, start=0x%x, devices=0x%x, device_ids=0x%x)", storages, start, devices, device_ids);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	if (!device_ids)
	{
		return CELL_EFAULT;
	}

	if (storages != 6)
	{
		return -5;
	}

	static constexpr std::array<u64, 0x11> all_devs = []
	{
		std::array<u64, 0x11> all_devs{};
		all_devs[0] = 0x10300000000000A;

		for (int i = 0; i < 7; ++i)
		{
			all_devs[i + 1] = 0x100000000000001 | (static_cast<u64>(i) << 32);
		}

		for (int i = 0; i < 3; ++i)
		{
			all_devs[i + 8] = 0x101000000000007 | (static_cast<u64>(i) << 32);
		}

		all_devs[11] = 0x101000000000006;

		for (int i = 0; i < 4; ++i)
		{
			all_devs[i + 12] = 0x100000000000004 | (static_cast<u64>(i) << 32);
		}

		all_devs[16] = 0x100000000000003;
		return all_devs;
	}();

	if (!devices || start >= all_devs.size() || devices > all_devs.size() - start)
	{
		return CELL_EINVAL;
	}

	std::copy_n(all_devs.begin() + start, devices, device_ids.get_ptr());

	return CELL_OK;
}

error_code sys_storage_configure_medium_event(ppu_thread& ppu, u32 fd, u32 equeue_id, vm::ptr<u32> handle)
{
	sys_storage.todo("sys_storage_configure_medium_event(fd=0x%x, equeue_id=0x%x, c=0x%x)", fd, equeue_id, handle);
	log_callback(*cpu_thread::get_current<ppu_thread>());

	if (!ppu.has_root_perm)
	{
		return CELL_EPERM;
	}

	u64 device_id = 0; // 0 means global

	if (fd)
	{
		const auto storage = idm::get_unlocked<lv2_obj, lv2_storage>(fd);

		if (!storage)
		{
			return {CELL_ESRCH, "storage"};
		}

		// Oddly that is all it needs from the storage descriptor
		// It closes the handle right after device ID extraction
		// Perhaps because not calling sys_storage_open's routines saves on expensive "error checking" the device ID?
		device_id = storage->device_id;
	}

	auto& manager = *ensure(g_fxo->try_get<storage_manager>());

	if (auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(equeue_id))
	{
		while (!idm::make_ptr<lv2_storage_medium_event_port>(device_id, queue))
		{
			std::vector<std::pair<u32, u32>> cleanup_list;

			// Try cleanup
			id_manager::g_process = 0;
			idm::select<lv2_storage_medium_event_port>([&](u32 id, u32 proc, lv2_storage_medium_event_port& port)
			{
				// Check port status
				if (!port.savable())
				{
					// Detached ports can be removed
					cleanup_list.emplace_back(id, proc);
				}
			});

			bool success = false;

			for (auto [id, proc] : cleanup_list)
			{
				success = idm::remove<lv2_storage_medium_event_port>(idm::id_index(id, proc));
			}

			id_manager::g_process = ppu.proc_id;

			if (!success)
			{
				fmt::throw_exception("lv2_storage_medium_event_port() entries depletion, consider increases lv2_storage_medium_event_port::id_count!");
			}
		}
	}
	else
	{
		return CELL_ESRCH;
	}

	// No idea what this means, seems like it returns uninitialized memory
	*handle = 0x5D7280;
	return CELL_OK;
}

error_code sys_storage_set_medium_polling_interval(ppu_thread& ppu, u32 fd, u64 interval)
{
	sys_storage.todo("sys_storage_set_medium_polling_interval()");

	return CELL_OK;
}

error_code sys_storage_create_region()
{
	sys_storage.todo("sys_storage_create_region()");

	return CELL_OK;
}

error_code sys_storage_delete_region()
{
	sys_storage.todo("sys_storage_delete_region()");

	return CELL_OK;
}

error_code sys_storage_execute_device_command(u32 fd, u64 cmd, vm::ptr<char> cmdbuf, u64 cmdbuf_size, vm::ptr<char> databuf, u64 databuf_size, vm::ptr<u32> driver_status)
{
	sys_storage.todo("sys_storage_execute_device_command(fd=0x%x, cmd=0x%llx, cmdbuf=*0x%x, cmdbuf_size=0x%llx, databuf=*0x%x, databuf_size=0x%llx, driver_status=*0x%x)", fd, cmd, cmdbuf, cmdbuf_size, databuf, databuf_size, driver_status);

	// cmd == 2 is get device info,
	// databuf, first byte 0 == status ok?
	// byte 1, if < 0 , not ata device
	if (cmd == 2 && databuf && databuf_size >= 2)
	{
		databuf[0] = 0; // status ok
		databuf[1] = 0; // is ata device
	}

	if (driver_status)
	{
		*driver_status = 0;
	}

	return CELL_OK;
}

error_code sys_storage_check_region_acl()
{
	sys_storage.todo("sys_storage_check_region_acl()");

	return CELL_OK;
}

error_code sys_storage_set_region_acl()
{
	sys_storage.todo("sys_storage_set_region_acl()");

	return CELL_OK;
}

error_code sys_storage_get_region_offset()
{
	sys_storage.todo("sys_storage_get_region_offset()");

	return CELL_OK;
}

error_code sys_storage_set_emulated_speed()
{
	sys_storage.todo("sys_storage_set_emulated_speed()");

	// todo: only debug kernel has this
	return CELL_ENOSYS;
}
