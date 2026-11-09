// ARM CPU on Dynarmic (A32 JIT), for 64-bit hosts. Same interface as arm_emu.c (Unicorn, 32-bit
// x86 hosts); see include/arm_emu.h.
//
// Memory: guest addresses are host addresses (G2H in guest_memory.h), so everything the game can
// see lives in the low 4 GiB: the executable is linked at a low fixed base, and plat_alloc and
// the guest heap only hand out memory below 4 GiB. The JIT reads memory through a 4 KiB page
// table. A page enters the table the first time the guest touches it (like Unicorn's unmapped
// hook in arm_emu.c): data pages are identity-mapped, pages of a loaded host module never are.
//
// Host calls: guest code calls host functions by jumping to their address, as on 32-bit x86. A
// code fetch from a host module page reads an SVC, and the SVC handler calls the host function at
// that address with r0-r3 and twelve stack words as 64-bit integer arguments, then returns to lr.
// Host functions the guest calls take only integer and pointer arguments (s3e_gl.c wraps the GL
// functions that take floats). Functions outside the low 4 GiB (system DLLs: the GL driver, ANGLE)
// get a slot in the SVC page instead (arm_emu_guest_fn).
//
// Code hooks: PreCodeReadHook replaces a hooked instruction with a supervisor call while the JIT
// translates it; the guest's code is never written. After the hook, the original instruction runs
// as one single step (single-step blocks are cached apart from normal ones).
//
// Faults: check_halt_on_memory_access stops the JIT right after an access to unmapped memory, at
// the faulting instruction with its registers, so the fault handler can repair them.
//
// Nesting: Jit::Run is not reentrant. Each nesting level of guest calls on a thread (host code
// called from the guest calling back into it) runs on its own Jit, and each thread has its own.
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/ir/terminal.h>

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>
#include <time.h>

extern "C" {
#include "arm_emu.h"
#include "platform/platform.h"
}
#include "guest_memory.h"

namespace {

using Dynarmic::A32::UserConfig;
using Dynarmic::HaltReason;
using PageTable = std::array<std::uint8_t *, UserConfig::NUM_PAGE_TABLE_ENTRIES>;

constexpr uint32_t PAGE_SIZE_EMU = 0x1000;
constexpr uint32_t SVC_ARM_WORD = 0xef00df00u; // ARM "svc #0xdf00"; its low half is Thumb "svc #0"
constexpr size_t STACK_SIZE = 2 * 1024 * 1024;
constexpr int HOST_ARG_WORDS = 16;
constexpr int MAX_DEPTH = 24;
constexpr unsigned MAX_CPUS = 32; // threads that ever run guest code (exclusive monitor slots)
// SVC page: word 0 .. FAR_FIRST-1 never run (the trap and the call sentinel are its last words);
// from FAR_FIRST on, one word per host function above 4 GiB.
constexpr uint32_t SVC_PAGE_SIZE = 0x10000;
constexpr uint32_t FAR_FIRST = 0x1000 / 4;
constexpr uint32_t FAR_MAX = SVC_PAGE_SIZE / 4 - FAR_FIRST;
constexpr uint32_t HOOK_SWI = 0xffffff01u; // emitted by PreCodeReadHook; no instruction encodes it
constexpr HaltReason HALT_RETURNED = HaltReason::UserDefined2;
constexpr HaltReason HALT_STEP = HaltReason::UserDefined3;
constexpr HaltReason HALT_FATAL = HaltReason::UserDefined4;

bool has(HaltReason value, HaltReason flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// ---- host calls ----

using HostFn16 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t);

// Integer and pointer arguments only: every 64-bit ABI we build for (x86-64 SysV and Windows,
// AArch64) then passes the n-th argument in the same place whatever the callee's prototype.
uint64_t call_host16(uintptr_t fn, const uint64_t *a) {
    return reinterpret_cast<HostFn16>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8],
                                          a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
}

// Where guest address va lives in the host (see guest_memory.h).
inline std::uint8_t *host_of(uint32_t va) {
#ifdef BOZ_GUEST_BASE
    return g_guest_base + va;
#else
    return reinterpret_cast<std::uint8_t *>(static_cast<uintptr_t>(va));
#endif
}

uint32_t *g_svc_page;
uint32_t g_svc_base;
// A host function the guest calls through a slot of the code page, with its argument kinds:
// bit n of pointers set = argument n is a guest address to translate; result 'p' = translate
// the result back (guest-base builds; see host_signatures.h).
struct Slot {
    std::atomic<uintptr_t> fn{0};
    uint32_t pointers = 0;
    char result = 'i';
};
Slot g_far[FAR_MAX];
std::atomic<uint32_t> g_far_count;
std::mutex g_far_mutex;

uint32_t g_image_start, g_image_end;
arm_emu_fault_handler g_fault_handler;
bool g_precise_faults = true;
bool g_perf_stats;
long g_trace_limit;
uint32_t g_trace_ignored[8];
unsigned g_trace_ignored_count;
std::atomic<unsigned long> g_host_calls, g_traced, g_hook_calls, g_flushes;
std::atomic<uint64_t> g_hook_us, g_host_us;
std::atomic<uint32_t> g_last_target;
std::atomic<unsigned long> g_slow_accesses; // memory accesses outside the page table fast path

// Pages already confirmed as host code (plat_address_in_module is slow; see arm_emu.c).
constexpr unsigned HOST_PAGE_SLOTS = 1024;
std::atomic<uint32_t> g_host_pages[HOST_PAGE_SLOTS];

