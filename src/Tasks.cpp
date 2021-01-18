/*
 * Startup.cpp
 *
 *  Created on: 26 Mar 2018
 *      Author: David
 */

#include "Tasks.h"
#include "RepRap.h"
#include "Platform.h"
#include <Cache.h>
#include <TaskPriorities.h>
#include <Hardware/SoftwareReset.h>
#include <Storage/CRC32.h>

#if SAM4E || SAM4S || SAME70
# include <efc/efc.h>		// for efc_enable_cloe()
#endif

#if SAME5x
# include <DmacManager.h>
# include <hpl_user_area.h>
#endif

#include "FreeRTOS.h"
#include "task.h"
#include <malloc.h>

const uint8_t memPattern = 0xA5;		// this must be the same pattern as FreeRTOS because we use common code for checking for stack overflow

extern char _end;						// defined in linker script
extern char _estack;					// defined in linker script

// Define replacement standard library functions
#include <syscalls.h>

#ifndef DEBUG
extern uint32_t _firmware_crc;			// defined in linker script
#endif

// MAIN task data
// The main task currently runs GCodes, so it needs to be large enough to hold the matrices used for delta auto calibration.
// The worst case stack usage is after running delta auto calibration with Move debugging enabled.
// The timer and idle tasks currently never do I/O, so they can be much smaller.
#if SAME70
constexpr unsigned int MainTaskStackWords = 1800;			// on the SAME70 we use matrices of doubles
#elif defined(__LPC17xx__)
constexpr unsigned int MainTaskStackWords = 1110-(16*9);	// LPC builds only support 16 calibration points, so less space needed
#else
constexpr unsigned int MainTaskStackWords = 1110;			// on other processors we use matrixes of floats
#endif

static Task<MainTaskStackWords> mainTask;
extern "C" [[noreturn]] void MainTask(void * pvParameters) noexcept;

// Idle task data
constexpr unsigned int IdleTaskStackWords = 40;				// currently we don't use the idle talk for anything, so this can be quite small
static Task<IdleTaskStackWords> idleTask;

extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize) noexcept
{
	*ppxIdleTaskTCBBuffer = idleTask.GetTaskMemory();
	*ppxIdleTaskStackBuffer = idleTask.GetStackBase();
	*pulIdleTaskStackSize = idleTask.GetStackSize();
}

#if configUSE_TIMERS

// Timer task data
constexpr unsigned int TimerTaskStackWords = 60;
static Task<TimerTaskStackWords> timerTask;

extern "C" void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer, StackType_t **ppxTimerTaskStackBuffer, uint32_t *pulTimerTaskStackSize) noexcept
{
    *ppxTimerTaskTCBBuffer = timerTask.GetTaskMemory();
    *ppxTimerTaskStackBuffer = timerTask.GetStackBase();
    *pulTimerTaskStackSize = timerTask.GetStackSize();
}

#endif

// Mutexes
static Mutex i2cMutex;
static Mutex mallocMutex;
static Mutex filamentsMutex;

// We need to make malloc/free thread safe. We must use a recursive mutex for it.
extern "C" void GetMallocMutex() noexcept
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)		// don't take mutex if scheduler not started or suspended
	{
		mallocMutex.Take();
	}
}

extern "C" void ReleaseMallocMutex() noexcept
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)		// don't release mutex if scheduler not started or suspended
	{
		mallocMutex.Release();
	}
}

