// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board-local instrument for the clover GOP display bring-up (round180).
// Citation rule for this file (round332): a claim that came down with the source it was vendored from is marked
// "inherited claim, no record in this repository" instead of cited to an `int-` round number -- neither this
// repository nor the base one carries the ledger those numbers count, so such a label would read as a reference
// nobody can open. Wording that makes the round shipping a file the subject of that file's own comment is not used either.
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
//   * the eleven of the reference's late-entry fan-out that are other boards' work items (keystore, cpufreq,
//     GPU power, touch, WiFi, ADSP, battery, audio, buttons, backlight, DPU) stay out. Round255 adds the
//     twelfth, HID, because that one is this board's blocker: package/CloverHidInputProbe.cpp hangs an owned
//     IOHIDDevice under the original IOHIDResource, which is how the reference board gets past the wait this
//     one dies in. Here the late entry therefore does two things: bring up the framebuffer, and arm the input.
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
// package/CloverHidInputProbe.cpp, in the same archive as this file and the driver it starts.
extern "C" void clover_hid_input_probe(void);
// package/CloverKeyStoreProbe.cpp, same archive, started from the same entry point (round276).
extern "C" void clover_keystore_probe(void);
static IOService *pendingLegacyDisplay,*activeLegacyDisplay,*activeGOPPresenter,*activeGOPNub,*gpuHandoffPresenter;
// GPU hand-off counters the driver writes through these names.
volatile uint32_t native_legacy_gpu_fallbacks,native_legacy_gpu_last_status;
static char cloverRound[]="clover r180:";
// One bounded log channel: the kernel message buffer, which the storage driver's tee carries onto the card. The budget
// is per boot and per event class, never per frame.
static volatile uint32_t cloverLogLines;
// Round329 names the other half of this budget. Boot 113 carried 55 of the 96 lines this channel may
// print, so a line that is missing from the card can mean either "the event never happened" or "the
// event happened after line 96" -- and the r329 row is read beside both. Counted here, at the one
// place that knows it dropped something, and published as `logdrop=` on the periodic row.
static volatile uint32_t cloverLogDropped;
/* Screen statistics sampled by the driver's state beat: non-black samples, samples, and a hash. */
extern "C" volatile uint32_t native_legacy_screen_nonblack,native_legacy_screen_sampled;
extern "C" volatile uint64_t native_legacy_screen_hash;
/* Round414: what the panel holds, not just whether it changed. The driver's sample pass fills eight slots with
 * the first eight distinct colours it meets and counts each; `other` counts samples outside all eight. The hash
 * above says the frame is different, which is not a reading of the frame -- and r376's colour rows are printed
 * once per 128 frames at event time, so they age out of the log ring's window long before the panel is worth
 * reading. The driver owns the storage (its extern "C" block at the top of that file), so this unit only names
 * them; a definition here would be a duplicate symbol in the forced-load archive. */
extern "C" volatile uint32_t native_legacy_screen_colors[8];
extern "C" volatile uint64_t native_legacy_screen_color_counts[8];
extern "C" volatile uint64_t native_legacy_screen_color_other;
/* Selector 6 entries minus returns: a positive value is a swap still inside the call. */
static volatile uint32_t sel6Enter,sel6Exit;
/* Selector 6 answers kIOReturnBusy that were returned to the client as success. */
static volatile uint32_t sel6Forced;
// Round416: the same beat's second row. The screen words are the driver's; the failure clock is this file's,
// because boot 188's fails=15 was already constant at the ring's first row and a count can never date it.
extern "C" volatile uint32_t native_legacy_screen_lit,native_legacy_screen_alpha0,native_legacy_screen_alpha255;
extern "C" volatile uint32_t native_legacy_screen_box[4],native_legacy_screen_box_seen;
static volatile uint32_t sel6FailCount;
static volatile uint64_t sel6FailFirstUs,sel6FailLastUs;
/* Successful scanout transfers and the time of the last one. */
static volatile uint32_t transferOkCount,transferOkTime;
/* Round414: the failures, which until now were only ever printed on the early-only log channel. The last
 * refusal's status, call site and site detail are kept so the periodic row can name where the copy stopped. */
