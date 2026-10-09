#include "stdafx.h"

#include "Emu/Cell/PPUThread.h"

#include "sys_usbbtaudio.h"

LOG_CHANNEL(sys_usbbtaudio);

error_code sys_usbbtaudio_initialize(ppu_thread& ppu, u32 equeue_id, vm::ptr<u32> out_shm_addr, u32 arg3, u32 arg4, u32 arg5)
{
	ppu.state += cpu_flag::wait;

	sys_usbbtaudio.todo("sys_usbbtaudio_initialize(equeue_id=0x%x, out_shm_addr=*0x%x, arg3=0x%x, arg4=0x%x, arg5=0x%x)", equeue_id, out_shm_addr, arg3, arg4, arg5);

	// EXPERIMENT: this was a no-op stub (always CELL_OK, nothing written back). UsbBtAudioProxy
	// stores whatever we hand back here as m_pShm and crashes with a null-pointer assertion the
	// moment its event loop thread starts, since nothing ever set it. Guessing out_shm_addr (r4)
	// is where the shared memory address belongs, based on register proximity in the syscall log.
	if (out_shm_addr)
	{
		if (const u32 addr = vm::alloc(0x1000, vm::main))
		{
			*out_shm_addr = addr;
		}
	}

	return CELL_OK;
}