bool host_code_address(uint32_t address) {
#ifdef BOZ_GUEST_BASE
    (void)address; // host code never lies in the guest space: every host call uses a slot
    return false;
#endif
    uint32_t page = (address >> 12) + 1;
    auto &slot = g_host_pages[page % HOST_PAGE_SLOTS];
    if (slot.load(std::memory_order_relaxed) == page) return true;
    if (!plat_address_in_module(reinterpret_cast<const void *>(static_cast<uintptr_t>(address)))) {
        return false;
    }
    slot.store(page, std::memory_order_relaxed);
    return true;
}

// ---- memory ----

PageTable *g_pages; // what the JIT reads through; null = not yet mapped
std::mutex g_map_mutex;

// Enters guest pages [lo, hi) into the page table.
void map_range(uint64_t lo, uint64_t hi) {
    lo &= ~uint64_t{PAGE_SIZE_EMU - 1};
    if (hi > (uint64_t{1} << 32)) hi = uint64_t{1} << 32;
    for (uint64_t at = lo; at < hi; at += PAGE_SIZE_EMU) {
        (*g_pages)[at >> 12] = host_of(static_cast<uint32_t>(at));
    }
}

// Maps the host data around va into the page table, as arm_emu.c's on_unmapped does.
bool map_on_demand(uint32_t va) {
    if (va < PAGE_SIZE_EMU) return false;
    std::lock_guard<std::mutex> lock(g_map_mutex);
    if ((*g_pages)[va >> 12]) return true;
    // Module images too (the executable's data): the JIT only fetches code through
    // MemoryReadCode, which never reads a host module's code.
    uint64_t lo, hi;
    uintptr_t host = reinterpret_cast<uintptr_t>(host_of(va));
#ifdef BOZ_GUEST_BASE
    // Only committed pages of the reservation: the rest of it is reserved, not readable.
    if (!plat_region(host, &lo, &hi) || lo > host || hi <= host) return false;
    uint64_t base = reinterpret_cast<uintptr_t>(g_guest_base);
    lo = lo < base + GUEST_LOWEST_ADDRESS ? GUEST_LOWEST_ADDRESS : lo - base;
    hi = hi - base > (uint64_t{1} << 32) ? (uint64_t{1} << 32) : hi - base;
#else
    if (!plat_data_window(host, &lo, &hi) || lo > host || hi <= host) return false;
#endif
    map_range(lo, hi);
    return true;
}

inline std::uint8_t *page_of(uint32_t va) {
    std::uint8_t *page = (*g_pages)[va >> 12];
    if (page || !map_on_demand(va)) return page;
    return (*g_pages)[va >> 12];
}

// ---- hooks ----

struct Cpu;

constexpr int MAX_CODE_HOOKS = 256;
struct CodeHook {
    std::atomic<bool> used{false};
    Cpu *owner = nullptr;
    uint32_t address = 0;
    arm_emu_code_hook fn = nullptr;
    void *user = nullptr;
};
CodeHook g_code_hooks[MAX_CODE_HOOKS];
std::mutex g_hook_mutex;

class Env;
class SystemCoprocessor;
std::unique_ptr<Dynarmic::ExclusiveMonitor> g_monitor;
std::array<std::shared_ptr<SystemCoprocessor>, 16> g_coprocs;
// Processor ids for the exclusive monitor: one per thread running guest code, given back when
// the thread ends (SDL starts a new thread each time a device opens, e.g. the voice-chat
// microphone), so a long session never runs out of the MAX_CPUS slots.
std::mutex g_cpu_ids_mutex;
bool g_cpu_ids_used[MAX_CPUS];
std::thread::id g_main_thread;

unsigned take_cpu_id() {
    std::lock_guard<std::mutex> lock(g_cpu_ids_mutex);
    for (unsigned i = 0; i < MAX_CPUS; ++i) {
        if (!g_cpu_ids_used[i]) {
            g_cpu_ids_used[i] = true;
            return i;
        }
    }
    std::fprintf(stderr, "[arm] more than %u threads run guest code at once\n", MAX_CPUS);
    std::abort();
}

void release_cpu_id(unsigned id) {
    std::lock_guard<std::mutex> lock(g_cpu_ids_mutex);
    if (id < MAX_CPUS) g_cpu_ids_used[id] = false;
}

// Per-thread CPU: one Jit per nesting level, the guest stack, the hook state.
struct Cpu {
    std::unique_ptr<Env> envs[MAX_DEPTH];
    int level = 0; // Jits inside Run(); envs[level - 1] is the one in a callback
    unsigned id = 0;
    std::uint8_t *stack = nullptr;
    uint32_t initial_sp = 0;
    std::atomic<bool> flush_pending{false};
    // Addresses whose hooks changed: only the blocks holding them are retranslated (a full
    // ClearCache retranslates everything, a stall of up to ~200 ms on Kino).
    std::mutex pending_mutex;
    std::vector<uint32_t> pending_addresses;
    bool pending_everything = false;
    bool stepping = false;
    bool in_host = false;
    uint64_t nested_guest_us = 0;
    uint32_t call_scratch = 0;
    struct arm_emu_fault fault {};
    bool faulted = false;
    bool fatal = false;
    int hook_depth = 0;

    Env &env(int at);
    void flush_all();
};

thread_local Cpu *t_cpu;

// A filter in front of the hook table: PreCodeReadHook asks for every instruction the JIT
// translates, and almost none are hooked. A bit per (address / 2) modulo 2^16, set when a hook is
// added and rebuilt when one is removed; a clear bit means no hook at that address.
constexpr unsigned FILTER_BITS = 1u << 16;
std::atomic<uint64_t> g_hook_filter[FILTER_BITS / 64];