// Application entry point
#if SAME5x		// if using CoreN2G
[[noreturn]] void AppMain() noexcept
#else			// using CoreNG
extern "C" [[noreturn]] void AppMain() noexcept
#endif
{
	pinMode(DiagPin, (DiagOnPolarity) ? OUTPUT_LOW : OUTPUT_HIGH);	// set up diag LED for debugging and turn it off

#if !defined(DEBUG) && !defined(__LPC17xx__)	// don't check the CRC of a debug build because debugger breakpoints mess up the CRC
	// Check the integrity of the firmware by checking the firmware CRC
	{
		const char *firmwareStart = reinterpret_cast<const char*>(SCB->VTOR & 0xFFFFFF80);
		CRC32 crc;
		crc.Update(firmwareStart, (const char*)&_firmware_crc - firmwareStart);
		if (crc.Get() != _firmware_crc)
		{
			// CRC failed so flash the diagnostic LED 3 times, pause and repeat. This is the same error code used by the Duet 3 expansion boards bootloader.
			for (unsigned int i = 0; ; ++i)
			{
				const bool on = (i & 1) == 0 && (i & 15) < 6;				// turn LED on if count is 0, 2, 4 or 16, 18, 20 etc. otherwise turn it off
				digitalWrite(DiagPin, XNor(on, DiagOnPolarity));
				for (unsigned int j = 0; j < 500; ++j)
				{
					delayMicroseconds(1000);								// delayMicroseconds only works with low values of delay so do 1ms at a time
				}
			}
		}
	}
#endif	// !defined(DEBUG) && !defined(__LPC17xx__)

	// Fill the free memory with a pattern so that we can check for stack usage and memory corruption
	char* heapend = heapTop;
	register const char * stack_ptr asm ("sp");
	while (heapend + 16 < stack_ptr)
	{
		*heapend++ = memPattern;
	}

#if SAME5x
	{
		const uint32_t bootloaderSize = SCB->VTOR & 0xFFFFFF80;
		if (bootloaderSize == 0x4000)
		{
			// Looks like this is release firmware that was loaded by a bootloader in the first 16Kb of flash
			// Check that the bootloader is protected and EEPROM is configured
			uint64_t nvmUserRow0 = *reinterpret_cast<const uint64_t*>(NVMCTRL_USER);						// we only need values in the first 64 bits of the user area
			constexpr uint64_t mask =     ((uint64_t)0x0F << 32) | ((uint64_t)0x07 << 36) | (0x0F << 26);	// we just want NVM_BOOT (bits 26-29), SEE.SBLK (bits 32-35) and SEE.PSZ (bits 36:38)
			constexpr uint64_t reqValue = ((uint64_t)0x01 << 32) | ((uint64_t)0x03 << 36) | (13 << 26);		// 4K SMART EEPROM and 16K bootloader (SBLK=1 PSZ=3 BOOTPROT=13)

			if ((nvmUserRow0 & mask) != reqValue)
			{
				nvmUserRow0 = (nvmUserRow0 & ~mask) | reqValue;												// set up the required value
				_user_area_write(reinterpret_cast<void*>(NVMCTRL_USER), 0, reinterpret_cast<const uint8_t*>(&nvmUserRow0), sizeof(nvmUserRow0));

				// If we reset immediately then the user area write doesn't complete and the bits get set to all 1s.
				delayMicroseconds(10000);
				Reset();
			}
		}
	}

	CoreInit();
	DeviceInit();
#endif

	// Trap integer divide-by-zero.
	// We could also trap unaligned memory access, if we change the gcc options to not generate code that uses unaligned memory access.
	SCB->CCR |= SCB_CCR_DIV_0_TRP_Msk;

#if !defined(__LPC17xx__) && !SAME5x
	// When doing a software reset, we disable the NRST input (User reset) to prevent the negative-going pulse that gets generated on it being held
	// in the capacitor and changing the reset reason from Software to User. So enable it again here. We hope that the reset signal will have gone away by now.
# ifndef RSTC_MR_KEY_PASSWD
// Definition of RSTC_MR_KEY_PASSWD is missing in the SAM3X ASF files
#  define RSTC_MR_KEY_PASSWD (0xA5u << 24)
# endif
	RSTC->RSTC_MR = RSTC_MR_KEY_PASSWD | RSTC_MR_URSTEN;
#endif

	Cache::Init();					// initialise the cache and/or the MPU, if applicable to this processor
	Cache::Enable();

#if SAM4S
	efc_enable_cloe(EFC0);			// enable code loop optimisation
#elif SAM4E || SAME70
	efc_enable_cloe(EFC);			// enable code loop optimisation
#endif

	idleTask.AddToList();			// add the FreeRTOS internal tasks to the task list

#if configUSE_TIMERS
	timerTask.AddToList();
#endif

	// Create the mutexes and the startup task
	mallocMutex.Create("Malloc");
	i2cMutex.Create("I2C");
	filamentsMutex.Create("Filaments");
	mainTask.Create(MainTask, "MAIN", nullptr, TaskPriority::SpinPriority);

	vTaskStartScheduler();			// doesn't return
	for (;;) { }					// keep gcc happy
}

extern "C" [[noreturn]] void MainTask(void *pvParameters) noexcept
{
	reprap.Init();
	for (;;)
	{
		reprap.Spin();
	}
}

#ifdef __LPC17xx__
	extern "C" size_t xPortGetTotalHeapSize( void );
#endif