static volatile uint32_t transferFailCount,transferFailStatus,transferFailSite,transferFailDetail;
enum {kCloverLogBudget=96};
static void cloverLog(const char *format,...) __attribute__((format(printf,1,2)));
static void cloverLog(const char *format,...) {
    if(cloverLogLines>=kCloverLogBudget){cloverLogDropped=cloverLogDropped+1;return;}
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
volatile uint64_t native_legacy_retirement_calls,native_legacy_latch,native_legacy_latch_copy,native_legacy_fc;
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
/* The latest presented frame, kept every frame so a periodic row can say what the panel holds late. */
volatile uint32_t native_legacy_last_kind,native_legacy_last_time;
volatile uint64_t native_legacy_last_box[4];
volatile uint32_t native_legacy_frame_kind[NATIVE_LEGACY_FRAMES];
volatile uint32_t native_legacy_frame_time[NATIVE_LEGACY_FRAMES];
volatile uint64_t native_legacy_frame_stats[NATIVE_LEGACY_FRAMES][5];
static uint64_t frameEpoch;
/*
 * Presentation time only: the content scans stay budgeted to the first frames, but whether a frame was
 * presented at all must keep being answered, or a boot that stopped presenting looks like a boot whose
 * scans were merely spent.
 */
void native_legacy_frame_presented(unsigned kind) {
    uint64_t now,frequency;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(now));__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
    if(!frameEpoch)frameEpoch=now;
    native_legacy_last_kind=kind;
    native_legacy_last_time=frequency?(uint32_t)((now-frameEpoch)/frequency):0;
    native_legacy_last_box[0]=0;native_legacy_last_box[1]=0;
    native_legacy_last_box[2]=0;native_legacy_last_box[3]=0;
}
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
    native_legacy_last_kind=kind;
    native_legacy_last_time=frequency?(uint32_t)((now-frameEpoch)/frequency):0;
    native_legacy_last_box[0]=minx;native_legacy_last_box[1]=miny;
    native_legacy_last_box[2]=maxx;native_legacy_last_box[3]=maxy;
    native_legacy_frame_count=n+1;
    /*
     * Every 128th frame after the opening burst is logged too, so a boot that runs to its deadline says
     * what the panel is showing late instead of only what the first six frames showed.
     */
    if(n<6 || (n>=128 && (n%128)==0))
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
void native_legacy_transfer_observed(IOReturn status,const void *request,uint64_t bytes,unsigned site,uint32_t detail) {
    native_legacy_transfer_calls=native_legacy_transfer_calls+1;
    if(status==kIOReturnSuccess) {
        uint64_t now,frequency;
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(now));__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
        if(!frameEpoch)frameEpoch=now;
        transferOkCount=transferOkCount+1;
        transferOkTime=frequency?(uint32_t)((now-frameEpoch)/frequency):0;
    } else {
        transferFailCount=transferFailCount+1;
        transferFailStatus=(uint32_t)status;transferFailSite=site;transferFailDetail=detail;
    }
    if(status==kIOReturnSuccess)native_legacy_swap_stage(2);
    native_legacy_transfer_status=status;native_legacy_transfer_request=(uintptr_t)request;native_legacy_transfer_bytes=bytes;
    if(status==kIOReturnSuccess) {
        if(native_legacy_transfer_calls<=4)
            cloverLog("scanout#%u ok bytes=%llu retired=%llu fc=%llu",native_legacy_transfer_calls,(unsigned long long)bytes,
                (unsigned long long)native_legacy_retirement_calls,(unsigned long long)native_legacy_fc);
    } else
        cloverLog("scanout#%u FAILED status=%08x site=%u detail=%x prepare=%u tries=%u bytes=%llu",native_legacy_transfer_calls,(unsigned)status,
            site,detail,native_legacy_prepare_reason,native_legacy_prepare_attempts,(unsigned long long)bytes);
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
    uint64_t current=0,pending=0;uint32_t latch=0,copy=0,fc=0;
    memcpy(&current,bytes+5472,8);memcpy(&pending,bytes+5480,8);
    // +0x16c0 is assigned from +0x16bc one instruction after the latch store (0x678aea0), so copy= is the same
    // quantity and not a second leg; +0xfc is the counter the completion path increments once per run (0x678ab70).
    memcpy(&latch,bytes+5820,4);memcpy(&copy,bytes+5824,4);memcpy(&fc,bytes+252,4);
    native_legacy_retired_current=current;native_legacy_retired_pending=pending;
    native_legacy_latch=latch;native_legacy_latch_copy=copy;native_legacy_fc=fc;
    native_legacy_retirement_calls=native_legacy_retirement_calls+1;
    if(native_legacy_retirement_calls<=4)
        cloverLog("retire#%llu latch=%x copy=%x fc=%x current=%llx pending=%llx",
            (unsigned long long)native_legacy_retirement_calls,latch,copy,fc,
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
    unsigned regs;
    // Round329: `regs` counts what this record kept (at most kRegEvents), so it saturates and cannot answer
    // "how many registrations arrived". `regCalls` counts every call, kept or not; the two together are the
    // difference between a client that registered once and one that registered eleven times and filled the slots.
    uint32_t regCalls;
    struct {uint32_t type;uint64_t port;uint32_t ref,status;} reg[kRegEvents];
    uint64_t sel6_first[3],sel6_last[3];uint32_t sel6_distinct;uint64_t sel6_seen[8][3];uint32_t sel6_calls[8],sel6_fails[8],sel6_status[8];
    uint32_t seq_count;struct {uint32_t a,b,c,status,latch,copy,fc;} seq[24];
    uintptr_t original_external,original_register;
    // Round333: the class size this object was allocated with. Every byte the swap-wait row reads out of the
    // client is gated on it, so a column says "this address is not provably inside the object" instead of
    // reading whatever follows it.
    uint32_t classSize;
    // Round333: per-swapID pairing, the record's whole job being to answer "is this the second call with the
    // same latched swapID" without the reader having to line two rows up by hand.
    struct {uint32_t id;uint32_t calls;uint64_t lastMs;} swaps[8];uint32_t swapKinds;
};
static ClientStat seenClients[kSeenClients];static unsigned seenClientCount;
typedef IOReturn (*ExternalFn)(IOUserClient *,uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *);
typedef IOReturn (*RegisterFn)(IOUserClient *,mach_port_t,UInt32,UInt32);
static ClientStat *statFor(IOUserClient *client) {
    for(unsigned i=0;i<seenClientCount;++i)if(seenClients[i].object==(uintptr_t)client)return &seenClients[i];
    return nullptr;
}
/*
 * Round333. What the swap wait reads out of the two objects, taken before the call and printed after it.
 *
 * Why this row exists. Boot 113 and boot 115 both end the same way: `selector 6 ... -> e00002d5` (the family's
 * own busy return) with no further swap, and every row the card has about this client answers a question about
 * the *call* -- arguments, return, latch/copy -- but none has ever said what the client's own waiting
 * machinery holds. The stock family that produces that busy return also owns a 5 s assertion
 * ("swap_wait_gated blocked for more than 5 seconds. Should not happen. Panicking"), so a boot that stalls in
 * this wait either answers the wait or dies saying so, and today the card cannot tell which half it is missing.
 *
 * The four cells below come from the host forensic pass over this board's own IOMobileFramebuffer image
 * (~/clover-out/round333-forensics/, decoded instruction by instruction, not from any other board's source):
 *   client+0xf8    the latched swapID, written as a 32-bit word (STR W,[X,#0xf8]) by the same agent that owns
 *                  the slot array, which is why it is read as a dword and printed unsigned.
 *   client+0x110   the wake-leg array: stride 0xb0, and the family walks it with
 *                  `add x8,x8,#0x110` / `movz x10,#0xb0` / `ldr x8,[x8,x9,lsl #4]`, so each slot is read here
 *                  as the qword at its base. A taken slot returns kIOReturnPortExists (0xe00002dd), so the
 *                  array is a port table, not a counter.
 *   +0x16d0/+0x16e0 of the display object (5840/5856) the two words the family's third and fourth special
 *                  queries test; every reference in the image is LDR W/STR W, so both are dwords, and they are
 *                  the same pair native_legacy_state_words walks.
 * The fifth cell is arithmetic, not a read: gap=/pair= are this record's own history, kept so that "the second
 * call with the same swapID" is a column rather than something a reader has to line up across rows by hand --
 * which is what boot 115 proved cannot be assumed.
 *
 * Column names, each for the expression that fills it (and the reason two of them are not what the round was
 * first sketched to print):
 *   ms=      probeNowUs()/1000. `t=` is not used, for r329's reason: this file's `t=` is seconds since the
 *            first frame and the kernel rows' `t=` is seconds since boot.
 *   opt=     scalarInput[1] in the raw, %llx: bit31 and the low bits are the two halves the family reads
 *            differently, so the word is printed rather than decoded (boot 113's `80000001` and boot 115's
 *            `80000000` are both this one word).
 *   ret=     what the original externalMethod returned for this call.
 *   arg=     scalarInput[0], the swapID the CALLER handed in, printed as the 32-bit word the family stores.
 *            Round333 read `swap=` from client+0xf8 instead; boot 118's helper pass (round334-forensics)
 *            showed that dword is written at the entry and cleared before the return, so `swap=0` was only
 *            ever "the last call cleaned it", not the argument -- this column names the input directly.
 *   latch=   the dword at display+0x16bc (5820): the swapID the family's completion path last stored there.
 *            `absent` when the display object was never recorded, `off` when its class size does not reach
 *            5820. Round335 called this done= and read it as "finished presenting"; the one visible writer
 *            (0x678ae7c) stores [[fb+0x1560]+0x314] under a [cur+0x32c] state test, so it is a latch.
 *   copy=    the dword at display+0x16c0 (5824), which that same function assigns from +0x16bc at 0x678aea0:
 *            one quantity in two columns, NOT an independent leg. Round335 called it presented=.
 *   fc=      the dword at display+0xfc (252), incremented once per completion-path run at 0x678ab70. This is
 *            the second leg; round329 printed it as frames=, naming a frame count this port does not have.
 *   cur=     the low dword of the qword at display+0x1560 (5472): a POINTER to the current swap object, so
 *            `0` says there is no current object, not "swapID 0 is the current one".
 *   list=    the low dword of the qword at display+0x1568 (5480): the pending-chain head POINTER, same reading.
 *            latch=/copy=/fc=/cur=/list= are read before the call, same as pw/ps; B2's criterion is arg==latch
 *            on a busy return, so all five must be this boot's own reads, not carried over from a prior row.
 *   pw= ps=  the two queried words, read before the call ran; `absent` says the display object was never
 *            recorded, `off` says its class size does not reach that offset -- three states, three words.
 *   np0..np5 the qword at client+0x110+k*0xb0: the value, `null` for a zero word (the family's own test is
 *            `cmp x8,#0`), or `off` when the slot exceeds the class size.
 *   sent=    vsyncEvents[1] -- the driver's own count of entering the original vsync sender. The sketch called
 *            this column notified=; this port cannot count the sender being entered from inside the family
 *            (its text is not touched), so the column is named for the one producer that does exist. It is the
 *            same quantity as the beat row's third `vs=` field, read here at event time instead of at the beat.
 *   s72=     this client's selector-72 entries -- the registration that arms the tick, which no row has ever
 *            printed. Zero with a non-null `np` is the reading branch B was built for.
 *   gap=     ms since the previous call carrying this same arg (the input swapID, no longer the transient
 *            latch); `none` for an arg this record has not seen before, `full` when the eight slots are used up.
 *   pair=    `first` for that arg's first call, `again` for any later one, `full` when the tracking is out of
 *            slots; the `off` case left with swap=, because arg is an in-parameter and is always known.
 *   pid=     whose client this is. Not in the sketch: with up to eight clients interposed, a row that names
 *            none of them makes two clients' swap sequences read as one.
 *
 * The row is printed with IOLog directly, not through cloverLog: the 96-line budget is per boot and this row
 * fires once per wait call, so routing it there would let the swap rows spend the budget that the beat row
 * exists to keep. Its own bound is the tee class, and the tee says out loud when that cap fills.
 */