unsigned filter_bit(uint32_t address) { return (address >> 1) & (FILTER_BITS - 1); }

void filter_add(uint32_t address) {
    unsigned bit = filter_bit(address);
    g_hook_filter[bit / 64].fetch_or(uint64_t{1} << (bit % 64), std::memory_order_release);
}

// Called with g_hook_mutex held.
void filter_rebuild() {
    for (auto &word : g_hook_filter) word.store(0, std::memory_order_relaxed);
    for (auto &hook : g_code_hooks) {
        if (hook.used.load(std::memory_order_acquire)) filter_add(hook.address);
    }
}

bool hooked(Cpu *cpu, uint32_t address) {
    unsigned bit = filter_bit(address);
    if (!(g_hook_filter[bit / 64].load(std::memory_order_acquire) & (uint64_t{1} << (bit % 64)))) {
        return false;
    }
    for (auto &hook : g_code_hooks) {
        if (hook.used.load(std::memory_order_acquire) && hook.owner == cpu &&
            hook.address == address) {
            return true;
        }
    }
    return false;
}

void report_registers(const char *what, uint32_t address, const uint32_t *r);

// ---- the JIT's callbacks ----

class Env final : public Dynarmic::A32::UserCallbacks {
public:
    Cpu *cpu = nullptr;
    std::unique_ptr<Dynarmic::A32::Jit> jit;

    void fault(const char *what, uint32_t va, bool write) {
        if (cpu->faulted) return;
        cpu->faulted = true;
        cpu->fault.address = va;
        cpu->fault.write = write;
        (void)what;
        jit->HaltExecution(HaltReason::MemoryAbort);
    }

    template <typename T> T read(uint32_t va) {
        g_slow_accesses.fetch_add(1, std::memory_order_relaxed);
        std::uint8_t *page = page_of(va);
        if (page && ((va & 0xfffu) + sizeof(T) <= PAGE_SIZE_EMU || page_of(va + sizeof(T) - 1))) {
            T v;
            std::memcpy(&v, host_of(va), sizeof v);
            return v;
        }
        fault("read", va, false);
        return T{};
    }
    template <typename T> void write(uint32_t va, T v) {
        g_slow_accesses.fetch_add(1, std::memory_order_relaxed);
        std::uint8_t *page = page_of(va);
        if (page && ((va & 0xfffu) + sizeof(T) <= PAGE_SIZE_EMU || page_of(va + sizeof(T) - 1))) {
            std::memcpy(host_of(va), &v, sizeof v);
            return;
        }
        fault("write", va, true);
    }
    template <typename T> bool write_exclusive(uint32_t va, T v, T expected) {
        if (!page_of(va) || !page_of(va + sizeof(T) - 1)) {
            fault("write", va, true);
            return false;
        }
        T *target = reinterpret_cast<T *>(host_of(va));
        return __atomic_compare_exchange_n(target, &expected, v, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }

    std::optional<std::uint32_t> MemoryReadCode(uint32_t va) override {
        if (va - g_svc_base < SVC_PAGE_SIZE) return SVC_ARM_WORD;
        if (host_code_address(va)) return SVC_ARM_WORD;
        if (std::uint8_t *page = page_of(va)) {
            std::uint32_t w;
            std::memcpy(&w, page + (va & 0xfffu), 4);
            return w;
        }
        return std::nullopt; // NoExecuteFault
    }

    bool PreCodeReadHook(bool is_thumb, uint32_t pc, Dynarmic::A32::IREmitter &ir) override {
        (void)is_thumb;
        if (cpu->stepping || !hooked(cpu, pc)) return true;
        ir.BranchWritePC(ir.Imm32(pc));
        ir.CallSupervisor(ir.Imm32(HOOK_SWI));
        ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::ReturnToDispatch{}});
        return false;
    }

    std::uint8_t MemoryRead8(uint32_t va) override { return read<std::uint8_t>(va); }
    std::uint16_t MemoryRead16(uint32_t va) override { return read<std::uint16_t>(va); }
    std::uint32_t MemoryRead32(uint32_t va) override { return read<std::uint32_t>(va); }
    std::uint64_t MemoryRead64(uint32_t va) override { return read<std::uint64_t>(va); }
    void MemoryWrite8(uint32_t va, std::uint8_t v) override { write(va, v); }
    void MemoryWrite16(uint32_t va, std::uint16_t v) override { write(va, v); }
    void MemoryWrite32(uint32_t va, std::uint32_t v) override { write(va, v); }
    void MemoryWrite64(uint32_t va, std::uint64_t v) override { write(va, v); }
    bool MemoryWriteExclusive8(uint32_t va, std::uint8_t v, std::uint8_t e) override {
        return write_exclusive(va, v, e);
    }
    bool MemoryWriteExclusive16(uint32_t va, std::uint16_t v, std::uint16_t e) override {
        return write_exclusive(va, v, e);
    }
    bool MemoryWriteExclusive32(uint32_t va, std::uint32_t v, std::uint32_t e) override {
        return write_exclusive(va, v, e);
    }
    bool MemoryWriteExclusive64(uint32_t va, std::uint64_t v, std::uint64_t e) override {
        return write_exclusive(va, v, e);
    }

    void InterpreterFallback(uint32_t pc, size_t n) override {
        std::fprintf(stderr, "[arm] no interpreter fallback (pc=%08x, %zu instructions)\n", pc, n);
        fatal();
    }

    void fatal() {
        cpu->fatal = true;
        jit->HaltExecution(HALT_FATAL);
    }

    void CallSVC(std::uint32_t swi) override;

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        switch (e) {
        case E::SendEvent: case E::SendEventLocal: case E::WaitForInterrupt:
        case E::WaitForEvent: case E::Yield: case E::PreloadData:
        case E::PreloadDataWithIntentToWrite: case E::PreloadInstruction:
            return; // hints: no-ops, as under Unicorn
        case E::NoExecuteFault:
            std::fprintf(stderr, "[arm] fetch from unmapped at 0x%08x\n", pc);
            break;
        default:
            std::fprintf(stderr, "[arm] guest exception %d at %08x\n", static_cast<int>(e), pc);
            break;
        }
        report_registers("unexpected exception", pc, jit->Regs().data());
        fatal();
    }

    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1u << 30; }

    void host_call(uint32_t target);
    void code_hook(uint32_t address);
};