// Return the amount of free handler stack space. It may be negative if the stack has overflowed into the area reserved for the heap.
static ptrdiff_t GetHandlerFreeStack() noexcept
{
	const char * const ramend = &_estack;
	const char * stack_lwm = sysStackLimit;
	while (stack_lwm < ramend && *stack_lwm == memPattern)
	{
		++stack_lwm;
	}
	return stack_lwm - sysStackLimit;
}

ptrdiff_t Tasks::GetNeverUsedRam() noexcept
{
	return heapLimit - heapTop;
}

const char* Tasks::GetHeapTop() noexcept
{
	return heapTop;
}

// Allocate memory permanently. Using this saves about 8 bytes per object. You must not call free() on the returned object.
// It doesn't try to allocate from the free list maintained by malloc, only from virgin memory.
void *Tasks::AllocPermanent(size_t sz, std::align_val_t align) noexcept
{
	GetMallocMutex();
	void * const ret = CoreAllocPermanent(sz, align);
	ReleaseMallocMutex();
	return ret;
}

// Write data about the current task
void Tasks::Diagnostics(MessageType mtype) noexcept
{
	Platform& p = reprap.GetPlatform();
	p.Message(mtype, "=== RTOS ===\n");
	// Print memory stats
	{
		const char * const ramstart =
#if SAME5x
			(char *) HSRAM_ADDR;
#elif defined(__LPC17xx__)
			(char *) 0x10000000;
#else
			(char *) IRAM_ADDR;
#endif
		p.MessageF(mtype, "Static ram: %d\n", &_end - ramstart);

#ifdef __LPC17xx__
		p.MessageF(mtype, "Dynamic Memory (RTOS Heap 5): %d free, %d never used\n", xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize() );
#else
		const struct mallinfo mi = mallinfo();
		p.MessageF(mtype, "Dynamic ram: %d of which %d recycled\n", mi.uordblks, mi.fordblks);
#endif
		p.MessageF(mtype, "Never used RAM %d, free system stack %d words\n", GetNeverUsedRam(), GetHandlerFreeStack()/4);

	}	// end memory stats scope

	p.Message(mtype, "Tasks:");
	for (TaskBase *t = TaskBase::GetTaskList(); t != nullptr; t = t->GetNext())
	{
		TaskStatus_t taskDetails;
		vTaskGetInfo(t->GetFreeRTOSHandle(), &taskDetails, pdTRUE, eInvalid);
		const char* const stateText = (taskDetails.eCurrentState == eRunning) ? "running"
										: (taskDetails.eCurrentState == eReady) ? "ready"
											: (taskDetails.eCurrentState == eBlocked) ? "blocked"
												: (taskDetails.eCurrentState == eSuspended) ? "suspended"
													: "invalid";
		p.MessageF(mtype, " %s(%s,%u)", taskDetails.pcTaskName, stateText, (unsigned int)taskDetails.usStackHighWaterMark);
	}
	p.Message(mtype, "\nOwned mutexes:");

	for (const Mutex *m = Mutex::GetMutexList(); m != nullptr; m = m->GetNext())
	{
		const TaskHandle holder = m->GetHolder();
		if (holder != nullptr)
		{
			p.MessageF(mtype, " %s(%s)", m->GetName(), pcTaskGetName(holder->GetFreeRTOSHandle()));
		}
	}
	p.Message(mtype, "\n");
}

TaskHandle Tasks::GetMainTask() noexcept
{
	return &mainTask;
}

void Tasks::TerminateMainTask() noexcept
{
	mainTask.TerminateAndUnlink();
}

Mutex *Tasks::GetI2CMutex() noexcept
{
	return &i2cMutex;
}

Mutex *Tasks::GetFilamentsMutex() noexcept
{
	return &filamentsMutex;
}

// This intercepts the 1ms system tick
extern "C" void vApplicationTickHook() noexcept
{
	CoreSysTick();
	reprap.Tick();
}

// We don't need the time zone functionality. Declaring these saves 8Kb.
extern "C" void __tzset() noexcept { }
extern "C" void __tz_lock() noexcept { }
extern "C" void __tz_unlock() noexcept { }
extern "C" void _tzset_unlocked() noexcept { }

#if SUPPORT_CAN_EXPANSION

// Functions called by CanMessageBuffer in CANlib
void *MessageBufferAlloc(size_t sz, std::align_val_t align) noexcept
{
	return Tasks::AllocPermanent(sz, align);
}

void MessageBufferDelete(void *ptr, std::align_val_t align) noexcept { }

#endif

// End