// Round414: kSwapWaitRowBytes was 320, which is the tee's whole line budget with nothing left for the msgbuf
// stamp, so a row at this ceiling was cut downstream where this file's own cut branch could not see it. 300 is
// the same bound the state and panel rows use. It costs this row nothing: the widest of the 308 bbswap rows on
// the archived cards is 203 characters (boot 169), and the cut branch below still answers the wider operand
// case -- six 16-hex-digit `np` slots can push the format past any ceiling, which is why the outlet exists.
enum {kWakeSlotBase=0x110,kWakeSlotStride=0xb0,kWakeSlots=6,kSwapWaitRowBytes=300,kSwapTracked=8};
enum {kLatchOffset=5820,kCopyOffset=5824,kFcOffset=252,kCurOffset=5472,kListOffset=5480};
static const unsigned kQueryOffsets[2]={5840,5856};
struct SwapWaitPre {
    uint64_t ms,option;uint32_t arg;
    char argTxt[16],dispTxt[5][16],queryTxt[2][16],slotTxt[kWakeSlots][24];
};
static const unsigned kDisplayOffsets[5]={kLatchOffset,kCopyOffset,kFcOffset,kCurOffset,kListOffset};
static void readSwapWaitState(ClientStat &stat,const uint64_t *in,SwapWaitPre &pre) {
    memset(&pre,0,sizeof pre);
    pre.ms=probeNowUs()/1000;pre.option=in[1];pre.arg=(uint32_t)in[0];
    snprintf(pre.argTxt,sizeof pre.argTxt,"%u",pre.arg);
    const unsigned char *client=reinterpret_cast<const unsigned char *>(stat.object);
    const unsigned char *display=reinterpret_cast<const unsigned char *>(native_legacy_object);
    for(unsigned d=0;d<5;++d) {
        if(!display)snprintf(pre.dispTxt[d],sizeof pre.dispTxt[d],"absent");
        else if(native_legacy_object_size<kDisplayOffsets[d]+4)snprintf(pre.dispTxt[d],sizeof pre.dispTxt[d],"off");
        else {uint32_t value=0;memcpy(&value,display+kDisplayOffsets[d],4);snprintf(pre.dispTxt[d],sizeof pre.dispTxt[d],"%x",value);}
    }
    for(unsigned q=0;q<2;++q) {
        if(!display)snprintf(pre.queryTxt[q],sizeof pre.queryTxt[q],"absent");
        else if(native_legacy_object_size<kQueryOffsets[q]+4)snprintf(pre.queryTxt[q],sizeof pre.queryTxt[q],"off");
        else {uint32_t value=0;memcpy(&value,display+kQueryOffsets[q],4);snprintf(pre.queryTxt[q],sizeof pre.queryTxt[q],"%x",value);}
    }
    for(unsigned k=0;k<kWakeSlots;++k) {
        unsigned offset=kWakeSlotBase+k*kWakeSlotStride;
        if(offset+8>stat.classSize){snprintf(pre.slotTxt[k],sizeof pre.slotTxt[k],"off");continue;}
        uint64_t value=0;memcpy(&value,client+offset,8);
        if(!value)snprintf(pre.slotTxt[k],sizeof pre.slotTxt[k],"null");
        else snprintf(pre.slotTxt[k],sizeof pre.slotTxt[k],"%llx",(unsigned long long)value);
    }
}
static void reportSwapWait(ClientStat &stat,const SwapWaitPre &pre,IOReturn status) {
    const char *gapTxt="none",*pairTxt="first";
    char gapValue[24];
    {
        unsigned at=0;
        while(at<stat.swapKinds && stat.swaps[at].id!=pre.arg)++at;
        if(at<stat.swapKinds) {
            uint64_t delta=pre.ms>=stat.swaps[at].lastMs?pre.ms-stat.swaps[at].lastMs:0;
            snprintf(gapValue,sizeof gapValue,"%llu",(unsigned long long)delta);gapTxt=gapValue;
            pairTxt="again";
            stat.swaps[at].calls=stat.swaps[at].calls+1;stat.swaps[at].lastMs=pre.ms;
        } else if(at<kSwapTracked) {
            stat.swaps[at].id=pre.arg;stat.swaps[at].calls=1;stat.swaps[at].lastMs=pre.ms;stat.swapKinds=at+1;
        } else gapTxt="full",pairTxt="full";
    }
    char row[kSwapWaitRowBytes];
    int length=snprintf(row,sizeof row,
        "clover r343: bbswap ms=%llu opt=%llx ret=%08x arg=%s latch=%s copy=%s fc=%s cur=%s list=%s pw=%s ps=%s "
        "np0=%s np1=%s np2=%s np3=%s np4=%s np5=%s sent=%llu s72=%u gap=%s pair=%s pid=%d\n",
        (unsigned long long)pre.ms,(unsigned long long)pre.option,(unsigned)status,
        pre.argTxt,pre.dispTxt[0],pre.dispTxt[1],pre.dispTxt[2],pre.dispTxt[3],pre.dispTxt[4],
        pre.queryTxt[0],pre.queryTxt[1],
        pre.slotTxt[0],pre.slotTxt[1],pre.slotTxt[2],pre.slotTxt[3],pre.slotTxt[4],pre.slotTxt[5],
        (unsigned long long)vsyncEvents[1],(unsigned)stat.selector_calls[72],gapTxt,pairTxt,stat.pid);
    if(length<0 || static_cast<unsigned>(length)>=sizeof row) {
        char cut[96];
        snprintf(cut,sizeof cut,"clover r343: bbswapcut bytes=%d cap=%u pid=%d arg=%s\n",
            length,(unsigned)sizeof row,stat.pid,pre.argTxt);
        IOLog("%s",cut);
        return;
    }
    IOLog("%s",row);
}
static IOReturn wrappedExternal(IOUserClient *client,uint32_t selector,IOExternalMethodArguments *arguments,IOExternalMethodDispatch *dispatch,OSObject *target,void *reference) {
    ClientStat *stat=statFor(client);
    // Round333 reads the swap-wait state before the call runs, because selector 6 is the call that changes it.
    SwapWaitPre pre;
    bool waitCall=stat && selector==6 && arguments && arguments->scalarInput && arguments->scalarInputCount>=3;
    if(waitCall)readSwapWaitState(*stat,arguments->scalarInput,pre);
    if(selector==6)sel6Enter=sel6Enter+1;
    IOReturn status=stat?reinterpret_cast<ExternalFn>(stat->original_external)(client,selector,arguments,dispatch,target,reference):kIOReturnNotAttached;
    if(selector==6)sel6Exit=sel6Exit+1;
    /*
     * A busy swap is a retryable answer the compositor may treat as a backoff. Hand it success instead
     * so the loop keeps submitting, and count the substitutions so the row says how many were made.
     */
    if(selector==6 && status==kIOReturnBusy) { sel6Forced=sel6Forced+1; status=kIOReturnSuccess; }
    if(selector==6 && status) {
        const uint64_t whenUs=probeNowUs();
        if(!sel6FailCount)sel6FailFirstUs=whenUs;
        sel6FailLastUs=whenUs;
        sel6FailCount=sel6FailCount+1;
    }
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
                entry.latch=entry.copy=entry.fc=0;
                if(object){memcpy(&entry.latch,object+kLatchOffset,4);memcpy(&entry.copy,object+kCopyOffset,4);memcpy(&entry.fc,object+kFcOffset,4);}
            }
            if(at>=0){++stat->sel6_calls[at];if(status)++stat->sel6_fails[at];stat->sel6_status[at]=(uint32_t)status;}
            if(at>=0 && stat->seq_count && stat->seq_count<=8)
                cloverLog("client pid=%d selector 6 %llx %llx %llx -> %08x latch=%x copy=%x fc=%x",stat->pid,
                    (unsigned long long)in[0],(unsigned long long)in[1],(unsigned long long)in[2],(unsigned)status,
                    stat->seq[stat->seq_count-1].latch,stat->seq[stat->seq_count-1].copy,stat->seq[stat->seq_count-1].fc);
            reportSwapWait(*stat,pre,status);
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
    if(stat) {
        ++stat->regCalls;
        if(stat->regs<kRegEvents){
            auto &entry=stat->reg[stat->regs++];entry.type=type;entry.port=(uint64_t)(uintptr_t)port;entry.ref=reference;entry.status=(uint32_t)status;
            cloverLog("client pid=%d registerNotification type=%u ref=%x port=%llx -> %08x",stat->pid,type,reference,
                (unsigned long long)(uintptr_t)port,(unsigned)status);
        }
    }
    return status;
}
// Round329's periodic state row, defined below with its columns. The beat that drives it is the display
// driver's own 30 s work-loop timer; this file also fires it once for every client interposed, which is
// what separates "the beat stopped" from "the channel carried nothing".
static void reportDisplayState(void);
static void interpose(IOUserClient *client,int pid) {
    if(seenClientCount>=kSeenClients)return;
    ClientStat &stat=seenClients[seenClientCount];
    memset(&stat,0,sizeof stat);
    uintptr_t *vtable=*reinterpret_cast<uintptr_t **>(client);
    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kVtableSlots*sizeof(uintptr_t)));
    if(!copy)return;
    memcpy(copy,vtable,kVtableSlots*sizeof(uintptr_t));
    stat.object=(uintptr_t)client;stat.pid=pid;stat.original_external=copy[168];stat.original_register=copy[175];
    stat.classSize=(uint32_t)client->getMetaClass()->getClassSize();
    copy[168]=(uintptr_t)&wrappedExternal;copy[175]=(uintptr_t)&wrappedRegister;
    ++seenClientCount;
    *reinterpret_cast<uintptr_t **>(client)=copy;
    reportDisplayState();
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
            at+=snprintf(out+at,cap-at,"  s6#%u %x %x %x -> %x [5820]=%x [5824]=%x [252]=%x\n",k,stat.seq[k].a,stat.seq[k].b,stat.seq[k].c,
                stat.seq[k].status,stat.seq[k].latch,stat.seq[k].copy,stat.seq[k].fc);
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
/*
 * Round329. The display client's state, published on the display's own clock instead of only beside the
 * events that happened to be first.
 *
 * Why this row exists. Boot 113 read the desktop cell back as four accepted swaps and a last wait
 * (`selector 6 4 80000001 0 -> 00000000`) with no fifth swap after it, and that is the whole of what the
 * card can say -- every r180 row about this client is printed on a first sighting or on the first four
 * events, so after the stall the channel goes silent and silence reads two ways: "backboardd stopped
 * asking" and "nothing new happened to print about". The four report helpers sheng's probe exports
 * (`prepare_report`, `properties_now`, `state_words`, `clients_dump`) are what answers that on the
 * reference board -- its package/BootLog.c pulls all four into a report -- and here they have no caller
 * at all. Round329 recorded that as an open debt rather than closing it (dev-log §Round329 ②.4, and the
 * gap row in README); what that round added is this row, which reads the same client counters and the
 * same three object words on the display's own clock.
 *
 * What the columns are, each named for the expression that fills it:
 *   ms=        probeNowUs()/1000: milliseconds since boot, from absolutetime_to_nanoseconds. Not `t=` --
 *              this file's existing `t=` means seconds since the first frame and the kernel rows' `t=`
 *              means seconds since boot; a third meaning for the same word is boot 100's `t=` defect.
 *   fires=     this row's own ordinal, incremented before formatting, so the cut branch and the row
 *              share one numbering and a gap in the numbering is a gap in the schedule.
 *   seen=      seenClientCount, written by the unconditional `interpose` path.
 *   client=    which record the client columns below describe, printed as `pid<N>` of the record the picker
 *              chose (`pick=`) or `none` while no client has been interposed. When it is `none` every client
 *              column prints `-`, because `-` is the difference between "nobody was there to ask" and the
 *              zero that a client that asked nothing would print (r327's rule). Round344 (#695) replaced a
 *              hardcoded pid 58 here: boot 123 had five records and this row kept describing the idle one.
 *   pick=      the score the picker used, with its unit: `<n>g` = that record grew by n since this row last
 *              sampled it, `<n>t` = the record had no earlier sample, so n is its total calls (a different
 *              quantity, which is why the suffix is there and why `grew=` prints `-` in that case). The
 *              selection is the largest score, ties broken by total calls, then by record order; `-` = no
 *              record to pick. Printing it is what makes `client=` answerable -- a row that selects by a
 *              number it never shows cannot be read.
 *   grewn=     how many records grew since this row's last fire; `-` while no record has an earlier sample
 *              (the first fire of the boot), for the same reason `grew=` is `-` there.
 *   p58=       pid 58's own growth since this row's last sample of it; `-` while there is no pid-58 record
 *              or it has no earlier sample. Boots 115/116/117 were read through pid58's columns, and this
 *              column keeps that reading available after `client=` stopped meaning pid 58.
 *   calls=     ClientStat::calls -- externalMethod entries, counted with no cap.
 *   fails=     the same walk's nonzero returns.
 *   regall=    ClientStat::regCalls: every registerNotificationPort call. `regs=` counts only what the
 *              record kept (kRegEvents slots), so the two together separate "registered once" from
 *              "registered eleven times and filled the slots".
 *   grew=      the selected record's calls minus the calls this row printed for it last fire; `-` when that
 *              record has no earlier sample, because total-calls is a different quantity.
 *   hot=       the selector with the most calls this boot, as `<selector>:<calls>/<fails>/<last status>`,
 *              or `none` while every selector count is zero.
 *   s6=        the last selector-6 wait: sel6_last (the three 64-bit scalarInput words the call itself
 *              carried, same memcpy as the event row) with selector_status[6], the last return the same
 *              walk recorded. Conditioned on selector_calls[6], the count that fills both.
 *   latch= copy= fc=   the display object's words at 5820/5824/252 -- the same three offsets, read the same
 *              way, that retire# prints at event time. copy= is +0x16bc's own copy (stored at 0x678aea0), so
 *              it is not a second leg; fc= is the completion-path counter at +0xfc. Here they are read every
 *              fire, so "the latch is still 4" keeps being said after the event row has aged out of the ring.
 *   vs= log= logdrop=   moved to the r414 panel row below in round414, unchanged in meaning: <hz>/<started>/
 *              <sent> from vsyncHz and vsyncEvents[4]/[1] (a producer that always exists in this file, so
 *              vs=0/0/0 is a reading), and cloverLogLines against kCloverLogBudget with the lines that budget
 *              refused. They left this row for width, not for relevance: the row was 317 characters and the
 *              tee keeps 304 of the stamped line, so the tail -- which was exactly these three columns --
 *              never reached the card. A row that loses its own budget columns cannot say it was cut.
 *
 * Round414 adds a third row on the same beat, `clover r414: panel`, carrying what this row lost plus the two
 * things the boot could not otherwise be read for:
 *   txa=       native_legacy_transfer_calls: every scanout observation, success or refusal. `tx=` on the state
 *              row counts successes only, so `txa - tx` is the refusal total -- and until this row existed the
 *              refusals were only ever printed by cloverLog, whose 96-line budget boot 171 had already spent
 *              (`log=66/96`) long before the panel was worth reading.
 *   txf=       transferFailCount, the same refusals counted at the one place that sees them. Printed beside
 *              txa so the two ways of reaching the number can disagree visibly rather than silently.
 *   txe= txs= txd=   the last refusal's IOReturn status, its call site, and that site's detail word. The sites
 *              are the driver's own numbering: 1 blank console/bounds, 2 blank prepare_scanout, 3 blank
 *              fill_pixels, 4 the surface compound guard (txd is its 13-bit refusal mask), 5 pre-copy
 *              prepare_scanout, 6 source map, 7 copy_layout, 8 post-retire prepare_scanout, 9 copy_pixels,
 *              10 a surface that is not an IOSurface. `-` while nothing has been refused. This is what turns
 *              "the copy failed" into "the copy failed here", which is the question the round is about.
 *   col=       the four most-sampled colours of the panel as `<%08x>/<count>` pairs, most first, from the
 *              driver's eight first-seen slots; `oth=` counts the samples outside all eight. `scr=` on the
 *              state row says the frame changed (nonblack/sampled/hash); this says what it changed into.
 *              `-` while no sample has been taken. r376's colour rows answer this at event time and age out
 *              of the ring's window, so a late boot had no colour reading at all.
 *   vs= log= logdrop=   as above, now on a row whose width they fit inside.
 *
 * The panel row has its own ceiling and its own `clover r414: panelcut` outlet, for the same reason: a row
 * cannot report its own truncation from inside the truncated part.
 *
 * Two trigger sites, one producer: the driver's 30 s work-loop beat, and the first client interposed.
 * The second is not a cadence, it is the discriminator -- a boot that shows fires=1 at the client's
 * arrival and nothing after has lost the beat (the display's work loop stopped running), while a boot
 * that shows no row at all never got the client, and the tee's own cap message says which.
 *
 * Round344 adds a second row, `clover r344: dispclient idx= pid= calls= fails= regall= regs= grew= hot= s6=`,
 * one record per line, printed for every record that is new or that grew since its own last sample -- the
 * shape the reference uses (it dumps all records into one report) cut down to one card record per line. The
 * state row still prints one record's columns, but it no longer decides *which* record the boot is about
 * without saying so: `pick=` is the number it chose by, and the per-record rows mean nothing can be hidden
 * behind that choice.
 *
 * Round414 corrects the ceiling this row is written against, and the correction is a measurement, not a
 * preference. The comment that used to stand here said both rows sit inside one card record
 * (512 - 23-byte prefix - 1 = 488) and therefore cannot be split. Two things were wrong with that. The card
 * record's body bound is 480, not 488 (SdhciLogCore.h kRecordBodyBytes), and -- the one that mattered -- the
 * record is not the narrowest thing on the path. The kernel ring's tee assembles a line at kVerdictTextBytes
 * = 320 characters, and that count includes the msgbuf stamp the kernel prepends (`[ 1790.633354]: ` is 16
 * characters), so 304 characters of the row survive and the rest is discarded up to the newline and counted
 * as one truncation. Boot 171's card is the reading (clover-out/round387-flash-boot171.log): all 22 of its
 * dispstate rows have an incomplete tail column, 21 of them at exactly 304 visible characters and one at
 * 303, and they end `log=66/96` or `log=66` where this format writes `log=66/96 logdrop=0`. The row's own
 * 384 ceiling never noticed, so `dispstatecut` printed zero times on a boot that was cut on every fire --
 * which is precisely the ambiguity the cut row exists to remove.
 *
 * Both ceilings are therefore 300: 300 + a 17-character stamp (five-digit seconds, the widest this board's
 * unattended window can produce) is 317, inside the 320 the tee keeps. A row that still does not fit prints
 * `clover r344: dispstatecut` with the width it needed, and that row is the arithmetic's mandatory outlet --
 * the long row cannot say it was cut. The width is checked by scripts/read-clover-log.py against the tee's
 * own bound rather than against the card record's, so a row grown past 300 fails on the host.
 */