// CP14/CP15 as plain storage (TLS registers, plausible IDs).
class SystemCoprocessor final : public Dynarmic::A32::Coprocessor {
    using CoprocReg = Dynarmic::A32::CoprocReg;

public:
    explicit SystemCoprocessor(unsigned number) {
        if (number == 15) {
            word(0, 0, 0, 0) = 0x412fc0f1u; // MIDR: Cortex-A15 r2p1 (Unicorn's model)
            word(0, 0, 0, 1) = 0x8444c004u; // CTR
            word(0, 1, 0, 2) = 0x00f00000u; // CPACR: cp10/cp11 full access
        }
    }
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg,
                                                     CoprocReg, unsigned) override {
        return nop();
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, CoprocReg n, CoprocReg m,
                                               unsigned opc2) override {
        return &slot(opc1, n, m, opc2 + (two ? 8u : 0u));
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool two, unsigned opc, CoprocReg m) override {
        return pair(opc + (two ? 8u : 0u), m);
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg n, CoprocReg m,
                                              unsigned opc2) override {
        return &slot(opc1, n, m, opc2 + (two ? 8u : 0u));
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg m) override {
        return pair(opc + (two ? 8u : 0u), m);
    }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg,
                                             std::optional<std::uint8_t>) override {
        return nop();
    }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg,
                                              std::optional<std::uint8_t>) override {
        return nop();
    }

private:
    static constexpr size_t SLOTS = 16 * 16 * 16 * 16;
    std::array<std::uint32_t, SLOTS> m_words{}; // stable: the JIT bakes these addresses in

    static size_t index(unsigned opc1, unsigned n, unsigned m, unsigned opc2) {
        return (((opc1 & 15u) * 16u + (n & 15u)) * 16u + (m & 15u)) * 16u + (opc2 & 15u);
    }
    std::uint32_t &word(unsigned opc1, unsigned n, unsigned m, unsigned opc2) {
        return m_words[index(opc1, n, m, opc2)];
    }
    std::uint32_t &slot(unsigned opc1, CoprocReg n, CoprocReg m, unsigned opc2) {
        return m_words[index(opc1, static_cast<unsigned>(n), static_cast<unsigned>(m), opc2)];
    }
    static std::optional<Callback> nop() {
        return Callback{[](void *, std::uint32_t, std::uint32_t) -> std::uint64_t { return 0; },
                        std::nullopt};
    }
    std::array<std::uint32_t *, 2> pair(unsigned opc, CoprocReg m) {
        return {&slot(opc, CoprocReg::C15, m, 0), &slot(opc, CoprocReg::C15, m, 1)};
    }
};

Env &Cpu::env(int at) {
    if (at >= MAX_DEPTH) {
        std::fprintf(stderr, "[arm] guest calls nested deeper than %d\n", MAX_DEPTH);
        std::abort();
    }
    if (!envs[at]) {
        auto env = std::make_unique<Env>();
        env->cpu = this;
        UserConfig cfg{};
        cfg.callbacks = env.get();
        cfg.arch_version = Dynarmic::A32::ArchVersion::v7; // Cortex-A15 class, as under Unicorn
        cfg.global_monitor = g_monitor.get();
        cfg.processor_id = id;
        cfg.page_table = g_pages;
        cfg.absolute_offset_page_table = false;
        for (unsigned i = 0; i < 16; ++i) {
            if (i == 10 || i == 11) continue; // VFP/NEON: Dynarmic's own
            cfg.coprocessors[i] = g_coprocs[i];
        }
        cfg.check_halt_on_memory_access = g_precise_faults;
        cfg.enable_cycle_counting = false;
#if defined(__aarch64__)
        cfg.code_cache_size = at == 0 ? 128u << 20 : 16u << 20; // arm64 backend's branch reach
#else
        cfg.code_cache_size = at == 0 ? 256u << 20 : 16u << 20;
#endif
        env->jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
        env->jit->SetCpsr(0x10u);
        env->jit->SetFpscr(0);
        envs[at] = std::move(env);
    }
    return *envs[at];
}

void Cpu::flush_all() {
    std::vector<uint32_t> addresses;
    bool everything;
    {
        std::lock_guard<std::mutex> lock(pending_mutex);
        addresses.swap(pending_addresses);
        everything = pending_everything || addresses.empty();
        pending_everything = false;
    }
    for (auto &env : envs) {
        if (!env) continue;
        if (everything) {
            env->jit->ClearCache();
        } else {
            for (uint32_t address : addresses) env->jit->InvalidateCacheRange(address, 4);
        }
    }
    g_flushes++;
}

void invalidate_later(Cpu *cpu, uint32_t address) {
    std::lock_guard<std::mutex> lock(cpu->pending_mutex);
    if (cpu->pending_addresses.size() < 256) {
        cpu->pending_addresses.push_back(address);
    } else {
        cpu->pending_everything = true;
    }
}

