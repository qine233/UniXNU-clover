// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board-local instrument for the clover GOP display bring-up (round180).
//
// This is the clover narrowing of the sheng device repository's package/LegacyDisplayProbe.cpp. It owns every hook
// that drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp declares but does not define (18 of the 27 undefined
// _native_legacy_* symbols of that object; the other 9 -- the seven IOSurface getters, the original vsync sender and
// the original setDisplayRefreshProperties -- are absolute symbols emitted by this board's builder into
// original-bindings.S, each one re-verified byte for byte against out/clover/kernel/kernel.macho), plus the two
// entry points the base repository's kernel patches call: native_legacy_display_probe() from bsd_init() and
// native_legacy_display_late_probe() from bsd_autoconf().
//
// What differs from the reference implementation, and why:
//   * no boot_progress_* weak imports and no diagnostic anchor mapping. This board builds with CONFIG_XNU_BOOT_PROGRESS=n,
//     so that channel does not exist here. Observability is the kernel's own message buffer: every reading below is an
//     IOLog line prefixed "clover r180: ", which CloverSdhci's tee (round175/176) filters into the kind-22 logfs ring,
//     so a frame is read back with the host script instead of photographed.
//   * none of the twelve unrelated probes the reference late entry fans out to (keystore, cpufreq, GPU power, touch,
//     WiFi, ADSP, battery, audio, buttons, backlight, DPU, HID). Those are other boards' work items; here the late entry
//     does one thing: bring up the framebuffer.
//   * a fixed line budget, and per-event caps, because the ring holds 512 slots and is shared with the storage stack.
//   * the console descriptor's v_display word is recorded. The base driver claimed the main display only for the
//     literal 1; this board's copy accepts any nonzero value (see the comment in that file), and the reading here is
//     what proves the two agree on hardware.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSData.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#include <stdarg.h>
extern "C" kmod_info_t g_kernel_kmod_info;
// The two module static initializers, named by their translation units. OSMetaClass::preModLoad/postModLoad run them
// for us; the archive is staged with __mod_init_func removed, so nothing else calls them.
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" void native_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" IOService *native_gop_presenter_create(void);
extern "C" bool native_gop_gpu_offer(IOService *,void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
static IOService *pendingLegacyDisplay,*activeLegacyDisplay,*activeGOPPresenter,*activeGOPNub,*gpuHandoffPresenter;
// GPU hand-off counters the driver writes through these names.
volatile uint32_t native_legacy_gpu_fallbacks,native_legacy_gpu_last_status;
static char cloverRound[]="clover r180:";
// One bounded log channel: the kernel message buffer, which the storage driver's tee carries onto the card. The budget
// is per boot and per event class, never per frame.
static volatile uint32_t cloverLogLines;
enum {kCloverLogBudget=96};
static void cloverLog(const char *format,...) __attribute__((format(printf,1,2)));
static void cloverLog(const char *format,...) {
    if(cloverLogLines>=kCloverLogBudget)return;
    cloverLogLines=cloverLogLines+1;
    char line[320];
    va_list arguments;va_start(arguments,format);
    int length=vsnprintf(line,sizeof(line),format,arguments);
    va_end(arguments);
    if(length<0)return;
    IOLog("%s %s\n",cloverRound,line);
}
extern "C" {
volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status=-1;
volatile uint32_t native_legacy_init_result,native_legacy_start_result,native_legacy_probe_match;
volatile uint32_t native_legacy_parent_match,native_legacy_object_size,native_legacy_released;
volatile uint64_t native_legacy_object,native_legacy_nub;
volatile uint32_t native_legacy_guard_slot;
volatile uint64_t native_legacy_guard_object,native_legacy_guard_caller;
volatile int native_legacy_surface_status=-1;
volatile uint32_t native_legacy_surface_found;
// The console descriptor exactly as PE_current_console returned it, including the flag the base driver used to test.
volatile uint64_t native_legacy_video_base,native_legacy_video_bytes;
volatile uint64_t native_legacy_video_display;
volatile uint32_t native_legacy_video_width,native_legacy_video_height,native_legacy_video_depth,
    native_legacy_video_row_bytes,native_legacy_video_rotate,native_legacy_video_scale;
volatile uint64_t native_legacy_gop_map,native_legacy_gop_virtual,native_legacy_gop_physical,native_legacy_gop_bytes;
volatile uint32_t native_legacy_gop_width,native_legacy_gop_height;
volatile uint32_t native_legacy_registration_calls,native_legacy_properties_length;
volatile uint32_t native_legacy_dt_attached;
volatile int native_legacy_unit_scale_status=-1;
volatile uint32_t native_legacy_unit_scale_calls,native_legacy_chosen_scale,native_legacy_chosen_rotation;
volatile uint32_t native_legacy_pm_stage,native_legacy_pm_calls,native_legacy_pm_callbacks,native_legacy_pm_ordinal;
volatile int native_legacy_pm_status=-1;
volatile uint32_t native_legacy_swap_validation_calls,native_legacy_swap_validation_result,native_legacy_swap_validation_reason;
volatile uint64_t native_legacy_swap_request,native_legacy_swap_surface;
volatile uint32_t native_legacy_transfer_calls;
volatile int native_legacy_transfer_status=-1;
volatile uint64_t native_legacy_transfer_bytes,native_legacy_transfer_request;
volatile uint64_t native_legacy_retirement_calls,native_legacy_retired_generation,native_legacy_presented_generation,native_legacy_retired_frames;
volatile uint64_t native_legacy_retired_current,native_legacy_retired_pending;
// Swaps retired by the driver the moment the GPU client took the frame, and copies that then failed. The driver owns
// the storage (its own extern "C" block at the top of that file defines both words and writes them), so this
// translation unit only names them; a definition here would be a duplicate symbol in the forced-load archive.
extern "C" volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;
char native_legacy_properties[4096];
volatile uint32_t native_legacy_client_calls,native_legacy_client_type;
volatile int native_legacy_client_pid,native_legacy_client_status=-1;
volatile uint64_t native_legacy_client_task,native_legacy_client_object;
char native_legacy_client_class[64];
volatile uint32_t native_legacy_prepare_calls,native_legacy_prepare_reason,native_legacy_prepare_attempts;
static uint64_t probeNowUs(void) {uint64_t nanoseconds;absolutetime_to_nanoseconds(mach_absolute_time(),&nanoseconds);return nanoseconds/1000;}
// The driver asks before every scanout store whether the frame buffer is free to write. On the reference board a
// boot-progress writer may still own it; this board has no such writer (CONFIG_XNU_BOOT_PROGRESS=n), and the video
// console only draws at panic time, so there is nothing to wait for. The interrupt-context test is kept: it is the
// one condition under which the copy itself would be wrong, and it is a claim we can still make honestly.
bool native_legacy_prepare_scanout(void) {
    native_legacy_prepare_calls=native_legacy_prepare_calls+1;native_legacy_prepare_attempts=0;
    if(ml_at_interrupt_context()) {native_legacy_prepare_reason=1;return false;}
    if(!ml_get_interrupts_enabled()) {native_legacy_prepare_reason=2;return false;}
    native_legacy_prepare_reason=0;
    return true;
}
__attribute__((noinline)) void native_legacy_display_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_gop_mapping_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_client_checkpoint(void) {__asm__ volatile("" ::: "memory");}
__attribute__((noinline)) void native_legacy_swap_validation_checkpoint(bool,unsigned,const void *,const void *) {__asm__ volatile("" ::: "memory");}
void native_legacy_pm_observed(unsigned stage,IOReturn status,unsigned long ordinal) {
    unsigned previous=native_legacy_pm_stage;
    native_legacy_pm_stage=stage;native_legacy_pm_status=status;native_legacy_pm_ordinal=(uint32_t)ordinal;
    native_legacy_pm_calls=native_legacy_pm_calls+1;
    if(stage==3)native_legacy_pm_callbacks=native_legacy_pm_callbacks+1;
    if(stage!=previous || native_legacy_pm_calls<=4)
        cloverLog("pm stage=%u calls=%u callbacks=%u ordinal=%u status=%08x",stage,native_legacy_pm_calls,native_legacy_pm_callbacks,
            (unsigned)ordinal,(unsigned)status);
}
volatile uint32_t native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_rej_reason;
volatile uint64_t native_legacy_rej_surfaces[4];
volatile uint32_t native_legacy_rej_state[3],native_legacy_rej_options[3],native_legacy_rej_size[2],native_legacy_rej_rects[2][4];
char native_legacy_rej_class[32];
volatile uint64_t native_legacy_rej_surface[7];   // width, height, format, planes, stride, allocation, descriptor bytes of the last rejected surface
void native_legacy_surface_rejected(uint64_t width,uint64_t height,uint32_t format,uint32_t planes,uint64_t stride,uint64_t allocation,uint64_t bytes) {
    native_legacy_rej_surface[0]=width;native_legacy_rej_surface[1]=height;native_legacy_rej_surface[2]=format;native_legacy_rej_surface[3]=planes;
    native_legacy_rej_surface[4]=stride;native_legacy_rej_surface[5]=allocation;native_legacy_rej_surface[6]=bytes;
}
// Last rejected swap request, copied out of the original request layout the validator reads (the offsets qualified in
// NativeLegacyDisplay::validateSwap). Observation only; nothing here changes the verdict.
static void recordRejectedSwap(unsigned reason,const void *request,const void *surface) {
    const unsigned char *bytes=reinterpret_cast<const unsigned char *>(request);
    if(!bytes)return;
    native_legacy_rej_reason=reason;
    uint64_t surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));
    for(unsigned i=0;i<4;++i)native_legacy_rej_surfaces[i]=surfaces[i];
    const unsigned stateOffsets[3]={792,796,800},optionOffsets[3]={804,808,812};
    for(unsigned i=0;i<3;++i){uint32_t a=0,b=0;memcpy(&a,bytes+stateOffsets[i],4);memcpy(&b,bytes+optionOffsets[i],4);
        native_legacy_rej_state[i]=a;native_legacy_rej_options[i]=b;}
    uint32_t width=0,height=0;memcpy(&width,bytes+816,4);memcpy(&height,bytes+820,4);native_legacy_rej_size[0]=width;native_legacy_rej_size[1]=height;
    const unsigned rectOffsets[2]={17,81};
    for(unsigned r=0;r<2;++r){uint32_t rect[4]={};memcpy(rect,bytes+rectOffsets[r],sizeof(rect));
        for(unsigned i=0;i<4;++i)native_legacy_rej_rects[r][i]=rect[i];}
    char name[32]={};
    if(surface){const OSObject *object=reinterpret_cast<const OSObject *>(surface);
        const OSMetaClass *meta=object->getMetaClass();const char *cls=meta?meta->getClassName():nullptr;
        if(cls)strlcpy(name,cls,sizeof(name));}
    memcpy(native_legacy_rej_class,name,sizeof(name));
}
// Content statistics of presented frames (copy = kind 0, blank fill = kind 1): the first 32 with the time in seconds
// since the first frame. A per-frame record shows the animation cadence and when the OS blanks the panel.
#define NATIVE_LEGACY_FRAMES 32
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_frame_kind[NATIVE_LEGACY_FRAMES];
volatile uint32_t native_legacy_frame_time[NATIVE_LEGACY_FRAMES];
volatile uint64_t native_legacy_frame_stats[NATIVE_LEGACY_FRAMES][5];
static uint64_t frameEpoch;
void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy) {
    uint32_t n=native_legacy_frame_count;
    uint64_t now,frequency;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(now));__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
    if(!frameEpoch)frameEpoch=now;
    if(n<NATIVE_LEGACY_FRAMES) {
        native_legacy_frame_kind[n]=kind;
        native_legacy_frame_time[n]=frequency?(uint32_t)((now-frameEpoch)/frequency):0;
        native_legacy_frame_stats[n][0]=count;native_legacy_frame_stats[n][1]=minx;native_legacy_frame_stats[n][2]=miny;
        native_legacy_frame_stats[n][3]=maxx;native_legacy_frame_stats[n][4]=maxy;
    }
    native_legacy_frame_count=n+1;
    if(n<6)
        cloverLog("frame#%u kind=%u t=%us pixels=%llu box=%llux%llu..%llux%llu",n,kind,(unsigned)(frequency?(now-frameEpoch)/frequency:0),
            (unsigned long long)count,(unsigned long long)minx,(unsigned long long)miny,(unsigned long long)maxx,(unsigned long long)maxy);
}
volatile uint32_t native_legacy_frame_colors[8][4];
volatile uint64_t native_legacy_frame_color_counts[8][4],native_legacy_frame_color_other[8];
// Called right after the matching native_legacy_frame_observed(); fills slot count-1 (or the blank slot).
void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other) {
    uint32_t n=native_legacy_frame_count;
    if(!n || n>8)return;   // dominant colours are kept for the first eight frames only
    n-=1;
    for(unsigned i=0;i<4;++i){native_legacy_frame_colors[n][i]=colors[i];native_legacy_frame_color_counts[n][i]=counts[i];}
    native_legacy_frame_color_other[n]=other;
    if(n<4)
        cloverLog("frame#%u colours %08x/%llu %08x/%llu %08x/%llu %08x/%llu other=%llu",n,colors[0],(unsigned long long)counts[0],
            colors[1],(unsigned long long)counts[1],colors[2],(unsigned long long)counts[2],colors[3],(unsigned long long)counts[3],
            (unsigned long long)other);
}
// The life of every accepted swap, from the kernel's side: validated (the client submitted it), offered to the GPU copy
// (or started on the CPU), and retired. Times are microseconds since boot; the last 64 are kept.
struct SwapEvent {uint64_t submitted,offered,done;uint32_t number;};
static SwapEvent swapEvents[64];static uint32_t swapEventCount;
void native_legacy_swap_stage(unsigned stage) {
    const uint64_t now=probeNowUs();
    uint32_t total=swapEventCount;
    if(stage==0) {
        SwapEvent &entry=swapEvents[total%64];entry={now,0,0,total+1};swapEventCount=total+1;return;
    }
    uint32_t first=total>64?total-64:0;
    for(uint32_t i=first;i<total;++i) {
        SwapEvent &entry=swapEvents[i%64];
        if(stage==1 && !entry.offered && !entry.done){entry.offered=now;return;}
        if(stage==2 && !entry.done){entry.done=now;if(!entry.offered)entry.offered=now;return;}
    }
}
unsigned native_legacy_swap_timeline(char *out,unsigned cap) {
    unsigned at=0;out[0]=0;
    uint32_t total=swapEventCount,first=total>40?total-40:0;
    for(uint32_t i=first;i<total && at+90<cap;++i) {
        const SwapEvent &entry=swapEvents[i%64];
        uint64_t previous=i>first?swapEvents[(i-1)%64].submitted:entry.submitted;
        at+=snprintf(out+at,cap-at,"W%u t=%llu.%03llus +%llums offer=%lldms done=%lldms\n",entry.number,(unsigned long long)(entry.submitted/1000000),
            (unsigned long long)((entry.submitted/1000)%1000),(unsigned long long)((entry.submitted-previous)/1000),
            entry.offered?(long long)((entry.offered-entry.submitted)/1000):-1LL,entry.done?(long long)((entry.done-entry.submitted)/1000):-1LL);
    }
    return at;
}
// Display refresh tick (sheng-vsync): vsync messages sent, arms, idle stops, late ticks, starts, refresh properties
// reported at that rate, and the rate.
static volatile uint64_t vsyncEvents[6];
static volatile unsigned vsyncHz;
void native_legacy_vsync_observed(unsigned event,unsigned hertz) {
    if(event<6)vsyncEvents[event]=vsyncEvents[event]+1;
    vsyncHz=hertz;
    if(vsyncEvents[1]==1 || vsyncEvents[1]==60 || (vsyncEvents[1]>60 && (vsyncEvents[1]%300)==0))
        cloverLog("vsync hz=%u sent=%llu arms=%llu idle=%llu late=%llu start=%llu refresh=%llu",hertz,
            (unsigned long long)vsyncEvents[1],(unsigned long long)vsyncEvents[0],(unsigned long long)vsyncEvents[2],
            (unsigned long long)vsyncEvents[3],(unsigned long long)vsyncEvents[4],(unsigned long long)vsyncEvents[5]);
}
unsigned native_legacy_prepare_report(char *out,unsigned cap) {
    return snprintf(out,cap,"prepare calls=%u reason=%u attempts=%u vsync hz=%u sent=%llu arms=%llu idle=%llu late=%llu start=%llu refresh=%llu",
        native_legacy_prepare_calls,native_legacy_prepare_reason,native_legacy_prepare_attempts,
        vsyncHz,(unsigned long long)vsyncEvents[1],(unsigned long long)vsyncEvents[0],
        (unsigned long long)vsyncEvents[2],(unsigned long long)vsyncEvents[3],(unsigned long long)vsyncEvents[4],(unsigned long long)vsyncEvents[5]);
}
void native_legacy_swap_validation_observed(bool accepted,unsigned reason,const void *request,const void *surface) {
    if(accepted)native_legacy_swap_stage(0);
    if(accepted)native_legacy_swap_accepted_count=native_legacy_swap_accepted_count+1;
    else {native_legacy_swap_rejected_count=native_legacy_swap_rejected_count+1;recordRejectedSwap(reason,request,surface);}
    native_legacy_swap_validation_calls=native_legacy_swap_validation_calls+1;
    native_legacy_swap_validation_result=accepted;native_legacy_swap_validation_reason=reason;
    native_legacy_swap_request=(uintptr_t)request;native_legacy_swap_surface=(uintptr_t)surface;
    if(accepted) {
        if(native_legacy_swap_accepted_count<=4)
            cloverLog("swap#%u accepted request=%llx surface=%llx",native_legacy_swap_accepted_count,
                (unsigned long long)(uintptr_t)request,(unsigned long long)(uintptr_t)surface);
    } else if(native_legacy_swap_rejected_count<=8) {
        cloverLog("swap reject reason=%u state=%x %x %x size=%x %x class=%s surface=%u/%u f%x p%u stride=%llu alloc=%llu desc=%llu",
            reason,native_legacy_rej_state[0],native_legacy_rej_state[1],native_legacy_rej_state[2],
            native_legacy_rej_size[0],native_legacy_rej_size[1],native_legacy_rej_class,
            (unsigned)native_legacy_rej_surface[0],(unsigned)native_legacy_rej_surface[1],
            (unsigned)native_legacy_rej_surface[2],(unsigned)native_legacy_rej_surface[3],
            (unsigned long long)native_legacy_rej_surface[4],(unsigned long long)native_legacy_rej_surface[5],
            (unsigned long long)native_legacy_rej_surface[6]);
    }
    native_legacy_swap_validation_checkpoint(accepted,reason,request,surface);
}
// Time of the pixel copy into the scanout buffer (counter ticks), for the report.
volatile uint64_t native_legacy_copy_n,native_legacy_copy_last,native_legacy_copy_max,native_legacy_copy_sum,native_legacy_copy_bytes_last,native_legacy_copy_bytes_sum;
static uint64_t counterFrequency(void) {uint64_t frequency;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));return frequency;}
void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes) {
    native_legacy_copy_bytes_last=bytes;native_legacy_copy_bytes_sum=native_legacy_copy_bytes_sum+bytes;
    native_legacy_copy_n=native_legacy_copy_n+1;native_legacy_copy_last=ticks;native_legacy_copy_sum=native_legacy_copy_sum+ticks;
    if(ticks>native_legacy_copy_max)native_legacy_copy_max=ticks;
    uint64_t frequency=counterFrequency();
    if(native_legacy_copy_n<=4)
        cloverLog("copy#%llu %llu ms bytes=%llu max=%llu ms",
            (unsigned long long)native_legacy_copy_n,frequency?(unsigned long long)(ticks*1000/frequency):0ULL,
            (unsigned long long)bytes,frequency?(unsigned long long)(native_legacy_copy_max*1000/frequency):0ULL);
}
void native_legacy_transfer_observed(IOReturn status,const void *request,uint64_t bytes) {
    native_legacy_transfer_calls=native_legacy_transfer_calls+1;
    if(status==kIOReturnSuccess)native_legacy_swap_stage(2);
    native_legacy_transfer_status=status;native_legacy_transfer_request=(uintptr_t)request;native_legacy_transfer_bytes=bytes;
    if(status==kIOReturnSuccess) {
        if(native_legacy_transfer_calls<=4)
            cloverLog("scanout#%u ok bytes=%llu retired=%llu frames=%llu",native_legacy_transfer_calls,(unsigned long long)bytes,
                (unsigned long long)native_legacy_retirement_calls,(unsigned long long)native_legacy_retired_frames);
    } else
        cloverLog("scanout#%u FAILED status=%08x prepare=%u tries=%u bytes=%llu",native_legacy_transfer_calls,(unsigned)status,
            native_legacy_prepare_reason,native_legacy_prepare_attempts,(unsigned long long)bytes);
}
bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    return gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter,display,memory,stride,width,height);
}
void native_legacy_gpu_fallback_observed(int status) {
    native_legacy_gpu_last_status=(uint32_t)status;native_legacy_gpu_fallbacks=native_legacy_gpu_fallbacks+1;
    if(native_legacy_gpu_fallbacks<=4)
        cloverLog("gpu hand-off refused status=%08x early_retires=%llu early_failures=%llu",
            (unsigned)status,(unsigned long long)native_legacy_early_retires,(unsigned long long)native_legacy_early_failures);
}
void native_legacy_retirement_observed(IOService *display) {
    const unsigned char *bytes=reinterpret_cast<const unsigned char *>(display);
    uint64_t current=0,pending=0;uint32_t completed=0,presented=0,frames=0;
    memcpy(&current,bytes+5472,8);memcpy(&pending,bytes+5480,8);
    memcpy(&completed,bytes+5820,4);memcpy(&presented,bytes+5824,4);memcpy(&frames,bytes+252,4);
    native_legacy_retired_current=current;native_legacy_retired_pending=pending;
    native_legacy_retired_generation=completed;native_legacy_presented_generation=presented;native_legacy_retired_frames=frames;
    native_legacy_retirement_calls=native_legacy_retirement_calls+1;
    if(native_legacy_retirement_calls<=4)
        cloverLog("retire#%llu done=%x presented=%x frames=%x current=%llx pending=%llx",
            (unsigned long long)native_legacy_retirement_calls,completed,presented,frames,
            (unsigned long long)current,(unsigned long long)pending);
}
// The display object's properties as they are now (the registration copy below predates setDisplayRefreshProperties and
// everything after it), for a report line. Returns the bytes written (NUL-terminated).
unsigned native_legacy_properties_now(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    out[0]=0;
    IOService *display=reinterpret_cast<IOService *>(native_legacy_object);
    if(!display)return 0;
    OSSerialize *xml=OSSerialize::withCapacity(8192);
    unsigned size=0;
    if(xml && display->serializeProperties(xml)) {
        size=xml->getLength();if(size>=cap)size=cap-1;
        memcpy(out,xml->text(),size);out[size]=0;
    }
    OSSafeReleaseNULL(xml);
    return size;
}
void native_legacy_registration_observed(IOService *display) {
    native_legacy_registration_calls=native_legacy_registration_calls+1;
    OSSerialize *xml=OSSerialize::withCapacity(2048);
    if(xml && display->serializeProperties(xml)) {
        unsigned size=xml->getLength();if(size>=sizeof(native_legacy_properties))size=sizeof(native_legacy_properties)-1;
        memcpy(native_legacy_properties,xml->text(),size);native_legacy_properties[size]=0;native_legacy_properties_length=size;
    }
    OSSafeReleaseNULL(xml);
    cloverLog("registered #%u properties=%u bytes",native_legacy_registration_calls,native_legacy_properties_length);
}
void native_legacy_client_enter(task_t task,uint32_t type) {
    native_legacy_client_calls=native_legacy_client_calls+1;native_legacy_client_task=(uintptr_t)task;
    native_legacy_client_type=type;native_legacy_client_pid=task?proc_pid((proc_t)get_bsdtask_info(task)):0;
}
// Every framebuffer user client seen (object, pid), with what it does: the original client's vtable is copied and two
// entries (externalMethod, slot 168, and registerNotificationPort(port,type,refCon), slot 175) point at wrappers that
// count and forward to the original function. The original code is not touched; only this object's vptr names the copy.
// XNU's ARM64 vptr points at slot 0 (the original code dispatches with ldr x8,[vptr,#slot*8]).
enum {kSeenClients=8,kSelectors=96,kRegEvents=8,kVtableSlots=256};
struct ClientStat {
    uintptr_t object;int pid;uint32_t calls,fails;uint32_t selector_calls[kSelectors],selector_fails[kSelectors],selector_status[kSelectors];
    unsigned regs;struct {uint32_t type;uint64_t port;uint32_t ref,status;} reg[kRegEvents];
    uint64_t sel6_first[3],sel6_last[3];uint32_t sel6_distinct;uint64_t sel6_seen[8][3];uint32_t sel6_calls[8],sel6_fails[8],sel6_status[8];
    uint32_t seq_count;struct {uint32_t a,b,c,status,done,presented;} seq[24];
    uintptr_t original_external,original_register;
};
static ClientStat seenClients[kSeenClients];static unsigned seenClientCount;
typedef IOReturn (*ExternalFn)(IOUserClient *,uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *);
typedef IOReturn (*RegisterFn)(IOUserClient *,mach_port_t,UInt32,UInt32);
static ClientStat *statFor(IOUserClient *client) {
    for(unsigned i=0;i<seenClientCount;++i)if(seenClients[i].object==(uintptr_t)client)return &seenClients[i];
    return nullptr;
}
static IOReturn wrappedExternal(IOUserClient *client,uint32_t selector,IOExternalMethodArguments *arguments,IOExternalMethodDispatch *dispatch,OSObject *target,void *reference) {
    ClientStat *stat=statFor(client);
    IOReturn status=stat?reinterpret_cast<ExternalFn>(stat->original_external)(client,selector,arguments,dispatch,target,reference):kIOReturnNotAttached;
    if(stat) {
        if(selector==6 && arguments && arguments->scalarInput && arguments->scalarInputCount>=3) {
            const uint64_t *in=arguments->scalarInput;
            if(!stat->sel6_distinct && !stat->sel6_first[0] && !stat->sel6_first[1])memcpy(stat->sel6_first,in,sizeof stat->sel6_first);
            memcpy(stat->sel6_last,in,sizeof stat->sel6_last);
            int at=-1;
            for(unsigned k=0;k<stat->sel6_distinct && k<8;++k)if(!memcmp(stat->sel6_seen[k],in,24))at=(int)k;
            if(at<0 && stat->sel6_distinct<8){at=(int)stat->sel6_distinct++;memcpy(stat->sel6_seen[at],in,24);}
            if(stat->seq_count<24){
                auto &entry=stat->seq[stat->seq_count++];
                entry.a=(uint32_t)in[0];entry.b=(uint32_t)in[1];entry.c=(uint32_t)in[2];entry.status=(uint32_t)status;
                const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
                entry.done=entry.presented=0;
                if(object){memcpy(&entry.done,object+5820,4);memcpy(&entry.presented,object+5824,4);}
            }
            if(at>=0){++stat->sel6_calls[at];if(status)++stat->sel6_fails[at];stat->sel6_status[at]=(uint32_t)status;}
            if(at>=0 && stat->seq_count && stat->seq_count<=8)
                cloverLog("client pid=%d selector 6 %llx %llx %llx -> %08x done=%x presented=%x",stat->pid,
                    (unsigned long long)in[0],(unsigned long long)in[1],(unsigned long long)in[2],(unsigned)status,
                    stat->seq[stat->seq_count-1].done,stat->seq[stat->seq_count-1].presented);
        }
        ++stat->calls;if(status)++stat->fails;
        if(selector<kSelectors){
            ++stat->selector_calls[selector];if(status)++stat->selector_fails[selector];stat->selector_status[selector]=(uint32_t)status;
            if(stat->selector_calls[selector]==1)
                cloverLog("client pid=%d first selector=%u args=%u/%u scalar=%u/%u status=%08x",stat->pid,selector,
                    arguments?arguments->structureInputSize:0,arguments?arguments->structureOutputSize:0,
                    arguments?arguments->scalarInputCount:0,arguments?arguments->scalarOutputCount:0,(unsigned)status);
        }
    }
    return status;
}
static IOReturn wrappedRegister(IOUserClient *client,mach_port_t port,UInt32 type,UInt32 reference) {
    ClientStat *stat=statFor(client);
    IOReturn status=stat?reinterpret_cast<RegisterFn>(stat->original_register)(client,port,type,reference):kIOReturnNotAttached;
    if(stat && stat->regs<kRegEvents){
        auto &entry=stat->reg[stat->regs++];entry.type=type;entry.port=(uint64_t)(uintptr_t)port;entry.ref=reference;entry.status=(uint32_t)status;
        cloverLog("client pid=%d registerNotification type=%u ref=%x port=%llx -> %08x",stat->pid,type,reference,
            (unsigned long long)(uintptr_t)port,(unsigned)status);
    }
    return status;
}
static void interpose(IOUserClient *client,int pid) {
    if(seenClientCount>=kSeenClients)return;
    ClientStat &stat=seenClients[seenClientCount];
    memset(&stat,0,sizeof stat);
    uintptr_t *vtable=*reinterpret_cast<uintptr_t **>(client);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kVtableSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,vtable,kVtableSlots*sizeof(uintptr_t));
    stat.object=(uintptr_t)client;stat.pid=pid;stat.original_external=copy[168];stat.original_register=copy[175];
    copy[168]=(uintptr_t)&wrappedExternal;copy[175]=(uintptr_t)&wrappedRegister;
    ++seenClientCount;
    *reinterpret_cast<uintptr_t **>(client)=copy;
}
// The display object's state words the original wait selector tests (offsets 5808..5872, as dwords), for a report line.
unsigned native_legacy_state_words(char *out,unsigned cap) {
    out[0]=0;
    const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
    if(!object)return 0;
    unsigned at=snprintf(out,cap,"d+5808:");
    for(unsigned offset=5808;offset<5872 && at+16<cap;offset+=4){uint32_t value=0;memcpy(&value,object+offset,4);at+=snprintf(out+at,cap-at," %x",value);}
    return at;
}
unsigned native_legacy_clients_dump(char *out,unsigned cap) {
    unsigned at=0;out[0]=0;
    for(unsigned c=0;c<seenClientCount && at+120<cap;++c) {
        const ClientStat &stat=seenClients[c];
        at+=snprintf(out+at,cap-at,"C%u pid=%d calls=%u fails=%u regs=%u",c,stat.pid,stat.calls,stat.fails,stat.regs);
        for(unsigned r=0;r<stat.regs && r<kRegEvents && at+60<cap;++r)
            at+=snprintf(out+at,cap-at," reg(t%u,p%llx,r%x)=%x",stat.reg[r].type,(unsigned long long)stat.reg[r].port,stat.reg[r].ref,stat.reg[r].status);
        at+=snprintf(out+at,cap-at,"\n");
        for(unsigned k=0;k<stat.sel6_distinct && k<8 && at+100<cap;++k)
            at+=snprintf(out+at,cap-at,"  sel6 args %llx %llx %llx: %u calls %u fails last %x\n",(unsigned long long)stat.sel6_seen[k][0],
                (unsigned long long)stat.sel6_seen[k][1],(unsigned long long)stat.sel6_seen[k][2],stat.sel6_calls[k],stat.sel6_fails[k],stat.sel6_status[k]);
        for(unsigned k=0;k<stat.seq_count && at+90<cap;++k)
            at+=snprintf(out+at,cap-at,"  s6#%u %x %x %x -> %x [5820]=%x [5824]=%x\n",k,stat.seq[k].a,stat.seq[k].b,stat.seq[k].c,
                stat.seq[k].status,stat.seq[k].done,stat.seq[k].presented);
        unsigned n=0;
        for(unsigned selector=0;selector<kSelectors && at+40<cap;++selector) {
            if(!stat.selector_calls[selector])continue;
            if(!n)at+=snprintf(out+at,cap-at,"  sel");
            at+=snprintf(out+at,cap-at," %u:%u/%u/%x",selector,stat.selector_calls[selector],stat.selector_fails[selector],stat.selector_status[selector]);
            if(++n%6==0)at+=snprintf(out+at,cap-at,"\n  sel");
        }
        if(n)at+=snprintf(out+at,cap-at,"\n");
    }
    return at;
}
void native_legacy_client_observed(IOReturn status,IOUserClient *client) {
    if(client && !status)interpose(client,native_legacy_client_pid);
    native_legacy_client_status=status;native_legacy_client_object=(uintptr_t)client;
    if(client)strlcpy(native_legacy_client_class,client->getMetaClass()->getClassName(),sizeof(native_legacy_client_class));
    if(native_legacy_client_calls<=8)
        cloverLog("client#%u pid=%d type=%u status=%08x class=%s seen=%u",native_legacy_client_calls,native_legacy_client_pid,
            native_legacy_client_type,(unsigned)status,native_legacy_client_class,seenClientCount);
    native_legacy_client_checkpoint();
}
// The unit-scale policy, unchanged from the reference: `offline-legacy-unit-scale=<1..3>` publishes that many actual
// scanout pixels per logical point through /chosen, which is the only place the family reads a UI scale from.
static IORegistryEntry *unitScaleChosen;
static bool unitScaleAdded,unitRotationAdded;
static void finishUnitScalePolicy(bool keep) {
    if(unitScaleChosen && !keep) {
        if(unitScaleAdded)unitScaleChosen->removeProperty("display-scale");
        if(unitRotationAdded)unitScaleChosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(unitScaleChosen);unitScaleAdded=false;unitRotationAdded=false;
}
static bool publishOrMatchWord(IORegistryEntry *entry,const char *key,uint32_t value,bool &added) {
    OSObject *previous=entry->getProperty(key);
    if(previous) {
        OSData *data=OSDynamicCast(OSData,previous);uint32_t word=0;
        if(!data || data->getLength()!=sizeof(word))return false;
        memcpy(&word,data->getBytesNoCopy(),sizeof(word));return word==value;
    }
    OSData *data=OSData::withBytes(&value,sizeof(value));
    bool result=data && entry->setProperty(key,data);OSSafeReleaseNULL(data);added=result;return result;
}
// Called by the driver from startHardware(), after it installed the boot dimensions and mapped the console memory.
// Returning false stops the backend, so this is also where a board policy that must hold before the first frame runs.
bool native_legacy_gop_mapping_observed(IOService *object,IOMemoryMap *mapping,uint32_t width,uint32_t height) {
    if((uintptr_t)object!=native_legacy_object)panic("NativeLegacyDisplay: wrong mapping owner");
    native_legacy_gop_map=(uintptr_t)mapping;native_legacy_gop_virtual=mapping->getVirtualAddress();
    native_legacy_gop_physical=mapping->getPhysicalAddress();native_legacy_gop_bytes=mapping->getLength();
    native_legacy_gop_width=width;native_legacy_gop_height=height;
    native_legacy_gop_mapping_checkpoint();
    unsigned enabled=0;PE_parse_boot_argn("offline-legacy-unit-scale",&enabled,sizeof(enabled));
    if(!enabled)return true;
    native_legacy_unit_scale_calls=native_legacy_unit_scale_calls+1;native_legacy_unit_scale_status=1;
    unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
    PE_Video video={};
    // The value is the UI scale: logical points = actual scanout pixels / scale (1..3). It changes only the published
    // display-scale; the scanout copy still moves one actual pixel per pixel.
    if(enabled<1 || enabled>3 || phase!=4 || unitScaleChosen || PE_current_console(&video) ||
       video.v_width!=width || video.v_height!=height || video.v_rotate ||
       mapping->getPhysicalAddress()!=video.v_baseAddr)return false;
    // Explicit offline UI policy: `enabled` actual GOP scanout pixels per logical point in each direction (1 = native
    // pixels). This is not physical pixel density, panel timing, an Apple device identity or a synthesized mode list.
    unitScaleChosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    if(!unitScaleChosen || !publishOrMatchWord(unitScaleChosen,"display-scale",enabled,unitScaleAdded) ||
       !publishOrMatchWord(unitScaleChosen,"display-rotation",video.v_rotate,unitRotationAdded)) {
        finishUnitScalePolicy(false);return false;
    }
    OSData *scale=OSDynamicCast(OSData,unitScaleChosen->getProperty("display-scale"));
    OSData *rotation=OSDynamicCast(OSData,unitScaleChosen->getProperty("display-rotation"));
    uint32_t actualScale=0,actualRotation=0;
    if(!scale || !rotation || scale->getLength()!=4 || rotation->getLength()!=4) {
        finishUnitScalePolicy(false);return false;
    }
    memcpy(&actualScale,scale->getBytesNoCopy(),4);memcpy(&actualRotation,rotation->getBytesNoCopy(),4);
    native_legacy_chosen_scale=actualScale;native_legacy_chosen_rotation=actualRotation;
    native_legacy_unit_scale_status=actualScale==enabled && actualRotation==video.v_rotate?0:2;
    if(native_legacy_unit_scale_status){finishUnitScalePolicy(false);return false;}
    return true;
}
// One of the driver's nine guarded leaves was reached: an original call into a backend slot this port does not
// implement. Stopping here is the whole contract -- the panic screen prints the message, and the tee carries it too.
__attribute__((noreturn,noinline)) void native_legacy_backend_guard(unsigned slot,void *object,void *caller) {
    native_legacy_guard_slot=slot;native_legacy_guard_object=(uintptr_t)object;native_legacy_guard_caller=(uintptr_t)caller;
    panic("NativeLegacyDisplay: leaf=%u caller=%llx stage=%u PM=%u swap=%u/%u copy=%08x bytes=%llu retire=%u handoff=%u/%u",
      slot,(uint64_t)(uintptr_t)caller,native_legacy_stage,native_legacy_pm_stage,
      native_legacy_swap_validation_result,native_legacy_swap_validation_reason,
      (unsigned)native_legacy_transfer_status,native_legacy_transfer_bytes,(unsigned)native_legacy_retirement_calls,
      native_legacy_prepare_reason,native_legacy_prepare_attempts);
}
// The whole state of this instrument on one line, for the log ring and for any later reader.
unsigned native_legacy_clover_report(char *out,unsigned cap) {
    if(!out || !cap)return 0;
    unsigned at=snprintf(out,cap,"stage=%u meta=%d init=%x size=%u parent=%u obj=%llx nub=%llx video %ux%u row=%u depth=%u display=%llu rot=%u scale=%u base=%llx bytes=%llx gop %ux%u map=%llx va=%llx pa=%llx len=%llu reg=%u dt=%u surface=%d found=%u start=%x probe=%u pm=%u/%x swap=%u %u/%u accept=%u reject=%u scanout=%u/%08x/%llu retire=%llu done=%x shown=%x frames=%u copy=%llu last=%llums",
        native_legacy_stage,native_legacy_meta_status,native_legacy_init_result,native_legacy_object_size,native_legacy_parent_match,
        (unsigned long long)native_legacy_object,(unsigned long long)native_legacy_nub,
        native_legacy_video_width,native_legacy_video_height,native_legacy_video_row_bytes,native_legacy_video_depth,
        (unsigned long long)native_legacy_video_display,native_legacy_video_rotate,native_legacy_video_scale,
        (unsigned long long)native_legacy_video_base,(unsigned long long)native_legacy_video_bytes,
        native_legacy_gop_width,native_legacy_gop_height,(unsigned long long)native_legacy_gop_map,
        (unsigned long long)native_legacy_gop_virtual,(unsigned long long)native_legacy_gop_physical,(unsigned long long)native_legacy_gop_bytes,
        native_legacy_registration_calls,native_legacy_dt_attached,native_legacy_surface_status,native_legacy_surface_found,
        native_legacy_start_result,native_legacy_probe_match,native_legacy_pm_stage,(unsigned)native_legacy_pm_status,
        native_legacy_swap_validation_calls,native_legacy_swap_validation_result,native_legacy_swap_validation_reason,
        native_legacy_swap_accepted_count,native_legacy_swap_rejected_count,native_legacy_transfer_calls,
        (unsigned)native_legacy_transfer_status,(unsigned long long)native_legacy_transfer_bytes,
        (unsigned long long)native_legacy_retirement_calls,(unsigned)native_legacy_retired_generation,
        (unsigned)native_legacy_presented_generation,native_legacy_frame_count,(unsigned long long)native_legacy_copy_n,
        (unsigned long long)(counterFrequency()?native_legacy_copy_last*1000/counterFrequency():0));
    return at;
}
void native_legacy_display_probe(void) {
    // The registration path and the optional original-userclient inspection both reach this boot-thread entry. Run the
    // class constructors only once; a second postModLoad would try to register the same metaclasses again.
    if(native_legacy_stage)return;
    native_legacy_stage=1;void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){cloverLog("probe: preModLoad refused");native_legacy_display_checkpoint();return;}
    native_legacy_display_static_init();native_gop_presenter_static_init();native_legacy_meta_status=OSMetaClass::postModLoad(handle);
    if(native_legacy_meta_status){cloverLog("probe: postModLoad=%d",native_legacy_meta_status);native_legacy_display_checkpoint();return;}
    PE_Video video={};native_legacy_stage=2;
    if(PE_current_console(&video)) {cloverLog("probe: no console descriptor");native_legacy_display_checkpoint();return;}
    native_legacy_video_base=video.v_baseAddr;native_legacy_video_bytes=video.v_rowBytes*video.v_height;
    native_legacy_video_display=video.v_display;
    native_legacy_video_width=(uint32_t)video.v_width;native_legacy_video_height=(uint32_t)video.v_height;
    native_legacy_video_depth=(uint32_t)video.v_depth;native_legacy_video_row_bytes=(uint32_t)video.v_rowBytes;
    native_legacy_video_rotate=video.v_rotate;native_legacy_video_scale=video.v_scale;
    cloverLog("probe: console %ux%u row=%u depth=%u display=%llu rotate=%u scale=%u base=%llx length=%llx",
        native_legacy_video_width,native_legacy_video_height,native_legacy_video_row_bytes,native_legacy_video_depth,
        (unsigned long long)video.v_display,native_legacy_video_rotate,native_legacy_video_scale,
        (unsigned long long)video.v_baseAddr,(unsigned long long)(video.v_rowBytes*video.v_height));
    if(!video.v_baseAddr || !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
       video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
       video.v_rowBytes>UINT64_MAX/video.v_height) {
        cloverLog("probe: console rejected");native_legacy_display_checkpoint();return;
    }
    IOService *display=native_legacy_display_allocate();native_legacy_object=(uintptr_t)display;
    if(!display){cloverLog("probe: allocation failed");native_legacy_display_checkpoint();return;}
    native_legacy_object_size=display->getMetaClass()->getClassSize();
    native_legacy_parent_match=(display->metaCast("IOMobileFramebufferLegacy") && display->metaCast("IOMobileFramebuffer") && display->metaCast("IOService"));
    native_legacy_stage=3;native_legacy_init_result=(uint32_t)display->init();
    cloverLog("probe: object=%llx size=%u parent=%u init=%x",
        (unsigned long long)(uintptr_t)display,native_legacy_object_size,native_legacy_parent_match,native_legacy_init_result);
    unsigned phase=1;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
    if(native_legacy_init_result && (phase==2 || phase==3 || phase==4)) {
        // IOSurface matches the real IOBSD resource, published by bsd_autoconf. Retain this actual object until that
        // normal lifecycle boundary.
        pendingLegacyDisplay=display;return;
    }
    native_legacy_stage=7;display->release();native_legacy_released=1;native_legacy_stage=8;
    cloverLog("probe: parked at stage=%u (phase=%u init=%x)",native_legacy_stage,phase,native_legacy_init_result);
    native_legacy_display_checkpoint();
}
void native_legacy_display_late_probe(void) {
    IOService *display=pendingLegacyDisplay;if(!display)return;pendingLegacyDisplay=nullptr;
    native_legacy_surface_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching=IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface=matching?IOService::waitForMatchingService(matching,5000000000ULL):nullptr;
    native_legacy_surface_found=surface!=nullptr;OSSafeReleaseNULL(surface);OSSafeReleaseNULL(matching);
    cloverLog("late: IOSurface load=%d root found=%u",native_legacy_surface_status,native_legacy_surface_found);
    PE_Video video={};
    if(!native_legacy_surface_status && native_legacy_surface_found && !PE_current_console(&video)
        && video.v_baseAddr && video.v_width && video.v_height && video.v_depth==32
        && !video.v_rotate && video.v_width<=16384 && video.v_height<=16384
        && video.v_rowBytes>=video.v_width*4 && video.v_rowBytes<=UINT64_MAX/video.v_height) {
        native_legacy_video_base=video.v_baseAddr;native_legacy_video_bytes=video.v_rowBytes*video.v_height;
        // A real firmware framebuffer resource, with addresses and extent obtained from the actual GOP handoff. It has
        // no invented compatible, display list, panel timing, GPU/fence completion or vendor registers.
        native_legacy_stage=4;IOPlatformDevice *nub=new IOPlatformDevice;
        native_legacy_nub=(uintptr_t)nub;bool nubAttached=false,nubStarted=false,attached=false,started=false;
        IORegistryEntry *displayTreeParent=nullptr;bool dtAttached=false;
        IOService *parent=IOService::getResourceService();
        IODeviceMemory *memory=IODeviceMemory::withRange(video.v_baseAddr,video.v_rowBytes*video.v_height);
        OSArray *ranges=OSArray::withCapacity(1);
        if(nub && memory && ranges && parent && nub->init()) {
            ranges->setObject(memory);nub->setDeviceMemory(ranges);nub->setName("NativeGOPFramebuffer");
            nubAttached=nub->attach(parent);if(nubAttached)nubStarted=nub->start(parent);
            if(nubStarted) {
                unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
                if(phase==4) {
                    // This is the actually implemented GOP display subsystem, attached to its real IOPlatformDevice
                    // memory resource. No Apple/vendor compatible or panel timing is supplied. This board's device tree
                    // names its IOPlatformExpert root /arm-io (scripts/make-clover-adt.py).
                    displayTreeParent=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
                    const char type[]="display-subsystem";OSData *data=OSData::withBytes(type,sizeof(type));
                    if(displayTreeParent && data && nub->setProperty("device_type",data)) {
                        nub->setName("gop-display",gIODTPlane);dtAttached=nub->attachToParent(displayTreeParent,gIODTPlane);
                    }
                    OSSafeReleaseNULL(data);native_legacy_dt_attached=dtAttached;
                }
                if(phase==3) {
                    IOService *presenter=native_gop_presenter_create();bool presenterAttached=false;
                    if(presenter && presenter->init() && (presenterAttached=presenter->attach(nub)) && presenter->start(nub)) {
                        // Explicit own transport service only. The incomplete original framebuffer backend is never published.
                        activeGOPPresenter=presenter;activeGOPNub=nub;nub=nullptr;nubAttached=false;nubStarted=false;
                        cloverLog("late: presenter only");
                    } else {if(presenterAttached)presenter->detach(nub);OSSafeReleaseNULL(presenter);}
                } else {
                    SInt32 score=0;native_legacy_probe_match=(phase!=4 || dtAttached) && display->probe(nub,&score)==display;
                    if(native_legacy_probe_match)attached=display->attach(nub);
                    if(attached){native_legacy_stage=5;started=display->start(nub);native_legacy_start_result=started;}
                    cloverLog("late: dt=%u probe=%u attached=%u started=%u score=%d",dtAttached,native_legacy_probe_match,attached,started,(int)score);
                    if(started && phase==4) {
                        activeLegacyDisplay=display;activeGOPNub=nub;native_legacy_stage=10;
                        {
                            unsigned gpu=0;PE_parse_boot_argn("sheng-fb-gpu",&gpu,sizeof gpu);
                            if(gpu==1) {
                                IOService *handoff=native_gop_presenter_create();
                                // Not a child of the display subsystem's node: the original display code enumerates that node's
                                // children (sheng int-075/077: SpringBoard found no display and crashed with the presenter there).
                                IOService *home=IOService::getResourceService();
                                bool handoffAttached=false;
                                if(handoff && home)native_gop_presenter_set_source(handoff,nub);
                                if(handoff && home && handoff->init() && (handoffAttached=handoff->attach(home)) && handoff->start(home))gpuHandoffPresenter=handoff;
                                else {if(handoffAttached)handoff->detach(home);OSSafeReleaseNULL(handoff);}
                                cloverLog("late: hand-off presenter=%llx attached=%d",
                                    (unsigned long long)(uintptr_t)gpuHandoffPresenter,(int)handoffAttached);
                            }
                        }
                        finishUnitScalePolicy(true);
                        OSSafeReleaseNULL(displayTreeParent);
                        {
                            char report[512];unsigned length=native_legacy_clover_report(report,sizeof report);
                            if(length && length<sizeof report)IOLog("%s %s\n",cloverRound,report);
                        }
                        native_legacy_display_checkpoint();return;
                    }
                }
            }
        }
        native_legacy_stage=6;finishUnitScalePolicy(false);
        if(started)display->stop(nub);if(attached)display->detach(nub);
        if(dtAttached)nub->detachFromParent(displayTreeParent,gIODTPlane);
        if(nubStarted)nub->stop(parent);if(nubAttached)nub->detach(parent);
        OSSafeReleaseNULL(ranges);OSSafeReleaseNULL(memory);OSSafeReleaseNULL(nub);
        OSSafeReleaseNULL(displayTreeParent);
    }
    native_legacy_stage=7;display->release();native_legacy_released=1;native_legacy_stage=8;
    {
        char report[512];unsigned length=native_legacy_clover_report(report,sizeof report);
        if(length && length<sizeof report)IOLog("%s %s unwound\n",cloverRound,report);
    }
    native_legacy_display_checkpoint();
}
}
