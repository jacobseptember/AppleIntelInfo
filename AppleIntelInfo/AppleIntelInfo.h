/*
 * Copyright (c) 2012-2015 Pike R. Alpha. All rights reserved.
 *
 * Original idea and initial development of MSRDumper.kext (c) 2011 by † RevoGirl.
 *
 * A big thank you to George for his help and continuation of Sam's work, but it
 * was time for me to push the envelope and add some really interesting stuff.
 *
 * This work is licensed under the Creative Commons Attribution-NonCommercial
 * 4.0 Unported License => http://creativecommons.org/licenses/by-nc/4.0
 */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-register"
#include <IOKit/IOLib.h>
#pragma clang diagnostic pop

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>

#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <i386/cpuid.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-register"
#include <i386/proc_reg.h>
#pragma clang diagnostic pop

#include <essentials.h>

#define super IOService

#define VERSION					"1.2"

#define REPORT_MSRS				1
#define REPORT_IGPU_P_STATES	1
#define REPORT_C_STATES			1
#define REPORT_IPG_STYLE		1
#define REPORT_INTEL_REGS		1

#define MMIO_READ8(Address)			(*(volatile UInt8  *)(Address))
#define MMIO_READ16(Address)		(*(volatile UInt16 *)(Address))
#define MMIO_READ32(Address)		(*(volatile UInt32 *)(gMMIOAddress + Address))

#define NB_BUS	0x00
#define NB_DEV	0x00
#define NB_FUN	0x00

#define DEVEN	(0x54)
#define DEVEN_D2EN_MASK	(0x10)

#define NB_PCICFG_SPACE_INDEX_REG	0xcf8
#define NB_PCICFG_SPACE_DATA_REG	0xcfc

#define BIT31						0x80000000

#define PCIEX_BASE_ADDRESS			0xF8000000
#define NB_MCH_BASE_ADDRESS			0xFED10000	// (G)MCH Memory Mapped Register Range Base Address (D0:F0:Rx48).

#define READ_PCI8(Bx, Dx, Fx, Rx)	ReadPci8(Bx, Dx, Fx, Rx)
#define READ_PCI8_NB(Rx)			READ_PCI8(NB_BUS, NB_DEV, NB_FUN, Rx)

#define IGPU_RATIO_TO_FREQUENCY(ratio)	((ratio * 100) / 2)

#define NB_PCI_CFG_ADDRESS(bus, dev, func, reg) \
(UInt64) ((((UInt8)(bus) << 24) + ((UInt8)(dev) << 16) + \
((UInt8)(func) << 8) + ((UInt8)(reg))) & 0xffffffff)

#define NB_PCIE_CFG_ADDRESS(bus, dev, func, reg) \
((UInt32)(PCIEX_BASE_ADDRESS + ((UInt8)(bus) << 20) + \
((UInt8)(dev) << 15) + ((UInt8)(func) << 12) + (reg)))

#define	FILE_PATH "/tmp/AppleIntelInfo.dat"

#define TEMP_BUFFER_SIZE	256
#define WRITE_BUFFER_SIZE	1024

int tempBufferLength = 0;