void check_flush(Cpu *cpu) {
    if (cpu->hook_depth == 0 && cpu->flush_pending.exchange(false)) cpu->flush_all();
}

Cpu *cpu_create() {
    auto *cpu = new Cpu();
    cpu->id = take_cpu_id();
    cpu->stack = static_cast<std::uint8_t *>(guest_pages_alloc(0, STACK_SIZE));
    uint32_t stack = cpu->stack ? GUEST_ADDRESS(cpu->stack) : 0;
    if (!stack) {
        std::fprintf(stderr, "[arm] unable to allocate a guest stack\n");
        std::abort();
    }
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        map_range(stack, uint64_t{stack} + STACK_SIZE);
    }
    cpu->initial_sp = stack + STACK_SIZE - 64;
    cpu->env(0).jit->Regs()[13] = cpu->initial_sp;
    return cpu;
}

// Releases the thread's CPU (its Jits, guest stack and processor id) when the thread ends. Not on
// the main thread: other threads may still run guest code while the process exits.
struct CpuReaper {
    ~CpuReaper() { arm_emu_thread_exit(); }
};
thread_local CpuReaper t_reaper;

Cpu *this_cpu() {
    if (!t_cpu) {
        t_cpu = cpu_create();
        if (std::this_thread::get_id() != g_main_thread) (void)&t_reaper; // constructs it
    }
    return t_cpu;
}

uint32_t sentinel_address() { return g_svc_base + 0x1000 - 4; }
uint32_t trap_address() { return g_svc_base + 0x1000 - 8; }

