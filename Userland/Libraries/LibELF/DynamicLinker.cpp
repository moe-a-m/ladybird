/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2022, Jesse Buhagiar <jooster669@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <Kernel/API/VirtualMemoryAnnotations.h>
#include <Kernel/API/prctl_numbers.h>
#include <LibELF/Arch/tls.h>
#include <LibELF/AuxiliaryVector.h>
#include <LibELF/DynamicLinker.h>
#include <LibELF/DynamicLoader.h>
#include <LibELF/DynamicObject.h>
#include <LibELF/Hashes.h>
#include <bits/dlfcn_integration.h>
#include <bits/pthread_integration.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <string.h>
#include <sys/internals.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <syscall.h>
#include <unistd.h>

namespace ELF {

static HashMap<ByteString, NonnullRefPtr<ELF::DynamicLoader>> s_loaders;
static ByteString s_main_program_path;

// Dependencies have to always be added after the object that depends on them in `s_global_objects`.
// This is needed for calling the destructors in the correct order.
static OrderedHashMap<ByteString, NonnullRefPtr<ELF::DynamicObject>> s_global_objects;

using EntryPointFunction = int (*)(int, char**, char**);
using LibCExitFunction = void (*)(int);
using DlIteratePhdrCallbackFunction = int (*)(struct dl_phdr_info*, size_t, void*);
using DlIteratePhdrFunction = int (*)(DlIteratePhdrCallbackFunction, void*);
using CallFiniFunctionsFunction = void (*)();

extern "C" [[noreturn]] void _invoke_entry(int argc, char** argv, char** envp, EntryPointFunction entry);

struct TLSData {
    size_t total_tls_size { 0 };
    void* tls_template { nullptr };
    size_t tls_template_size { 0 };
    size_t alignment { 0 };
    size_t static_tls_region_size { 0 };
    size_t static_tls_region_alignment { 0 };
};
static TLSData s_tls_data;

static char** s_envp = nullptr;
static __pthread_mutex_t s_loader_lock = __PTHREAD_MUTEX_INITIALIZER;
static ByteString s_cwd;

static bool s_allowed_to_check_environment_variables { false };
static bool s_do_breakpoint_trap_before_entry { false };
static StringView s_ld_library_path;
static StringView s_main_program_pledge_promises;
static ByteString s_loader_pledge_promises;

static HashMap<StringView, DynamicObject::SymbolLookupResult> s_magic_functions;

Optional<DynamicObject::SymbolLookupResult> DynamicLinker::lookup_global_symbol(StringView name)
{
    auto symbol = DynamicObject::HashSymbol { name };

    for (auto& lib : s_global_objects) {
        auto res = lib.value->lookup_symbol(symbol);
        if (!res.has_value())
            continue;
        if (res.value().bind == STB_GLOBAL || res.value().bind == STB_WEAK)
            return res;
        // We don't want to allow local symbols to be pulled in to other modules
    }

    if (auto magic_lookup = s_magic_functions.get(name); magic_lookup.has_value())
        return *magic_lookup;
    return {};
}

static Result<NonnullRefPtr<DynamicLoader>, DlErrorMessage> map_library(ByteString const& filepath, int fd)
{
    VERIFY(filepath.starts_with('/'));

    auto loader = TRY(ELF::DynamicLoader::try_create(fd, filepath));

    s_loaders.set(filepath, *loader);

    static size_t s_current_tls_offset = 0;

    if constexpr (TLS_VARIANT == 1) {
        if (loader->tls_alignment_of_current_object() != 0)
            s_current_tls_offset = align_up_to(s_current_tls_offset, loader->tls_alignment_of_current_object());
        loader->set_tls_offset(s_current_tls_offset);

        s_current_tls_offset += loader->tls_size_of_current_object();
    } else if constexpr (TLS_VARIANT == 2) {
        s_current_tls_offset -= loader->tls_size_of_current_object();
        if (loader->tls_alignment_of_current_object() != 0)
            s_current_tls_offset = align_down_to(s_current_tls_offset, loader->tls_alignment_of_current_object());
        loader->set_tls_offset(s_current_tls_offset);
    }

    // This actually maps the library at the intended and final place.
    auto main_library_object = loader->map();
    s_global_objects.set(filepath, *main_library_object);

    return loader;
}

Optional<ByteString> DynamicLinker::resolve_library(ByteString const& name, DynamicObject const& parent_object)
{
    // Absolute and relative (to the current working directory) paths are already considered resolved.
    // However, ensure that the returned path is absolute and canonical, so pass it through LexicalPath.
    if (name.contains('/'))
        return LexicalPath::absolute_path(s_cwd, name);

    Vector<StringView> search_paths;

    // Search RPATH values indicated by the ELF (only if RUNPATH is not present).
    if (parent_object.runpath().is_empty())
        search_paths.extend(parent_object.rpath().split_view(':'));

    // Scan the LD_LIBRARY_PATH environment variable if applicable.
    search_paths.extend(s_ld_library_path.split_view(':'));

    // Search RUNPATH values indicated by the ELF.
    search_paths.extend(parent_object.runpath().split_view(':'));

    // Last are the default search paths.
    search_paths.append("/usr/lib"sv);
    search_paths.append("/usr/local/lib"sv);

    for (auto const& search_path : search_paths) {
        LexicalPath library_path(search_path.replace("$ORIGIN"sv, LexicalPath::dirname(parent_object.filepath()), ReplaceMode::FirstOnly));
        ByteString library_name = library_path.append(name).string();

        if (access(library_name.characters(), F_OK) == 0) {
            if (!library_name.starts_with('/')) {
                // FIXME: Non-absolute paths should resolve from the current working directory. However,
                //        since that's almost never the effect that is actually desired, let's print
                //        a warning and only implement it once something actually needs that behavior.
                dbgln("\033[33mWarning:\033[0m Resolving library '{}' resulted in non-absolute path '{}'. Check your binary for relative RPATHs and RUNPATHs.", name, library_name);
            }

            return library_name;
        }
    }

    return {};
}

static Result<NonnullRefPtr<DynamicLoader>, DlErrorMessage> map_library(ByteString const& path)
{
    VERIFY(path.starts_with('/'));

    int fd = open(path.characters(), O_RDONLY);
    if (fd < 0)
        return DlErrorMessage { ByteString::formatted("Could not open shared library '{}': {}", path, strerror(errno)) };

    return map_library(path, fd);
}

static Vector<ByteString> get_dependencies(ByteString const& path)
{
    VERIFY(path.starts_with('/'));

    auto name = LexicalPath::basename(path);
    auto lib = s_loaders.get(path).value();
    Vector<ByteString> dependencies;

    lib->for_each_needed_library([&dependencies, &name](auto needed_name) {
        if (name == needed_name)
            return;
        dependencies.append(needed_name);
    });
    return dependencies;
}

static Result<void, DlErrorMessage> map_dependencies(ByteString const& path)
{
    VERIFY(path.starts_with('/'));

    dbgln_if(DYNAMIC_LOAD_DEBUG, "mapping dependencies for: {}", path);

    auto const& parent_object = (*s_loaders.get(path))->dynamic_object();

    for (auto const& needed_name : get_dependencies(path)) {
        dbgln_if(DYNAMIC_LOAD_DEBUG, "needed library: {}", needed_name.characters());

        auto dependency_path = DynamicLinker::resolve_library(needed_name, parent_object);

        if (!dependency_path.has_value())
            return DlErrorMessage { ByteString::formatted("Could not find required shared library: {}", needed_name) };

        if (!s_loaders.contains(dependency_path.value()) && !s_global_objects.contains(dependency_path.value())) {
            auto loader = TRY(map_library(dependency_path.value()));
            TRY(map_dependencies(loader->filepath()));
        }
    }
    dbgln_if(DYNAMIC_LOAD_DEBUG, "mapped dependencies for {}", path);
    return {};
}

static ErrorOr<FlatPtr> __create_new_tls_region()
{
    void* static_tls_region = serenity_mmap(nullptr, s_tls_data.static_tls_region_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0, s_tls_data.static_tls_region_alignment, "Static TLS Data");
    if (static_tls_region == MAP_FAILED)
        return Error::from_syscall("mmap"sv, -errno);

    auto thread_pointer = calculate_tp_value_from_static_tls_region_address(bit_cast<FlatPtr>(static_tls_region), s_tls_data.tls_template_size, s_tls_data.static_tls_region_alignment);
    VERIFY(thread_pointer % s_tls_data.static_tls_region_alignment == 0);

    auto* tcb = get_tcb_pointer_from_thread_pointer(thread_pointer);

    // FIXME: Add support for dynamically-allocated TLS blocks.
    tcb->dynamic_thread_vector = nullptr;

#if ARCH(X86_64)
    tcb->thread_pointer = bit_cast<void*>(thread_pointer);
#endif

    auto* static_tls_blocks = get_pointer_to_first_static_tls_block_from_thread_pointer(thread_pointer, s_tls_data.tls_template_size, s_tls_data.static_tls_region_alignment);

    if (s_tls_data.tls_template_size != 0)
        memcpy(static_tls_blocks, s_tls_data.tls_template, s_tls_data.tls_template_size);

    return thread_pointer;
}

static ErrorOr<void> __free_tls_region(FlatPtr thread_pointer)
{
    auto* static_tls_region = get_pointer_to_static_tls_region_from_thread_pointer(thread_pointer, s_tls_data.tls_template_size, s_tls_data.static_tls_region_alignment);

    if (munmap(static_tls_region, s_tls_data.static_tls_region_size) != 0)
        return Error::from_syscall("mmap"sv, -errno);

    return {};
}

static void allocate_tls()
{
    // FIXME: Use the max p_align of all TLS segments.
    //        We currently pass s_tls_data.static_tls_region_alignment as the alignment to mmap,
    //        so we would have to manually insert padding, as mmap only accepts alignments that
    //        are multiples of PAGE_SIZE. Or instead use aligned_alloc/posix_memalign?
    s_tls_data.alignment = PAGE_SIZE;

    for (auto const& data : s_loaders) {
        dbgln_if(DYNAMIC_LOAD_DEBUG, "{}: TLS Size: {}, TLS Alignment: {}", data.key, data.value->tls_size_of_current_object(), data.value->tls_alignment_of_current_object());
        s_tls_data.total_tls_size += data.value->tls_size_of_current_object() + data.value->tls_alignment_of_current_object();
    }

    if (s_tls_data.total_tls_size == 0)
        return;

    s_tls_data.tls_template_size = align_up_to(s_tls_data.total_tls_size, PAGE_SIZE);
    s_tls_data.tls_template = mmap_with_name(nullptr, s_tls_data.tls_template_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0, "TLS Template");

    if (s_tls_data.tls_template == MAP_FAILED) {
        dbgln("Failed to allocate memory for the TLS template");
        VERIFY_NOT_REACHED();
    }

    s_tls_data.static_tls_region_alignment = max(s_tls_data.alignment, sizeof(ThreadControlBlock));
    s_tls_data.static_tls_region_size = calculate_static_tls_region_size(s_tls_data.tls_template_size, s_tls_data.static_tls_region_alignment);

    auto tls_template = Bytes(s_tls_data.tls_template, s_tls_data.tls_template_size);

    // Initialize TLS data
    for (auto const& entry : s_loaders) {
        entry.value->copy_initial_tls_data_into(tls_template);
    }

    set_thread_pointer_register(MUST(__create_new_tls_region()));
}

static int __dl_iterate_phdr(DlIteratePhdrCallbackFunction callback, void* data)
{
    pthread_mutex_lock(&s_loader_lock);
    ScopeGuard unlock_guard = [] { pthread_mutex_unlock(&s_loader_lock); };

    for (auto& it : s_global_objects) {
        auto& object = it.value;
        auto info = dl_phdr_info {
            .dlpi_addr = (Elf_Addr)object->base_address().as_ptr(),
            .dlpi_name = object->filepath().characters(),
            .dlpi_phdr = object->program_headers(),
            .dlpi_phnum = object->program_header_count()
        };

        auto res = callback(&info, sizeof(info), data);
        if (res != 0)
            return res;
    }

    return 0;
}

static void initialize_libc(DynamicObject& libc)
{
    uintptr_t stack_guard = get_random<uintptr_t>();
    // We include an additional hardening: zero the first byte of the stack guard to avoid leaking
    // or overwriting the stack guard with C-style string functions.
    stack_guard &= ~0xffULL;

    auto res = libc.lookup_symbol("__libc_init"sv);
    VERIFY(res.has_value());
    using libc_init_func = decltype(__libc_init);
    ((libc_init_func*)res.value().address.as_ptr())(stack_guard);
}

template<typename Callback>
static void for_each_unfinished_dependency_of(ByteString const& path, HashTable<ByteString>& seen_names, Callback callback)
{
    VERIFY(path.starts_with('/'));

    auto loader = s_loaders.get(path);

    if (!loader.has_value()) {
        // Not having a loader here means that the library has already been loaded in at an earlier point,
        // and the loader itself was cleared during the end of `linker_main`.
        return;
    }

    if (loader.value()->is_fully_relocated()) {
        if (!loader.value()->is_fully_initialized()) {
            // If we are ending up here, that possibly means that this library either dlopens itself or a library that depends
            // on it while running its initializers. Assuming that this is the only funny thing that the library does, there is
            // a reasonable chance that nothing breaks, so just warn and continue.
            dbgln("\033[33mWarning:\033[0m Querying for dependencies of '{}' while running its initializers", path);
        }

        return;
    }

    if (seen_names.contains(path))
        return;
    seen_names.set(path);

    for (auto const& needed_name : get_dependencies(path)) {
        auto dependency_path = *DynamicLinker::resolve_library(needed_name, loader.value()->dynamic_object());
        for_each_unfinished_dependency_of(dependency_path, seen_names, callback);
    }

    callback(*s_loaders.get(path).value());
}

static Vector<NonnullRefPtr<DynamicLoader>> collect_loaders_for_library(ByteString const& path)
{
    VERIFY(path.starts_with('/'));

    HashTable<ByteString> seen_names;
    Vector<NonnullRefPtr<DynamicLoader>> loaders;
    for_each_unfinished_dependency_of(path, seen_names, [&](auto& loader) {
        loaders.append(loader);
    });
    return loaders;
}

static void drop_loader_promise(StringView promise_to_drop)
{
    if (s_main_program_pledge_promises.is_empty() || s_loader_pledge_promises.is_empty())
        return;

    s_loader_pledge_promises = s_loader_pledge_promises.replace(promise_to_drop, ""sv, ReplaceMode::All);

    auto extended_promises = ByteString::formatted("{} {}", s_main_program_pledge_promises, s_loader_pledge_promises);
    Syscall::SC_pledge_params params {
        { extended_promises.characters(), extended_promises.length() },
        { nullptr, 0 },
    };
    int rc = syscall(SC_pledge, &params);
    if (rc < 0 && rc > -EMAXERRNO) {
        warnln("Failed to drop loader pledge promise: {}. errno={}", promise_to_drop, errno);
        _exit(1);
    }
}

static Result<void, DlErrorMessage> link_main_library(ByteString const& path, int flags)
{
    VERIFY(path.starts_with('/'));

    auto loaders = collect_loaders_for_library(path);

    // Verify that all objects are already mapped
    for (auto& loader : loaders)
        VERIFY(!loader->map());

    for (auto& loader : loaders) {
        bool success = loader->link(flags);
        if (!success) {
            return DlErrorMessage { ByteString::formatted("Failed to link library {}", loader->filepath()) };
        }
    }

    for (auto& loader : loaders) {
        auto result = loader->load_stage_3(flags);
        VERIFY(!result.is_error());
        auto& object = result.value();

        if (loader->filepath().ends_with("/libc.so"sv)) {
            initialize_libc(*object);
        }

        if (loader->filepath().ends_with("/libsystem.so"sv)) {
            VERIFY(!loader->text_segments().is_empty());
            for (auto const& segment : loader->text_segments()) {
                auto flags = static_cast<int>(VirtualMemoryRangeFlags::SyscallCode) | static_cast<int>(VirtualMemoryRangeFlags::Immutable);
                if (syscall(SC_annotate_mapping, segment.address().get(), flags)) {
                    VERIFY_NOT_REACHED();
                }
            }
        } else {
            for (auto const& segment : loader->text_segments()) {
                auto flags = static_cast<int>(VirtualMemoryRangeFlags::Immutable);
                if (syscall(SC_annotate_mapping, segment.address().get(), flags)) {
                    VERIFY_NOT_REACHED();
                }
            }
        }
    }

    drop_loader_promise("prot_exec"sv);

    for (auto& loader : loaders) {
        loader->load_stage_4();
    }

    return {};
}

static Result<void, DlErrorMessage> __dlclose(void* handle)
{
    dbgln_if(DYNAMIC_LOAD_DEBUG, "__dlclose: {}", handle);

    pthread_mutex_lock(&s_loader_lock);
    ScopeGuard unlock_guard = [] { pthread_mutex_unlock(&s_loader_lock); };

    // FIXME: this will not currently destroy the dynamic object
    // because we're intentionally holding a strong reference to it
    // via s_global_objects until there's proper unload support.
    auto object = static_cast<ELF::DynamicObject*>(handle);
    object->unref();
    return {};
}

static Optional<DlErrorMessage> verify_tls_for_dlopen(DynamicLoader const& loader)
{
    if (loader.tls_size_of_current_object() == 0)
        return {};

    if (s_tls_data.total_tls_size + loader.tls_size_of_current_object() + loader.tls_alignment_of_current_object() > s_tls_data.tls_template_size)
        return DlErrorMessage("TLS size too large");

    bool tls_data_is_all_zero = true;
    loader.image().for_each_program_header([&loader, &tls_data_is_all_zero](ELF::Image::ProgramHeader program_header) {
        if (program_header.type() != PT_TLS)
            return IterationDecision::Continue;

        auto* tls_data = (u8 const*)loader.image().base_address() + program_header.offset();
        for (size_t i = 0; i < program_header.size_in_image(); ++i) {
            if (tls_data[i] != 0) {
                tls_data_is_all_zero = false;
                break;
            }
        }
        return IterationDecision::Break;
    });

    if (tls_data_is_all_zero)
        return {};

    return DlErrorMessage("Using dlopen() with libraries that have non-zeroed TLS is currently not supported");
}

static Result<void*, DlErrorMessage> __dlopen(char const* filename, int flags)
{
    // FIXME: RTLD_NOW and RTLD_LOCAL are not supported
    flags &= ~RTLD_NOW;
    flags |= RTLD_LAZY;
    flags &= ~RTLD_LOCAL;
    flags |= RTLD_GLOBAL;

    dbgln_if(DYNAMIC_LOAD_DEBUG, "__dlopen invoked, filename={}, flags={}", filename, flags);

    if (pthread_mutex_trylock(&s_loader_lock) != 0)
        return DlErrorMessage { "Nested calls to dlopen() are not permitted." };
    ScopeGuard unlock_guard = [] { pthread_mutex_unlock(&s_loader_lock); };

    auto const& parent_object = **s_global_objects.get(s_main_program_path);

    auto library_path = (filename ? DynamicLinker::resolve_library(filename, parent_object) : s_main_program_path);

    if (!library_path.has_value())
        return DlErrorMessage { ByteString::formatted("Could not find required shared library: {}", filename) };

    auto existing_elf_object = s_global_objects.get(library_path.value());
    if (existing_elf_object.has_value()) {
        // It's up to the caller to release the ref with dlclose().
        existing_elf_object.value()->ref();
        return *existing_elf_object;
    }

    auto loader = TRY(map_library(library_path.value()));

    if (auto error = verify_tls_for_dlopen(loader); error.has_value())
        return error.value();

    TRY(map_dependencies(loader->filepath()));

    TRY(link_main_library(loader->filepath(), flags));

    s_tls_data.total_tls_size += loader->tls_size_of_current_object() + loader->tls_alignment_of_current_object();

    auto object = s_global_objects.get(library_path.value());
    if (!object.has_value())
        return DlErrorMessage { "Could not load ELF object." };

    // It's up to the caller to release the ref with dlclose().
    object.value()->ref();
    return *object;
}

static Result<void*, DlErrorMessage> __dlsym(void* handle, char const* symbol_name)
{
    dbgln_if(DYNAMIC_LOAD_DEBUG, "__dlsym: {}, {}", handle, symbol_name);

    pthread_mutex_lock(&s_loader_lock);
    ScopeGuard unlock_guard = [] { pthread_mutex_unlock(&s_loader_lock); };

    StringView symbol_name_view { symbol_name, strlen(symbol_name) };
    Optional<DynamicObject::SymbolLookupResult> symbol;

    if (handle) {
        auto object = static_cast<DynamicObject*>(handle);
        symbol = object->lookup_symbol(symbol_name_view);
    } else {
        // When handle is 0 (RTLD_DEFAULT) we should look up the symbol in all global modules
        // https://pubs.opengroup.org/onlinepubs/009604499/functions/dlsym.html
        symbol = DynamicLinker::lookup_global_symbol(symbol_name_view);
    }

    if (!symbol.has_value())
        return DlErrorMessage { ByteString::formatted("Symbol {} not found", symbol_name_view) };

    if (symbol.value().type == STT_GNU_IFUNC)
        return (void*)reinterpret_cast<DynamicObject::IfuncResolver>(symbol.value().address.as_ptr())();
    return symbol.value().address.as_ptr();
}

static Result<void, DlErrorMessage> __dladdr(void const* addr, Dl_info* info)
{
    VirtualAddress user_addr { addr };
    pthread_mutex_lock(&s_loader_lock);
    ScopeGuard unlock_guard = [] { pthread_mutex_unlock(&s_loader_lock); };

    RefPtr<DynamicObject> best_matching_library;
    VirtualAddress best_library_offset;
    for (auto& lib : s_global_objects) {
        if (user_addr < lib.value->base_address())
            continue;
        auto offset = user_addr - lib.value->base_address();
        if (!best_matching_library || offset < best_library_offset) {
            best_matching_library = lib.value;
            best_library_offset = offset;
        }
    }

    if (!best_matching_library) {
        return DlErrorMessage { "No library found which contains the specified address" };
    }

    Optional<DynamicObject::Symbol> best_matching_symbol;
    best_matching_library->for_each_symbol([&](auto const& symbol) {
        if (user_addr < symbol.address() || user_addr > symbol.address().offset(symbol.size()))
            return;
        best_matching_symbol = symbol;
    });

    info->dli_fbase = best_matching_library->base_address().as_ptr();
    // This works because we don't support unloading objects.
    info->dli_fname = best_matching_library->filepath().characters();
    if (best_matching_symbol.has_value()) {
        info->dli_saddr = best_matching_symbol.value().address().as_ptr();
        info->dli_sname = best_matching_symbol.value().raw_name();
    } else {
        info->dli_saddr = nullptr;
        info->dli_sname = nullptr;
    }
    return {};
}

static void __call_fini_functions()
{
    typedef void (*FiniFunc)();

    for (auto& it : s_global_objects) {
        auto object = it.value;

        if (object->has_fini_array_section()) {
            auto fini_array_section = object->fini_array_section();

            FiniFunc* fini_begin = (FiniFunc*)(fini_array_section.address().as_ptr());
            FiniFunc* fini_end = fini_begin + fini_array_section.entry_count();
            while (fini_begin != fini_end) {
                --fini_end;

                // Android sources claim that these can be -1, to be ignored.
                // 0 deffiniely shows up. Apparently 0/-1 are valid? Confusing.
                if (!*fini_end || ((FlatPtr)*fini_end == (FlatPtr)-1))
                    continue;
                (*fini_end)();
            }
        }

        if (object->has_fini_section()) {
            auto fini_function = object->fini_section_function();
            (fini_function)();
        }
    }
}

static char** __environ_value()
{
    return s_envp;
}

static void read_environment_variables()
{
    for (char** env = s_envp; *env; ++env) {
        StringView env_string { *env, strlen(*env) };
        if (env_string == "_LOADER_BREAKPOINT=1"sv) {
            s_do_breakpoint_trap_before_entry = true;
        }

        constexpr auto library_path_string = "LD_LIBRARY_PATH="sv;
        if (env_string.starts_with(library_path_string)) {
            s_ld_library_path = env_string.substring_view(library_path_string.length());
        }

        constexpr auto main_pledge_promises_key = "_LOADER_MAIN_PROGRAM_PLEDGE_PROMISES="sv;
        if (env_string.starts_with(main_pledge_promises_key)) {
            s_main_program_pledge_promises = env_string.substring_view(main_pledge_promises_key.length());
        }

        constexpr auto loader_pledge_promises_key = "_LOADER_PLEDGE_PROMISES="sv;
        if (env_string.starts_with(loader_pledge_promises_key)) {
            s_loader_pledge_promises = env_string.substring_view(loader_pledge_promises_key.length());
        }
    }
}

void ELF::DynamicLinker::linker_main(ByteString&& main_program_path, int main_program_fd, bool is_secure, int argc, char** argv, char** envp)
{
    VERIFY(main_program_path.starts_with('/'));

    s_envp = envp;

    auto define_magic_function = [&](StringView name, auto function) {
        s_magic_functions.set(name,
            DynamicObject::SymbolLookupResult {
                .size = 8,
                .address = VirtualAddress { reinterpret_cast<void*>(function) },
                .bind = STB_GLOBAL,
                .type = STT_FUNC,
            });
    };
    define_magic_function("__call_fini_functions"sv, __call_fini_functions);
    define_magic_function("__create_new_tls_region"sv, __create_new_tls_region);
    define_magic_function("__dl_iterate_phdr"sv, __dl_iterate_phdr);
    define_magic_function("__dladdr"sv, __dladdr);
    define_magic_function("__dlclose"sv, __dlclose);
    define_magic_function("__dlopen"sv, __dlopen);
    define_magic_function("__dlsym"sv, __dlsym);
    define_magic_function("__environ_value"sv, __environ_value);
    define_magic_function("__free_tls_region"sv, __free_tls_region);

    char* raw_current_directory = getcwd(nullptr, 0);
    s_cwd = raw_current_directory;
    free(raw_current_directory);

    s_allowed_to_check_environment_variables = !is_secure;
    if (s_allowed_to_check_environment_variables)
        read_environment_variables();

    s_main_program_path = main_program_path;

    // NOTE: We always map the main library first, since it may require
    //       placement at a specific address.
    auto result1 = map_library(main_program_path, main_program_fd);
    if (result1.is_error()) {
        warnln("{}", result1.error().text);
        fflush(stderr);
        _exit(1);
    }

    auto loader = result1.release_value();
    size_t needed_dependencies = 0;
    loader->for_each_needed_library([&needed_dependencies](auto) {
        needed_dependencies++;
    });
    bool has_interpreter = false;
    loader->image().for_each_program_header([&has_interpreter](const ELF::Image::ProgramHeader& program_header) {
        if (program_header.type() == PT_INTERP)
            has_interpreter = true;
    });
    // NOTE: Refuse to run a program if it has a dynamic section,
    // it is pie, and does not have an interpreter or needed libraries
    // which is also called "static-pie". These binaries are probably
    // some sort of ELF packers or dynamic loaders, and there's no added
    // value in trying to run them, as they will probably crash due to trying
    // to invoke syscalls from a non-syscall memory executable (code) region.
    if (loader->is_dynamic() && (!has_interpreter || needed_dependencies == 0) && loader->dynamic_object().is_pie()) {
        char const message[] = R"(error: the dynamic loader can't reasonably run static-pie ELF. static-pie ELFs might run executable code that invokes syscalls
outside of the defined syscall memory executable (code) region security measure we implement.
Examples of static-pie ELF objects are ELF packers, and the system dynamic loader itself.)";
        fprintf(stderr, "%s", message);
        fflush(stderr);
        _exit(1);
    }