#define IOLOG(fmt, args...)								\
memset(logBuffer, 0, TEMP_BUFFER_SIZE);				\
snprintf(logBuffer, TEMP_BUFFER_SIZE, fmt, ##args);	\
writeReport();


//------------------------------------------------------------------------------

static __inline__ void outl(UInt16 port, UInt32 value)
{
	__asm__ volatile("outl %0, %w1" : : "a" (value), "Nd" (port));
}

//------------------------------------------------------------------------------

static __inline__ unsigned char inb(UInt16 port)
{
	UInt8 value;
	__asm__ volatile("inb %w1, %b0" : "=a" (value) : "Nd" (port));
	return (value);
}

//------------------------------------------------------------------------------

static __inline__ unsigned int inl(UInt16 port)
{
	UInt32 value;
	__asm__ volatile("inl %w1, %0" : "=a" (value) : "Nd" (port));
	return (value);
}

//------------------------------------------------------------------------------

UInt8 ReadPci8(UInt8 Bus, UInt8 Dev, UInt8 Fun, UInt16 Reg)
{
	if (Reg >= 0x100)
	{
		return MMIO_READ8((UInt64)NB_PCIE_CFG_ADDRESS(Bus, Dev, Fun, Reg));
	}
	else
	{
		outl(NB_PCICFG_SPACE_INDEX_REG, BIT31 | (Bus << 16) | (Dev << 11) | (Fun << 8) | (Reg & 0xfc));
		return inb(NB_PCICFG_SPACE_DATA_REG | (UInt8)(Reg & 3));
	}
}

extern "C" void mp_rendezvous_no_intrs(void (*action_func)(void *), void * arg);
extern "C" int cpu_number(void);

//------------------------------------------------------------------------------

class AppleIntelInfo : public IOService
{
	OSDeclareDefaultStructors (AppleIntelInfo);
	
private:
	IOSimpleLock		*simpleLock;
	IOWorkLoop			*workLoop;
	IOTimerEventSource	*timerEventSource;
	
#if REPORT_IGPU_P_STATES
	IOMemoryDescriptor	*memDescriptor;
	IOMemoryMap			*memoryMap;

	bool igpuEnabled	= false;	// Set <key>logIGPU</key> to <true/> in Info.plist to enable this feature.
#endif
	
	IOReturn result		= kIOReturnSuccess;
	
	virtual IOReturn loopTimerEvent(void);

	int writeReport(void);

#if REPORT_MSRS
	void reportMSRs(UInt8 aCPUModel);

	bool logMSRs		= true;		// Set <key>logIGPU</key> to <false/> in Info.plist to disable this feature.
#endif

	bool loopLock		= false;

#if REPORT_C_STATES
	bool logCStates		= true;		//  Set <key>logCStates</key> to <false/> in Info.plist to disable this feature.
#endif

#if REPORT_IPG_STYLE
	bool logIPGStyle	= true;		//  Set <key>logIPGStyle</key> to <false/> in Info.plist to disable this feature.
#endif

#if REPORT_INTEL_REGS
	bool logIntelRegs	= true;		//  Set <key>logIntelRegs</key> to <false/> in Info.plist to disable this feature.

	#define DEBUGSTRING(func) void func(char *result, int len, UInt32 reg, UInt32 val)
	#define DEFINEREG2(reg, func) { reg, #reg, func, 0 }

	#define DEFINE_FUNC_VOID(func) void func(void)
	#define DEFINE_FUNC_DUMP(func) void func(struct reg_debug *regs, uint32_t count)
	
	void intel_dump_other_regs(void);
	void dumpRegisters(struct reg_debug *regs, uint32_t count);
	void getPCHDeviceID(void);
	void reportIntelRegs(void);
#endif

	UInt16 Interval = 50;
	
	UInt64	gCoreMultipliers		= 0ULL;
	UInt64	gTriggeredPStates		= 0ULL;
	
	UInt64	gIGPUMultipliers		= 0ULL;
	UInt64	gTriggeredIGPUPStates	= 0ULL;
	
	vfs_context_t mCtx				= NULL;
	long reportFileOffset			= 0L;

	char tempBuffer[TEMP_BUFFER_SIZE];
	char logBuffer[WRITE_BUFFER_SIZE];

public:
	virtual IOService *	probe(IOService * provider, SInt32 * score) override;
	virtual bool start(IOService * provider) override;
	virtual void stop(IOService * provider) override;
	virtual void free(void) override;
	
	UInt8	gMinRatio	= 0;
	UInt8	gClockRatio	= 0;
	UInt8	gMaxRatio	= 0;
};

OSDefineMetaClassAndStructors(AppleIntelInfo, IOService)

UInt8	gCoreStates	= 0ULL;

#if REPORT_C_STATES
	bool	gCheckC3	= true;
	bool	gCheckC6	= true;
	bool	gCheckC7	= false;

	UInt32	gC3Cores	= 0;
	UInt32	gC6Cores	= 0;
	UInt32	gC7Cores	= 0;

	UInt32	gTriggeredC3Cores	= 0;
	UInt32	gTriggeredC6Cores	= 0;
	UInt32	gTriggeredC7Cores	= 0;
#endif

UInt64	gCoreMultipliers = 0ULL;

uint64_t gTSC = 0;

#if REPORT_IGPU_P_STATES
UInt8	* gMchbar	= NULL;
#endif

#if REPORT_INTEL_REGS
	#include "../AppleIntelRegisterDumper/AppleIntelRegisterDumper.h"
#endif
