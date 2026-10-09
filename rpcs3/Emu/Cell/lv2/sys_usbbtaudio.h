#pragma once

#include "Emu/Memory/vm_ptr.h"
#include "Emu/Cell/ErrorCodes.h"

class ppu_thread;

// SysCalls

error_code sys_usbbtaudio_initialize(ppu_thread& ppu, u32 equeue_id, vm::ptr<u32> out_shm_addr, u32 arg3, u32 arg4, u32 arg5);