void report_registers(const char *what, uint32_t address, const uint32_t *r) {
    std::fprintf(stderr, "[arm] %s at 0x%08x pc=%08x lr=%08x sp=%08x\n", what, address, r[15],
                 r[14], r[13]);
    const char *labels[2] = {"pc", "lr"};
    uint32_t values[2] = {r[15], r[14]};
    for (int i = 0; i < 2; ++i) {
        if (g_image_start && values[i] >= g_image_start && values[i] < g_image_end) {
            std::fprintf(stderr, "[arm]   %s = image+0x%06x\n", labels[i], values[i] - g_image_start);
        }
    }
    for (int i = 0; i < 13; i += 4) {
        std::fprintf(stderr, "[arm]   r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n", i, r[i], i + 1,
                     r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    }
    if (std::uint8_t *page = (*g_pages)[r[13] >> 12]) {
        (void)page;
        const uint32_t *stack = reinterpret_cast<const uint32_t *>(host_of(r[13]));
        if ((r[13] & 0xfffu) <= PAGE_SIZE_EMU - 64) {
            for (int i = 0; i < 16; i += 8) {
                std::fprintf(stderr, "[arm]   sp+%02x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                             i * 4, stack[i], stack[i + 1], stack[i + 2], stack[i + 3], stack[i + 4],
                             stack[i + 5], stack[i + 6], stack[i + 7]);
            }
        }
    }
}

bool trace_call(uint32_t target) {
    ++g_host_calls;
    g_last_target.store(target, std::memory_order_relaxed);
    if (g_trace_limit <= 0) return false;
    for (unsigned i = 0; i < g_trace_ignored_count; ++i) {
        if (g_trace_ignored[i] == target) return false;
    }
    return static_cast<long>(++g_traced) <= g_trace_limit;
}

void Env::host_call(uint32_t target) {
    auto &r = jit->Regs();
    uintptr_t fn = target;
    uint32_t pointers = 0;
    char result_kind = 'i';
    if (target - g_svc_base < SVC_PAGE_SIZE) {
        uint32_t index = (target - g_svc_base) / 4;
        if (target == sentinel_address()) {
            jit->HaltExecution(HALT_RETURNED);
            return;
        }
        if (index < FAR_FIRST || index - FAR_FIRST >= g_far_count.load() ||
            !(fn = g_far[index - FAR_FIRST].fn.load())) {
            report_registers("jump into the emulator's code page", target, r.data());
            fatal();
            return;
        }
        pointers = g_far[index - FAR_FIRST].pointers;
        result_kind = g_far[index - FAR_FIRST].result;
    }
    uint64_t a[HOST_ARG_WORDS];
    for (int i = 0; i < 4; ++i) a[i] = r[i];
    uint32_t sp = r[13];
    for (int i = 4; i < HOST_ARG_WORDS; ++i) {
        // Twelve words whatever the callee takes: past the top of the stack they read as 0.
        uint32_t at = sp + static_cast<uint32_t>(i - 4) * 4u;
        a[i] = page_of(at) ? *reinterpret_cast<const uint32_t *>(host_of(at)) : 0;
    }
    for (uint32_t bits = pointers; bits; bits &= bits - 1) {
        int i = __builtin_ctz(bits);
        a[i] = reinterpret_cast<uintptr_t>(GUEST_POINTER(static_cast<uint32_t>(a[i])));
    }
    cpu->call_scratch = r[12];
    uint32_t lr = r[14];
    bool traced = trace_call(target);

    uint64_t result;
    if (g_perf_stats && !cpu->in_host && cpu->level == 1) {
        uint64_t started = now_us(), nested = cpu->nested_guest_us;
        cpu->in_host = true;
        result = call_host16(fn, a);
        cpu->in_host = false;
        g_host_us += now_us() - started - (cpu->nested_guest_us - nested);
    } else {
        result = call_host16(fn, a);
    }
    if (result_kind == 'p') {
        result = GUEST_ADDRESS(reinterpret_cast<const void *>(static_cast<uintptr_t>(result)));
    } else if (result_kind == 's') {
        result = guest_string_address(reinterpret_cast<const char *>(static_cast<uintptr_t>(result)));
    }
    if (traced) {
        const char *name = plat_symbol_name(reinterpret_cast<void *>(fn));
        std::fprintf(stderr, "[call %lu] %s@%08x=%p(%08x, %08x, %08x, %08x) from %08x -> %08x\n",
                     g_traced.load(), name ? name : "?", target, reinterpret_cast<void *>(fn),
                     static_cast<uint32_t>(a[0]),
                     static_cast<uint32_t>(a[1]), static_cast<uint32_t>(a[2]),
                     static_cast<uint32_t>(a[3]), lr, static_cast<uint32_t>(result));
    }
    check_flush(cpu);
    auto &regs = jit->Regs(); // the host call may have run nested guest code on other Jits
    regs[0] = static_cast<uint32_t>(result);
    regs[1] = static_cast<uint32_t>(result >> 32);
    regs[15] = lr & ~1u;
    uint32_t cpsr = jit->Cpsr();
    jit->SetCpsr((lr & 1u) ? (cpsr | 0x20u) : (cpsr & ~0x20u));
}

void Env::code_hook(uint32_t address) {
    struct arm_emu_regs regs;
    std::memcpy(regs.r, jit->Regs().data(), sizeof regs.r);
    regs.thumb = (jit->Cpsr() & 0x20u) != 0;
    regs.r[15] = address;
    const struct arm_emu_regs before = regs;

    bool timed = g_perf_stats && cpu->hook_depth == 0;
    uint64_t started = timed ? now_us() : 0;
    cpu->hook_depth++;
    for (auto &hook : g_code_hooks) {
        if (!hook.used.load(std::memory_order_acquire) || hook.owner != cpu || hook.address != address) {
            continue;
        }
        hook.fn(&regs, hook.user);
        if (regs.r[15] != before.r[15] || regs.thumb != before.thumb) break;
    }
    cpu->hook_depth--;
    if (g_perf_stats) {
        ++g_hook_calls;
        if (timed) g_hook_us += now_us() - started;
    }
    check_flush(cpu);

    auto &r = jit->Regs();
    for (int i = 0; i < 15; ++i) r[i] = regs.r[i];
    if (regs.r[15] != before.r[15] || regs.thumb != before.thumb) {
        r[15] = regs.r[15] & ~1u;
        uint32_t cpsr = jit->Cpsr();
        jit->SetCpsr(regs.thumb ? (cpsr | 0x20u) : (cpsr & ~0x20u));
        return;
    }
    // Carry on with the hooked instruction itself: one single step, outside this callback.
    r[15] = address;
    jit->HaltExecution(HALT_STEP);
}

void Env::CallSVC(std::uint32_t swi) {
    auto &r = jit->Regs();
    if (swi == HOOK_SWI) {
        code_hook(r[15]);
        return;
    }
    bool thumb = (jit->Cpsr() & 0x20u) != 0;
    uint32_t at = thumb ? r[15] - 2 : r[15] - 4;
    bool shadow = at - g_svc_base < SVC_PAGE_SIZE || host_code_address(at);
    if (!shadow || (thumb ? swi != 0 : swi != (SVC_ARM_WORD & 0xffffffu))) {
        report_registers("unexpected supervisor call", at, r.data());
        fatal();
        return;
    }
    host_call(at);
}

// Runs the CPU's Jit at `level` from pc until it returns to the sentinel. False on a fatal error.
bool run(Cpu *cpu, int level, uint32_t pc) {
    Env &env = cpu->env(level);
    auto &jit = *env.jit;
    jit.Regs()[14] = sentinel_address();
    jit.Regs()[15] = pc & ~1u;
    jit.SetCpsr((jit.Cpsr() & ~0x3fu) | 0x10u | ((pc & 1u) ? 0x20u : 0u));
    cpu->level = level + 1;
    bool ok = true;
    for (;;) {
        cpu->faulted = false;
        HaltReason reason = jit.Run();
        if (cpu->fatal) {
            ok = false;
            break;
        }
        if (has(reason, HaltReason::MemoryAbort) && cpu->faulted) {
            struct arm_emu_fault &f = cpu->fault;
            std::memcpy(f.r, jit.Regs().data(), sizeof f.r);
            f.thumb = (jit.Cpsr() & 0x20u) != 0;
            f.r[15] &= ~1u;
            uint32_t fault_pc = f.r[15];
            if (g_precise_faults && g_fault_handler && g_fault_handler(&f)) {
                auto &r = jit.Regs();
                for (int i = 0; i < 15; ++i) r[i] = f.r[i];
                r[15] = f.r[15] & ~1u;
                uint32_t cpsr = jit.Cpsr();
                jit.SetCpsr(f.thumb ? (cpsr | 0x20u) : (cpsr & ~0x20u));
                static uint32_t reported_pc[16];
                for (auto &seen : reported_pc) {
                    if (seen == fault_pc) break;
                    if (!seen) {
                        seen = fault_pc;
                        std::fprintf(stderr, "[arm] recovered %s fault at 0x%08x (pc=%08x)\n",
                                     f.write ? "write" : "read", f.address, fault_pc);
                        break;
                    }
                }
                continue;
            }
            report_registers(f.write ? "write to unmapped" : "read from unmapped", f.address,
                             jit.Regs().data());
            ok = false;
            break;
        }
        if (has(reason, HALT_RETURNED)) break;
        if (has(reason, HALT_STEP)) {
            cpu->stepping = true;
            jit.Step();
            cpu->stepping = false;
            if (cpu->fatal) {
                ok = false;
                break;
            }
            if (cpu->faulted) {
                // The faulting step's registers are the hooked instruction's: handle it as above.
                // (Rare: a hook on an instruction that faults.)
                report_registers("fault in a hooked instruction", cpu->fault.address,
                                 jit.Regs().data());
                ok = false;
                break;
            }
            if (jit.Regs()[15] == sentinel_address()) break;
        }
        // Anything else (cache invalidation): keep going.
    }
    cpu->level = level;
    return ok;
}

} // namespace