    auto result2 = map_dependencies(main_program_path);
    if (result2.is_error()) {
        warnln("{}", result2.error().text);
        fflush(stderr);
        _exit(1);
    }

    dbgln_if(DYNAMIC_LOAD_DEBUG, "loaded all dependencies");
    for ([[maybe_unused]] auto& lib : s_loaders) {
        dbgln_if(DYNAMIC_LOAD_DEBUG, "{} - tls size: {}, tls alignment: {}, tls offset: {}", lib.key, lib.value->tls_size_of_current_object(), lib.value->tls_alignment_of_current_object(), lib.value->tls_offset());
    }

    allocate_tls();

    auto entry_point_function = [&main_program_path] {
        auto result = link_main_library(main_program_path, RTLD_GLOBAL | RTLD_LAZY);
        if (result.is_error()) {
            warnln("{}", result.error().text);
            _exit(1);
        }

        drop_loader_promise("rpath"sv);

        auto& main_executable_loader = *s_loaders.get(main_program_path);
        auto entry_point = main_executable_loader->image().entry();
        if (main_executable_loader->is_dynamic())
            entry_point = entry_point.offset(main_executable_loader->base_address().get());
        return (EntryPointFunction)(entry_point.as_ptr());
    }();

    s_loaders.clear();

    int rc = syscall(SC_prctl, PR_SET_NO_NEW_SYSCALL_REGION_ANNOTATIONS, 1, 0, nullptr);
    if (rc < 0) {
        VERIFY_NOT_REACHED();
    }

    dbgln_if(DYNAMIC_LOAD_DEBUG, "Jumping to entry point: {:p}", entry_point_function);
    if (s_do_breakpoint_trap_before_entry) {
#if ARCH(AARCH64)
        asm("brk #0");
#elif ARCH(RISCV64)
        asm("ebreak");
#elif ARCH(X86_64)
        asm("int3");
#else
#    error "Unknown architecture"
#endif
    }

    _invoke_entry(argc, argv, envp, entry_point_function);
    VERIFY_NOT_REACHED();
}

}