static uint32_t dispstateFires;
enum {kDispStateRowBytes=300,kDispClientRowBytes=300};
static uint32_t dispstateReportedCalls[kSeenClients];
static bool dispstateReported[kSeenClients];
static void formatHotFor(const ClientStat &s,char *out,unsigned cap) {
    unsigned best=0;uint32_t bestCalls=0;
    for(unsigned k=0;k<kSelectors;++k)if(s.selector_calls[k]>bestCalls){bestCalls=s.selector_calls[k];best=k;}
    if(bestCalls)snprintf(out,cap,"%u:%u/%u/%08x",best,bestCalls,s.selector_fails[best],s.selector_status[best]);
    else snprintf(out,cap,"none");
}
static bool formatSel6For(const ClientStat &s,char *out,unsigned cap) {
    if(!s.selector_calls[6])return false;
    snprintf(out,cap,"%llx %llx %llx -> %08x",(unsigned long long)s.sel6_last[0],
        (unsigned long long)s.sel6_last[1],(unsigned long long)s.sel6_last[2],s.selector_status[6]);
    return true;
}
// Round344 (#695). Boot 123 is why this row exists: five records were interposed (pid 58/86/87/56/74) and the
// state row printed `client=pid58 calls=0` five times while the other records were the ones asking. The fix is
// the reference's own answer, not a better picker: sheng's native_legacy_clients_dump() prints every record
// (package/LegacyDisplayProbe.cpp:367) and its BootLog pulls that into one report. This port has no report
// buffer -- one card record is 512 bytes, so one line per record is the shape that fits, and a record is
// printed when it is new or when it grew since its own last sample. Nothing here selects a record to hide.
static void reportDisplayClient(unsigned index,uint32_t grew) {
    const ClientStat &s=seenClients[index];
    char hot[40],s6[96];
    formatHotFor(s,hot,sizeof hot);
    const bool hasSel6=formatSel6For(s,s6,sizeof s6);
    char row[kDispClientRowBytes];
    int length=snprintf(row,sizeof row,
        "clover r344: dispclient idx=%u pid=%d calls=%u fails=%u regall=%u regs=%u grew=%u hot=%s s6=%s\n",
        index,s.pid,s.calls,s.fails,s.regCalls,s.regs,grew,hot,hasSel6?s6:"-");
    if(length<0 || static_cast<unsigned>(length)>=sizeof row) {
        char cut[96];
        snprintf(cut,sizeof cut,"clover r344: dispclientcut bytes=%d cap=%u idx=%u pid=%d\n",
            length,(unsigned)sizeof row,index,s.pid);
        IOLog("%s",cut);
        return;
    }
    IOLog("%s",row);
}
enum {kPanelRowBytes=300};
// Round414: the four most-sampled colours, most first. The driver's eight slots are in first-seen order, which
// is not importance order -- the first sample of a boot can be a stray pixel -- so the pick is by count and a
// slot with no samples is skipped rather than printed as black. Counts print as %u because the sample pass
// bounds them: its stride is n/4096 and at least 1, so at most 8191 samples are taken and neither a colour
// count nor `oth` can exceed that. 64 bytes holds four `%08x/%u` pairs at that bound (55 characters), so this
// row's width is bounded by its own operands rather than by a hope about the readings.
static void formatPanelColors(char *out,unsigned cap) {
    uint32_t colors[8];uint64_t counts[8];
    for(unsigned k=0;k<8;++k) {colors[k]=native_legacy_screen_colors[k];counts[k]=native_legacy_screen_color_counts[k];}
    unsigned order[8],used=0;
    for(unsigned k=0;k<8;++k)if(counts[k])order[used++]=k;
    for(unsigned i=0;i<used;++i)
        for(unsigned j=i+1;j<used;++j)
            if(counts[order[j]]>counts[order[i]]) {unsigned swap=order[i];order[i]=order[j];order[j]=swap;}
    if(!used) {snprintf(out,cap,"-");return;}
    unsigned at=0;
    for(unsigned i=0;i<used && i<4;++i) {
        int written=snprintf(out+at,cap-at,"%s%08x/%u",i?" ":"",colors[order[i]],(unsigned)counts[order[i]]);
        if(written<0 || static_cast<unsigned>(written)>=cap-at)break;
        at+=static_cast<unsigned>(written);
    }
}
// The second half of the state beat. `fires=` is dispstateFires, the same ordinal the state row prints, so the
// two rows of one beat are joinable on the card without a timestamp guess.
static void reportPanelState(uint64_t now) {
    char col[64],txe[16],txs[8],txd[16];
    formatPanelColors(col,sizeof col);
    const char *txeTxt="-",*txsTxt="-",*txdTxt="-";
    if(transferFailCount) {
        snprintf(txe,sizeof txe,"%08x",(unsigned)transferFailStatus);txeTxt=txe;
        snprintf(txs,sizeof txs,"%u",(unsigned)transferFailSite);txsTxt=txs;
        snprintf(txd,sizeof txd,"%x",(unsigned)transferFailDetail);txdTxt=txd;
    }
    char row[kPanelRowBytes];
    int length=snprintf(row,sizeof row,
        "clover r414: panel ms=%llu fires=%u txa=%u txf=%u txe=%s txs=%s txd=%s col=%s oth=%u "
        "vs=%u/%llu/%llu log=%u/%u logdrop=%u\n",
        (unsigned long long)(now/1000),dispstateFires,(unsigned)native_legacy_transfer_calls,
        (unsigned)transferFailCount,txeTxt,txsTxt,txdTxt,col,(unsigned)native_legacy_screen_color_other,
        vsyncHz,(unsigned long long)vsyncEvents[4],(unsigned long long)vsyncEvents[1],
        (unsigned)cloverLogLines,(unsigned)kCloverLogBudget,(unsigned)cloverLogDropped);
    if(length<0 || static_cast<unsigned>(length)>=sizeof row) {
        char cut[96];
        snprintf(cut,sizeof cut,"clover r414: panelcut bytes=%d cap=%u fires=%u txa=%u\n",
            length,(unsigned)sizeof row,dispstateFires,(unsigned)native_legacy_transfer_calls);
        IOLog("%s",cut);
    } else {
        IOLog("%s",row);
    }
    /*
     * Round416's second row of this beat. `col=` says the panel is mostly black; it cannot say whether the
     * surface is empty or the copy missed the content. lit= and box= separate those two, a0=/a255= say whether
     * the alpha channel was written at all, and s6fail/first/last date the selector-6 failures that the count
     * alone leaves undated.
     */
    char box[40];
    if(native_legacy_screen_box_seen)snprintf(box,sizeof box,"%ux%u..%ux%u",(unsigned)native_legacy_screen_box[0],
        (unsigned)native_legacy_screen_box[1],(unsigned)native_legacy_screen_box[2],(unsigned)native_legacy_screen_box[3]);
    else snprintf(box,sizeof box,"-");
    char boxRow[kPanelRowBytes];
    int boxLen=snprintf(boxRow,sizeof boxRow,
        "clover r416: panelbox ms=%llu fires=%u lit=%u/%u a0=%u a255=%u box=%s s6fail=%u first=%llus last=%llus\n",
        (unsigned long long)(now/1000),dispstateFires,(unsigned)native_legacy_screen_lit,
        (unsigned)native_legacy_screen_sampled,(unsigned)native_legacy_screen_alpha0,
        (unsigned)native_legacy_screen_alpha255,box,(unsigned)sel6FailCount,
        (unsigned long long)(sel6FailFirstUs/1000000ULL),(unsigned long long)(sel6FailLastUs/1000000ULL));
    if(boxLen<0 || static_cast<unsigned>(boxLen)>=sizeof boxRow) {
        char boxCut[96];
        snprintf(boxCut,sizeof boxCut,"clover r416: panelboxcut bytes=%d cap=%u fires=%u\n",
            boxLen,(unsigned)sizeof boxRow,dispstateFires);
        IOLog("%s",boxCut);
    } else {
        IOLog("%s",boxRow);
    }
}
static void reportDisplayState(void) {
    dispstateFires=dispstateFires+1;
    // One delta per record, computed the same way for the picker and for the per-record rows. A record with no
    // earlier sample has reportedCalls==0, so its delta is its total -- that is what `pick=` marks with `t`.
    uint32_t delta[kSeenClients];
    unsigned best=0,grown=0,sampled=0;
    bool have=false;uint32_t bestPick=0,bestTotal=0;
    for(unsigned c=0;c<seenClientCount;++c) {
        const ClientStat &s=seenClients[c];
        delta[c]=s.calls-dispstateReportedCalls[c];
        if(dispstateReported[c]) {++sampled;if(delta[c])++grown;}
        if(!have || delta[c]>bestPick || (delta[c]==bestPick && s.calls>bestTotal)) {
            have=true;best=c;bestPick=delta[c];bestTotal=s.calls;
        }
    }
    const ClientStat *stat=have?&seenClients[best]:nullptr;
    const bool statPrior=have && dispstateReported[best];
    char client[16],pick[24];
    if(stat)snprintf(client,sizeof client,"pid%d",stat->pid);else snprintf(client,sizeof client,"none");
    if(!stat)snprintf(pick,sizeof pick,"-");
    else snprintf(pick,sizeof pick,"%u%s",bestPick,statPrior?"g":"t");
    const char *callsTxt="-",*failsTxt="-",*regsTxt="-",*regallTxt="-",*grewTxt="-",*hotTxt="-",*s6Txt="-";
    const char *latchTxt="-",*copyTxt="-",*fcTxt="-",*grewnTxt="-",*p58Txt="-",*lastTxt="-";
    char lastValue[96];
    char calls[16],fails[16],regs[16],regall[16],grew[16],hot[40],s6[96],latch[16],copy[16],fc[16];
    char grewn[8],p58[16];
    if(stat) {
        snprintf(calls,sizeof calls,"%u",stat->calls);callsTxt=calls;
        snprintf(fails,sizeof fails,"%u",stat->fails);failsTxt=fails;
        snprintf(regs,sizeof regs,"%u",stat->regs);regsTxt=regs;
        snprintf(regall,sizeof regall,"%u",stat->regCalls);regallTxt=regall;
        if(statPrior) {snprintf(grew,sizeof grew,"%u",bestPick);grewTxt=grew;}
        formatHotFor(*stat,hot,sizeof hot);hotTxt=hot;
        if(formatSel6For(*stat,s6,sizeof s6))s6Txt=s6;
    }
    if(sampled) {snprintf(grewn,sizeof grewn,"%u",grown);grewnTxt=grewn;}
    for(unsigned c=0;c<seenClientCount;++c) {
        if(seenClients[c].pid==58 && dispstateReported[c]) {
            snprintf(p58,sizeof p58,"%u",seenClients[c].calls-dispstateReportedCalls[c]);
            p58Txt=p58;break;
        }
    }
    const unsigned char *object=reinterpret_cast<const unsigned char *>(native_legacy_object);
    if(object) {
        uint32_t latchValue=0,copyValue=0,fcValue=0;
        memcpy(&latchValue,object+kLatchOffset,4);memcpy(&copyValue,object+kCopyOffset,4);memcpy(&fcValue,object+kFcOffset,4);
        snprintf(latch,sizeof latch,"%x",latchValue);latchTxt=latch;
        snprintf(copy,sizeof copy,"%x",copyValue);copyTxt=copy;
        snprintf(fc,sizeof fc,"%x",fcValue);fcTxt=fc;
        snprintf(lastValue,sizeof lastValue,"%u@%us:%llux%llu..%llux%llu",(unsigned)native_legacy_last_kind,
            (unsigned)native_legacy_last_time,(unsigned long long)native_legacy_last_box[0],
            (unsigned long long)native_legacy_last_box[1],(unsigned long long)native_legacy_last_box[2],
            (unsigned long long)native_legacy_last_box[3]);
        lastTxt=lastValue;
    }
    char row[kDispStateRowBytes];
    const uint64_t now=probeNowUs();
    int length=snprintf(row,sizeof row,
        "clover r344: dispstate ms=%llu fires=%u seen=%u client=%s pick=%s grewn=%s p58=%s calls=%s fails=%s "
        "last=%s tx=%u@%us s6if=%u s6f=%u scr=%u/%u/%llx "
        "regall=%s regs=%s grew=%s hot=%s s6=%s latch=%s copy=%s fc=%s\n",
        (unsigned long long)(now/1000),dispstateFires,seenClientCount,client,pick,grewnTxt,p58Txt,
        callsTxt,failsTxt,lastTxt,(unsigned)transferOkCount,(unsigned)transferOkTime,(unsigned)(sel6Enter-sel6Exit),(unsigned)sel6Forced,(unsigned)native_legacy_screen_nonblack,
        (unsigned)native_legacy_screen_sampled,(unsigned long long)native_legacy_screen_hash,regallTxt,regsTxt,grewTxt,hotTxt,s6Txt,latchTxt,copyTxt,fcTxt);
    if(length<0 || static_cast<unsigned>(length)>=sizeof row) {
        char cut[96];
        snprintf(cut,sizeof cut,"clover r344: dispstatecut bytes=%d cap=%u fires=%u client=%s\n",
            length,(unsigned)sizeof row,dispstateFires,client);
        IOLog("%s",cut);
    } else {
        IOLog("%s",row);
    }
    reportPanelState(now);
    // The per-record rows and the bookkeeping do not live under the state row's width. A boot whose state
    // row is always cut is exactly the boot that needs the records it cannot see, and an early return here
    // would leave reportedCalls frozen, so every fire would re-print every record as new.
    for(unsigned c=0;c<seenClientCount;++c) {
        if(!dispstateReported[c] || delta[c])reportDisplayClient(c,delta[c]);
        dispstateReportedCalls[c]=seenClients[c].calls;dispstateReported[c]=true;
    }
}
// The driver's beat calls this; it is also called once from the first client interposed, from below.
extern "C" void native_legacy_display_state_beat(void) {reportDisplayState();}
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
    unsigned at=snprintf(out,cap,"stage=%u meta=%d init=%x size=%u parent=%u obj=%llx nub=%llx video %ux%u row=%u depth=%u display=%llu rot=%u scale=%u base=%llx bytes=%llx gop %ux%u map=%llx va=%llx pa=%llx len=%llu reg=%u dt=%u surface=%d found=%u start=%x probe=%u pm=%u/%x swap=%u %u/%u accept=%u reject=%u scanout=%u/%08x/%llu retire=%llu latch=%x copy=%x frames=%u copies=%llu last=%llums",
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
        (unsigned long long)native_legacy_retirement_calls,(unsigned)native_legacy_latch,
        (unsigned)native_legacy_latch_copy,native_legacy_frame_count,(unsigned long long)native_legacy_copy_n,
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
    // Round255, and deliberately first: the HID device waits five seconds for IOHIDResource to appear, and the
    // framebuffer branch below can return without ever running. The two instruments share this entry point the
    // way the reference board's late fan-out does, and neither one's result is an argument for the other.
    clover_hid_input_probe();
    // Round276, for exactly the same reason and in front of the same return: the keystore is the service
    // datamigrator's -waitUntilPreboardDidFinishUnlock is waiting on, so it has to be published before the
    // display hand-off can bail out, and whether the panel comes up says nothing about whether it was.
    clover_keystore_probe();
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
                                // Not a child of the display subsystem's node: the original display code enumerates that node's children, and the
                                // crash that settled this (SpringBoard finding no display and dying with the presenter under that node) is a label
                                // from sheng's own ledger, which is in neither repository -- so this is an
                                // inherited claim, no record in this repository. It is also inert here: sheng-fb-gpu is not in board.toml's
                                // args, so gpuHandoffPresenter is never created and this branch never runs on any image this board ships.
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