extern "C" {

void arm_emu_set_fault_handler(arm_emu_fault_handler handler) { g_fault_handler = handler; }

void arm_emu_register_code(uint32_t start, uint32_t size) {
    if (!size || !g_pages) return;
    std::lock_guard<std::mutex> lock(g_map_mutex);
    map_range(start, uint64_t{start} + size + PAGE_SIZE_EMU - 1);
}

void arm_emu_set_image(uint32_t start, uint32_t end) {
    g_image_start = start;
    g_image_end = end;
    arm_emu_register_code(start, end - start);
}

void arm_emu_trace_ignore(uint32_t fn) {
    if (g_trace_ignored_count < sizeof(g_trace_ignored) / sizeof(g_trace_ignored[0])) {
        g_trace_ignored[g_trace_ignored_count++] = fn;
    }
}

static void *status_thread(void *) {
    unsigned long previous = 0;
    for (;;) {
        struct timespec ts = {2, 0};
        nanosleep(&ts, nullptr);
        unsigned long now = g_host_calls;
        uint32_t last = g_last_target;
        const char *name = plat_symbol_name(reinterpret_cast<void *>(static_cast<uintptr_t>(last)));
        std::fprintf(stderr,
                     "[status] host calls %lu (+%lu/2s) last=%s@%08x jit flushes %lu slow accesses %lu\n",
                     now, now - previous, name ? name : "?", last, g_flushes.load(),
                     g_slow_accesses.exchange(0));
        previous = now;
    }
    return nullptr;
}

bool arm_emu_init(void) {
    if (g_svc_page) return true;
    if (!guest_space_init()) return false;
    g_pages = new PageTable{};
    void *page = guest_pages_alloc(0, SVC_PAGE_SIZE);
    uint32_t base = page ? GUEST_ADDRESS(page) : 0;
    if (!base) {
        std::fprintf(stderr, "[arm] unable to allocate the emulator's code page\n");
        return false;
    }
    g_svc_page = static_cast<uint32_t *>(page);
    g_svc_base = base;
    for (size_t i = 0; i < SVC_PAGE_SIZE / sizeof(uint32_t); ++i) g_svc_page[i] = SVC_ARM_WORD;
    map_range(g_svc_base, uint64_t{g_svc_base} + SVC_PAGE_SIZE);

    g_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(MAX_CPUS);
    g_main_thread = std::this_thread::get_id();
    for (unsigned i = 0; i < 16; ++i) {
        if (i != 10 && i != 11) g_coprocs[i] = std::make_shared<SystemCoprocessor>(i);
    }
    const char *trace = std::getenv("BOZ_TRACE_CALLS");
    g_trace_limit = trace ? std::strtol(trace, nullptr, 10) : 0;
    g_perf_stats = std::getenv("BOZ_PERF_STATS") != nullptr;
    const char *precise = std::getenv("BOZ_PRECISE_FAULTS");
    g_precise_faults = !(precise && std::strcmp(precise, "0") == 0);
    if (std::getenv("BOZ_TRACE_STATUS")) {
        pthread_t thread;
        pthread_create(&thread, nullptr, status_thread, nullptr);
        pthread_detach(thread);
    }
    std::fprintf(stderr, "[arm] Dynarmic A32 JIT%s\n", g_precise_faults ? "" : ", imprecise faults");
    return true;
}

uint32_t arm_emu_guest_fn_sig(const void *fn, const char *signature) {
    uintptr_t address = reinterpret_cast<uintptr_t>(fn);
    if (!fn) return 0;
#ifndef BOZ_GUEST_BASE
    // Identity map: a function below 4 GiB is called at its own address (arguments as words,
    // which equal the host pointers).
    if (address + 4 <= (uint64_t{1} << 32)) return static_cast<uint32_t>(address);
#endif
    if (!g_svc_page && !arm_emu_init()) return 0;
    std::lock_guard<std::mutex> lock(g_far_mutex);
    uint32_t count = g_far_count.load();
    for (uint32_t i = 0; i < count; ++i) {
        if (g_far[i].fn.load() == address) return g_svc_base + (FAR_FIRST + i) * 4;
    }
    if (count >= FAR_MAX) {
        std::fprintf(stderr, "[arm] no slot for host function %p\n", fn);
        return 0;
    }
    Slot &slot = g_far[count];
    slot.pointers = 0;
    slot.result = 'i';
    if (signature && signature[0] && signature[1] == ':') {
        slot.result = signature[0];
        for (int i = 0; signature[2 + i] && i < HOST_ARG_WORDS; ++i) {
            if (signature[2 + i] == 'p') slot.pointers |= 1u << i;
        }
    }
    slot.fn.store(address);
    g_far_count.store(count + 1);
    return g_svc_base + (FAR_FIRST + count) * 4;
}

uint32_t arm_emu_guest_fn(const void *fn) { return arm_emu_guest_fn_sig(fn, nullptr); }

uint64_t arm_emu_call(uint32_t fn, int argc, const uint32_t *argv) {
    Cpu *cpu = this_cpu();
    check_flush(cpu);
    int level = cpu->level;
    Env &env = cpu->env(level);
    auto &jit = *env.jit;
    const auto saved_regs = jit.Regs();
    const uint32_t saved_cpsr = jit.Cpsr();

    uint32_t sp;
    if (level > 0) {
        Env &parent = cpu->env(level - 1);
        sp = (parent.jit->Regs()[13] - 64u) & ~7u;
        jit.SetFpscr(parent.jit->Fpscr());
    } else {
        sp = jit.Regs()[13];
    }
    int stack_args = argc > 4 ? argc - 4 : 0;
    sp -= static_cast<uint32_t>((stack_args * 4 + 7) & ~7);
    for (int i = 0; i < argc; ++i) {
        if (i < 4) {
            jit.Regs()[i] = argv[i];
        } else {
            std::memcpy(host_of(sp + (i - 4) * 4u), &argv[i], 4);
        }
    }
    jit.Regs()[13] = sp;

    bool nested = cpu->in_host;
    uint64_t started = nested ? now_us() : 0;
    cpu->in_host = false;
    bool ok = run(cpu, level, fn);
    if (nested) {
        cpu->in_host = true;
        cpu->nested_guest_us += now_us() - started;
    }
    if (!ok) {
        std::fprintf(stderr, "[arm] call 0x%08x stopped (pc=%08x)\n", fn, jit.Regs()[15]);
        std::abort();
    }
    uint64_t result = jit.Regs()[0] | (static_cast<uint64_t>(jit.Regs()[1]) << 32);
    if (level > 0) {
        jit.Regs() = saved_regs;
        jit.SetCpsr(saved_cpsr);
    } else {
        jit.Regs()[13] = sp + static_cast<uint32_t>((stack_args * 4 + 7) & ~7);
    }
    return result;
}

int arm_emu_hook_add(uint32_t address, arm_emu_code_hook fn, void *user) {
    if (!g_pages) return -1;
    Cpu *cpu = this_cpu();
    address &= ~1u;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    for (int id = 0; id < MAX_CODE_HOOKS; ++id) {
        CodeHook &hook = g_code_hooks[id];
        if (hook.used.load()) continue;
        hook.owner = cpu;
        hook.address = address;
        hook.fn = fn;
        hook.user = user;
        hook.used.store(true, std::memory_order_release);
        filter_add(address);
        invalidate_later(cpu, address);
        cpu->flush_pending = true;
        if (cpu->level == 0) check_flush(cpu);
        return id;
    }
    return -1;
}

void arm_emu_hook_remove(int id) {
    if (id < 0 || id >= MAX_CODE_HOOKS) return;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    CodeHook &hook = g_code_hooks[id];
    if (!hook.used.load()) return;
    hook.used.store(false, std::memory_order_release);
    filter_rebuild();
    invalidate_later(hook.owner, hook.address);
    hook.owner->flush_pending = true;
    if (hook.owner == t_cpu && t_cpu->level == 0) check_flush(t_cpu);
}

void arm_emu_perf_read(struct arm_emu_perf *out) {
    out->host_calls = g_host_calls;
    out->host_us = g_host_us;
    out->hook_calls = g_hook_calls;
    out->hook_us = g_hook_us;
    out->jit_flushes = g_flushes;
}

uint32_t arm_emu_trap_address(void) { return g_svc_page ? trap_address() : 0; }

uint32_t arm_emu_call_scratch(void) { return t_cpu ? t_cpu->call_scratch : 0; }

uint64_t arm_emu_call_host(uint32_t fn, const uint32_t *args) {
    uintptr_t target = fn;
    uint32_t pointers = 0;
    char result_kind = 'i';
    if (g_svc_page && fn - g_svc_base >= FAR_FIRST * 4 && fn - g_svc_base < SVC_PAGE_SIZE) {
        uint32_t index = (fn - g_svc_base) / 4 - FAR_FIRST;
        if (index < g_far_count.load()) {
            target = g_far[index].fn.load();
            pointers = g_far[index].pointers;
            result_kind = g_far[index].result;
        }
    }
    uint64_t a[HOST_ARG_WORDS];
    for (int i = 0; i < HOST_ARG_WORDS; ++i) {
        a[i] = (pointers >> i) & 1u ? reinterpret_cast<uintptr_t>(GUEST_POINTER(args[i])) : args[i];
    }
    uint64_t result = call_host16(target, a);
    if (result_kind == 'p') {
        result = GUEST_ADDRESS(reinterpret_cast<const void *>(static_cast<uintptr_t>(result)));
    } else if (result_kind == 's') {
        result = guest_string_address(reinterpret_cast<const char *>(static_cast<uintptr_t>(result)));
    }
    return result;
}

void arm_emu_thread_exit(void) {
    Cpu *cpu = t_cpu;
    if (!cpu) return;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        for (auto &hook : g_code_hooks) {
            if (hook.used.load() && hook.owner == cpu) hook.used.store(false);
        }
    }
    for (auto &env : cpu->envs) env.reset();
    {
        std::lock_guard<std::mutex> lock(g_map_mutex);
        uint32_t stack = GUEST_ADDRESS(cpu->stack);
        for (uint64_t at = stack; at < uint64_t{stack} + STACK_SIZE; at += PAGE_SIZE_EMU) {
            (*g_pages)[at >> 12] = nullptr; // the stack's pages may be reused for host data
        }
    }
    guest_pages_free(cpu->stack, STACK_SIZE);
    if (g_monitor) g_monitor->ClearProcessor(cpu->id);
    release_cpu_id(cpu->id);
    delete cpu;
    t_cpu = nullptr;
}

} // extern "C"
