#include <slick-trader/core/strategy_loader.hpp>
#include <slick/logger.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace slick::trader {

StrategyLoader::~StrategyLoader() {
    unload();
}

std::shared_ptr<IStrategy> StrategyLoader::load(const std::string& library_path) {
    // Unload any existing strategy first
    if (is_loaded()) {
        LOG_WARN("StrategyLoader: Unloading existing strategy before loading new one");
        unload();
    }

    library_path_ = library_path;
    last_error_.clear();

#ifdef _WIN32
    // Windows: LoadLibrary
    handle_ = LoadLibrary(library_path.c_str());
    if (!handle_) {
        DWORD error = GetLastError();
        LOG_ERROR("StrategyLoader: Failed to load library: {} (error: {})", library_path, std::to_string(error));
        return nullptr;
    }

    // Get function pointers
    create_func_ = reinterpret_cast<CreateStrategyFunc>(GetProcAddress(static_cast<HMODULE>(handle_), "create_strategy"));
    destroy_func_ = reinterpret_cast<DestroyStrategyFunc>(GetProcAddress(static_cast<HMODULE>(handle_), "destroy_strategy"));
    auto get_name_func = reinterpret_cast<GetNameFunc>(GetProcAddress(static_cast<HMODULE>(handle_), "get_strategy_name"));
    auto get_version_func = reinterpret_cast<GetVersionFunc>(GetProcAddress(static_cast<HMODULE>(handle_), "get_strategy_version"));
    auto set_logger_func = reinterpret_cast<SetLoggerFunc>(GetProcAddress(static_cast<HMODULE>(handle_), "set_logger_instance"));

#else
    // Unix: dlopen
    handle_ = dlopen(library_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle_) {
        LOG_ERROR("StrategyLoader: Failed to load library: {} (error: {})", library_path, std::string(dlerror()));
        return nullptr;
    }

    // Clear any existing errors
    dlerror();

    // Get function pointers
    create_func_ = reinterpret_cast<CreateStrategyFunc>(dlsym(handle_, "create_strategy"));
    destroy_func_ = reinterpret_cast<DestroyStrategyFunc>(dlsym(handle_, "destroy_strategy"));
    auto get_name_func = reinterpret_cast<GetNameFunc>(dlsym(handle_, "get_strategy_name"));
    auto get_version_func = reinterpret_cast<GetVersionFunc>(dlsym(handle_, "get_strategy_version"));
    auto set_logger_func = reinterpret_cast<SetLoggerFunc>(dlsym(handle_, "set_logger_instance"));
#endif

    // Verify required functions exist
    if (!create_func_ || !destroy_func_) {
        LOG_ERROR("StrategyLoader: Library missing required functions (create_strategy/destroy_strategy)");
        unload();
        return nullptr;
    }

    // Forward host logger to plugin so LOG_* macros in the plugin route to the
    // same initialized logger instance rather than the plugin-local singleton.
    if (set_logger_func) {
        set_logger_func(&slick::logger::Logger::instance());
    }

    // Log strategy info if available
    if (get_name_func && get_version_func) {
        LOG_INFO("StrategyLoader: Loading strategy '{}' v{} from {}",
                 get_name_func(), get_version_func(), library_path);
    } else {
        LOG_INFO("StrategyLoader: Loading strategy from {}", library_path);
    }

    // Create strategy instance
    IStrategy* raw_strategy = create_func_();
    if (!raw_strategy) {
        last_error_ = "create_strategy() returned nullptr";
        LOG_ERROR("StrategyLoader: {}", last_error_);
        unload();
        return nullptr;
    }

    // Wrap in shared_ptr with custom deleter
    // The deleter keeps a reference to the DLL handle to prevent premature unloading
    std::shared_ptr<IStrategy> strategy(raw_strategy, StrategyDeleter{destroy_func_, handle_});

    LOG_INFO("StrategyLoader: Successfully loaded strategy '{}' v{}",
             strategy->name(), strategy->version());

    // Clear handle_ so unload() won't free the library
    // The library will remain loaded until the process exits
    handle_ = nullptr;

    return strategy;
}

void StrategyLoader::unload() {
    if (!handle_) {
        return;
    }

    LOG_INFO("StrategyLoader: Releasing library handle for {}", library_path_);

    // Note: We only free the library if handle_ is still set
    // If a strategy was successfully loaded, handle_ is transferred to the deleter
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif

    handle_ = nullptr;
    create_func_ = nullptr;
    destroy_func_ = nullptr;
    library_path_.clear();
}

} // namespace slick::trader
