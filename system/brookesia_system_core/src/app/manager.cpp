/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "brookesia/system_core/macro_configs.h"
#if !BROOKESIA_SYSTEM_CORE_SYSTEM_ENABLE_DEBUG_LOG
#   define BROOKESIA_LOG_DISABLE_DEBUG_TRACE 1
#endif
#include "brookesia/system_core/app/package.hpp"
#include "private/filesystem.hpp"
#include "private/utils.hpp"
#include "private/heap_trace.hpp"
#include "private/system/impl.hpp"
#include "brookesia/service_manager/common.hpp"

#if BROOKESIA_SYSTEM_CORE_ENABLE_PROFILE_LOG
#   define SYSTEM_CORE_PROFILE_LOGI(...) BROOKESIA_LOGI(__VA_ARGS__)
#else
#   define SYSTEM_CORE_PROFILE_LOGI(...) do { if (false) { BROOKESIA_LOGI(__VA_ARGS__); } } while (0)
#endif

namespace esp_brookesia::system::core {

namespace {

constexpr const char *RUNTIME_APP_ID_CALL_CONTEXT_KEY = "brookesia.system.runtime_app_id";
using SteadyClock = std::chrono::steady_clock;
using SteadyTimePoint = SteadyClock::time_point;

bool is_runtime_service_call_context()
{
    return service::get_current_call_context_value(RUNTIME_APP_ID_CALL_CONTEXT_KEY).has_value();
}

int64_t elapsed_ms_since(SteadyTimePoint started_at, SteadyTimePoint ended_at = SteadyClock::now())
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(ended_at - started_at).count();
}

std::string with_cleanup_failure(
    std::string operation_error,
    std::string_view cleanup_operation,
    std::string_view cleanup_error
)
{
    operation_error += "; ";
    operation_error += cleanup_operation;
    operation_error += " failed: ";
    operation_error += cleanup_error;
    return operation_error;
}

// Retain every completion-cleanup failure while allowing the remaining requests to close.
void append_cleanup_failure(
    std::expected<void, std::string> &result,
    std::string_view operation,
    std::string_view error
)
{
    if (result) {
        std::string contextual_error(operation);
        contextual_error += " failed: ";
        contextual_error += error;
        result = std::unexpected(std::move(contextual_error));
        return;
    }
    result = std::unexpected(with_cleanup_failure(result.error(), operation, error));
}

} // namespace


namespace {

std::filesystem::path normalize_filesystem_path(const std::filesystem::path &path)
{
    return path.lexically_normal();
}

std::expected<gui::MountTarget, std::string> map_manifest_target(AppKind kind, const GuiScreenFlowEntry &entry)
{
    if (kind == AppKind::Runtime &&
            (entry.layer == GuiAppLayer::SystemBottom || entry.layer == GuiAppLayer::SystemTop)) {
        return std::unexpected("Runtime app screen flows can only use AppDefault or AppTop layers");
    }
    if (kind == AppKind::Runtime && entry.mount_mode != gui::MountStackMode::Replace) {
        return std::unexpected("Runtime app screen flows can only use Replace mount mode");
    }
    if (kind == AppKind::Runtime &&
            (entry.z_order < 0 || entry.z_order > BROOKESIA_SYSTEM_CORE_RUNTIME_APP_MAX_Z_ORDER)) {
        return std::unexpected(
                   "Runtime app screen flow z_order must be in range 0.." +
                   std::to_string(BROOKESIA_SYSTEM_CORE_RUNTIME_APP_MAX_Z_ORDER)
               );
    }

    gui::GuiLayer layer = gui::GuiLayer::Default;
    switch (entry.layer) {
    case GuiAppLayer::SystemBottom:
        layer = gui::GuiLayer::Bottom;
        break;
    case GuiAppLayer::AppDefault:
        layer = gui::GuiLayer::Default;
        break;
    case GuiAppLayer::AppTop:
        layer = gui::GuiLayer::Top;
        break;
    case GuiAppLayer::SystemTop:
        layer = gui::GuiLayer::System;
        break;
    default:
        return std::unexpected("Unsupported app GUI layer");
    }

    return gui::MountTarget{
        .display_id = {},
        .layer = layer,
        .mount_mode = entry.mount_mode,
        .z_order = entry.z_order,
    };
}

std::expected<void, std::string> validate_gui_descriptor(AppKind kind, const AppGuiDescriptor &descriptor)
{
    if ((descriptor.root_kind == GuiRootKind::None) && !descriptor.screen_flows.empty()) {
        return std::unexpected("App GUI descriptor without a root cannot declare screen flows");
    }
    if ((descriptor.root_kind != GuiRootKind::None) && descriptor.screen_flows.empty()) {
        return std::unexpected("App GUI descriptor with a root must declare at least one screen flow");
    }
    for (const auto &entry : descriptor.screen_flows) {
        auto target = map_manifest_target(kind, entry);
        if (!target) {
            return std::unexpected(target.error());
        }
    }
    return {};
}

void normalize_app_manifest(AppManifest &manifest)
{
    if (manifest.name.empty()) {
        manifest.name = resolve_app_display_name(manifest, "en");
    }
}

std::string get_lifecycle_app_name(const AppInfo &info)
{
    auto name = resolve_app_display_name(info.manifest, "en");
    if (!name.empty()) {
        return name;
    }
    if (!info.manifest.id.empty()) {
        return info.manifest.id;
    }
    return "App";
}

void show_lifecycle_error_dialog(
    System &system,
    const AppInfo &info,
    std::string_view action,
    std::string_view lifecycle,
    std::string_view error
)
{
    MessageDialogOptions options{
        .text = "Failed to " + std::string(action) + " " + get_lifecycle_app_name(info),
        .informative_text = std::string(lifecycle) + ": " + std::string(error),
        .icon = MessageDialogIcon::Warning,
        .buttons = {
            MessageDialogButton{
                .text = "Close",
                .role = MessageDialogButtonRole::Accept,
            },
        },
        .auto_close_ms = 3000,
    };
    auto dialog_result = system.show_system_message_dialog(std::move(options));
    if (!dialog_result) {
        BROOKESIA_LOGW(
            "Failed to show lifecycle error dialog: app_id(%1%), lifecycle(%2%), error(%3%)",
            info.app_id,
            lifecycle,
            dialog_result.error()
        );
    }
}

} // namespace

std::expected<System::Impl::AppRecord *, std::string> System::Impl::get_record(AppId app_id)
{
    auto it = apps_.find(app_id);
    if (it == apps_.end()) {
        return std::unexpected("App not found");
    }
    return &it->second;
}


std::expected<const System::Impl::AppRecord *, std::string> System::Impl::get_record(AppId app_id) const
{
    auto it = apps_.find(app_id);
    if (it == apps_.end()) {
        return std::unexpected("App not found");
    }
    return &it->second;
}


std::expected<void, std::string> System::Impl::dispatch_action(
    AppId app_id,
    std::string action,
    std::string path,
    std::string payload_json
)
{
    auto record_result = get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.manifest.kind == AppKind::Native) {
        if (!record.native_app || !record.context) {
            return std::unexpected("Native app is not available");
        }
        return record.native_app->on_action(*record.context, action);
    }
    return call_runtime_lifecycle(
               record,
               LIFECYCLE_ON_ACTION,
    {std::move(action), std::move(path), std::move(payload_json)}
           );
}


std::expected<void, std::string> System::Impl::dispatch_event(
    AppId app_id,
    std::string service_name,
    std::string event_name,
    std::string items_json
)
{
    auto record_result = get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.manifest.kind == AppKind::Native) {
        return {};
    }
    return call_runtime_lifecycle(
               record,
               LIFECYCLE_ON_EVENT,
    {std::move(service_name), std::move(event_name), std::move(items_json)}
           );
}


std::expected<AppId, std::string> System::install_app(std::shared_ptr<IApp> app)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    if (!app) {
        return std::unexpected("App is null");
    }
    auto manifest = app->get_manifest();
    manifest.kind = AppKind::Native;
    normalize_app_manifest(manifest);
    if (manifest.id.empty()) {
        return std::unexpected("App manifest id is empty");
    }
    if (impl_->manifest_id_to_app_.contains(manifest.id)) {
        return std::unexpected("App manifest id already installed");
    }
    auto gui_descriptor = app->get_gui_descriptor();
    auto descriptor_result = validate_gui_descriptor(AppKind::Native, gui_descriptor);
    if (!descriptor_result) {
        return std::unexpected(descriptor_result.error());
    }
    auto storage_result = impl_->ensure_app_storage_paths(manifest.id);
    if (!storage_result) {
        return std::unexpected(storage_result.error());
    }

    const AppId app_id = impl_->next_app_id_++;
    Impl::AppRecord record;
    record.info = AppInfo{
        .app_id = app_id,
        .manifest = std::move(manifest),
        .state = AppState::Installed,
        .last_error = "",
    };
    record.gui_descriptor = std::move(gui_descriptor);
    record.native_app = std::move(app);
    record.context = std::make_unique<AppContext>(*this, app_id);
    record.preload_dom = record.info.manifest.preload_dom;
    auto install_result = record.native_app->on_install(*record.context);
    if (!install_result) {
        return std::unexpected(install_result.error());
    }
    impl_->manifest_id_to_app_.emplace(record.info.manifest.id, app_id);
    auto [it, _] = impl_->apps_.emplace(app_id, std::move(record));
    auto gui_prepare_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                  SYSTEM_GUI_TASK_GROUP,
    [this, &it]() -> std::expected<void, std::string> {
        return impl_->prepare_installed_app_gui(it->second);
    },
    std::unexpected("Failed to post app GUI install preparation task")
                              );
    if (!gui_prepare_result) {
        auto rollback_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                   SYSTEM_GUI_TASK_GROUP,
        [this, &it]() -> std::expected<void, std::string> {
            return impl_->rollback_installed_app_gui(it->second);
        },
        std::unexpected("Failed to post app GUI install rollback task")
                               );
        if (!rollback_result) {
            BROOKESIA_LOGW("Failed to rollback app GUI resources: %1%", rollback_result.error());
            auto error = with_cleanup_failure(
                             gui_prepare_result.error(),
                             "GUI install rollback",
                             rollback_result.error()
                         );
            it->second.info.state = AppState::Error;
            it->second.info.last_error = error;
            return std::unexpected(std::move(error));
        }
        impl_->manifest_id_to_app_.erase(it->second.info.manifest.id);
        impl_->apps_.erase(it);
        return std::unexpected(gui_prepare_result.error());
    }
    auto hook_result = on_app_installed(it->second.info);
    if (!hook_result) {
        auto rollback_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                   SYSTEM_GUI_TASK_GROUP,
        [this, &it]() -> std::expected<void, std::string> {
            return impl_->rollback_installed_app_gui(it->second);
        },
        std::unexpected("Failed to post app GUI install rollback task")
                               );
        if (!rollback_result) {
            BROOKESIA_LOGW("Failed to rollback app GUI resources: %1%", rollback_result.error());
            auto error = with_cleanup_failure(
                             hook_result.error(),
                             "GUI install rollback",
                             rollback_result.error()
                         );
            it->second.info.state = AppState::Error;
            it->second.info.last_error = error;
            return std::unexpected(std::move(error));
        }
        impl_->manifest_id_to_app_.erase(it->second.info.manifest.id);
        impl_->apps_.erase(it);
        return std::unexpected(hook_result.error());
    }
    BROOKESIA_LOGI("Native app installed: id(%1%), manifest(%2%)", app_id, it->second.info.manifest.id);
    return app_id;
}

std::expected<AppId, std::string> System::install_runtime_app(const AppManifest &manifest)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    if (manifest.id.empty()) {
        return std::unexpected("Runtime app manifest id is empty");
    }
    if (impl_->manifest_id_to_app_.contains(manifest.id)) {
        return std::unexpected("App manifest id already installed");
    }
    auto heap_before_descriptor = heap_trace::capture();
    heap_trace::log("system.install_runtime", "before descriptor read", manifest.id, heap_before_descriptor);
    auto resource_descriptor = read_runtime_app_resource_descriptor(manifest);
    heap_trace::log(
        "system.install_runtime",
        "after descriptor read",
        manifest.id,
        heap_trace::capture(),
        &heap_before_descriptor
    );
    if (!resource_descriptor) {
        return std::unexpected(resource_descriptor.error());
    }
    auto descriptor_result = validate_gui_descriptor(AppKind::Runtime, resource_descriptor->gui);
    if (!descriptor_result) {
        return std::unexpected(descriptor_result.error());
    }
    auto storage_result = impl_->ensure_app_storage_paths(manifest.id);
    if (!storage_result) {
        return std::unexpected(storage_result.error());
    }

    const AppId app_id = impl_->next_app_id_++;
    Impl::AppRecord record;
    auto normalized_manifest = manifest;
    normalized_manifest.icon_id = std::move(resource_descriptor->icon_id);
    normalized_manifest.preload_dom = resource_descriptor->preload_dom;
    normalize_app_manifest(normalized_manifest);
    record.info = AppInfo{
        .app_id = app_id,
        .manifest = std::move(normalized_manifest),
        .state = AppState::Installed,
        .last_error = "",
    };
    record.gui_descriptor = std::move(resource_descriptor->gui);
    record.info.manifest.kind = AppKind::Runtime;
    record.context = std::make_unique<AppContext>(*this, app_id);
    record.preload_dom = record.info.manifest.preload_dom;
    impl_->manifest_id_to_app_.emplace(record.info.manifest.id, app_id);
    auto [it, _] = impl_->apps_.emplace(app_id, std::move(record));
    auto gui_prepare_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                  SYSTEM_GUI_TASK_GROUP,
    [this, &it]() -> std::expected<void, std::string> {
        return impl_->prepare_installed_app_gui(it->second);
    },
    std::unexpected("Failed to post app GUI install preparation task")
                              );
    if (!gui_prepare_result) {
        auto rollback_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                   SYSTEM_GUI_TASK_GROUP,
        [this, &it]() -> std::expected<void, std::string> {
            return impl_->rollback_installed_app_gui(it->second);
        },
        std::unexpected("Failed to post app GUI install rollback task")
                               );
        if (!rollback_result) {
            BROOKESIA_LOGW("Failed to rollback app GUI resources: %1%", rollback_result.error());
            auto error = with_cleanup_failure(
                             gui_prepare_result.error(),
                             "GUI install rollback",
                             rollback_result.error()
                         );
            it->second.info.state = AppState::Error;
            it->second.info.last_error = error;
            return std::unexpected(std::move(error));
        }
        impl_->manifest_id_to_app_.erase(it->second.info.manifest.id);
        impl_->apps_.erase(it);
        return std::unexpected(gui_prepare_result.error());
    }
    auto hook_result = on_app_installed(it->second.info);
    if (!hook_result) {
        auto rollback_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                   SYSTEM_GUI_TASK_GROUP,
        [this, &it]() -> std::expected<void, std::string> {
            return impl_->rollback_installed_app_gui(it->second);
        },
        std::unexpected("Failed to post app GUI install rollback task")
                               );
        if (!rollback_result) {
            BROOKESIA_LOGW("Failed to rollback app GUI resources: %1%", rollback_result.error());
            auto error = with_cleanup_failure(
                             hook_result.error(),
                             "GUI install rollback",
                             rollback_result.error()
                         );
            it->second.info.state = AppState::Error;
            it->second.info.last_error = error;
            return std::unexpected(std::move(error));
        }
        impl_->manifest_id_to_app_.erase(it->second.info.manifest.id);
        impl_->apps_.erase(it);
        return std::unexpected(hook_result.error());
    }
    BROOKESIA_LOGI("Runtime app installed: id(%1%), manifest(%2%)", app_id, manifest.id);
    return app_id;
}

std::expected<void, std::string> System::install_registered_apps()
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    auto providers = AppProviderRegistry::get_all_instances();
    for (auto &[name, provider] : providers) {
        if (!provider) {
            BROOKESIA_LOGW("App provider is null: name(%1%)", name);
            continue;
        }
        auto app = provider->create_app();
        std::expected<AppId, std::string> result = std::unexpected("No app installed");
        if (app) {
            result = install_app(std::move(app));
        } else {
            result = install_runtime_app(provider->get_manifest());
        }
        if (!result) {
            BROOKESIA_LOGW("Registered app install failed: provider(%1%), error(%2%)", name, result.error());
            return std::unexpected(result.error());
        }
    }
    return {};
}

// Uninstall an app while preserving failures from closing its app-owned input requests.
std::expected<void, std::string> System::uninstall_app(AppId app_id)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id]() {
            return uninstall_app(app_id);
        },
        std::unexpected("Failed to post app uninstall task")
               );
    }
    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.state == AppState::Running || record.info.state == AppState::Paused) {
        auto stop_result = stop_app(app_id);
        if (!stop_result) {
            return stop_result;
        }
    }
    const auto uninstalled_app = record.info;
    if (record.native_app && record.context) {
        record.native_app->on_uninstall(*record.context);
    } else if (record.info.manifest.kind == AppKind::Runtime) {
        auto lifecycle_result = impl_->call_runtime_lifecycle(record, LIFECYCLE_ON_UNINSTALL);
        if (!lifecycle_result) {
            BROOKESIA_LOGW("Runtime app uninstall hook failed: %1%", lifecycle_result.error());
        }
    }
    on_hide_app_loading(record.info.app_id);
    auto input_cleanup_result = close_app_input_requests(record.info.app_id);
    if (!input_cleanup_result) {
        BROOKESIA_LOGW(
            "Failed to close app input requests while uninstalling: app_id(%1%), error(%2%)",
            app_id,
            input_cleanup_result.error()
        );
    }
    impl_->cancel_app_timers(record);
    impl_->unload_runtime(record);
    auto cleanup_result = impl_->run_task_sync<std::expected<void, std::string>>(
                              SYSTEM_GUI_TASK_GROUP,
    [this, &record]() -> std::expected<void, std::string> {
        impl_->clear_pending_gui_bindings(record.info.app_id);
        return impl_->rollback_installed_app_gui(record);
    },
    std::unexpected("Failed to post GUI cleanup task")
                          );
    if (!cleanup_result) {
        BROOKESIA_LOGW("Failed to cleanup app GUI while uninstalling app: %1%", cleanup_result.error());
        auto cleanup_error = cleanup_result.error();
        if (!input_cleanup_result) {
            cleanup_error = with_cleanup_failure(
                                std::move(cleanup_error),
                                "app input request cleanup",
                                input_cleanup_result.error()
                            );
        }
        record.info.state = AppState::Error;
        record.info.last_error = cleanup_error;
        return std::unexpected(std::move(cleanup_error));
    }
    if (record.info.manifest.kind == AppKind::Runtime) {
        if (record.info.manifest.app_path.empty()) {
            return std::unexpected("Runtime app manifest app_path is empty");
        }
        const auto app_path = normalize_filesystem_path(std::filesystem::path(record.info.manifest.app_path));
        if (!impl_->is_managed_app_directory(app_path)) {
            return std::unexpected(
                       "Refuse to remove runtime app directory outside managed app roots: " +
                       app_path.generic_string()
                   );
        }
        auto remove_result = remove_path_tree(app_path, "runtime app directory");
        if (!remove_result) {
            return std::unexpected(remove_result.error());
        }
    }
    impl_->manifest_id_to_app_.erase(record.info.manifest.id);
    impl_->apps_.erase(app_id);
    if (impl_->active_app_id_ == app_id) {
        impl_->active_app_id_ = INVALID_APP_ID;
    }
    auto hook_result = on_app_uninstalled(uninstalled_app);
    if (!hook_result) {
        BROOKESIA_LOGW("App uninstall hook failed: id(%1%), error(%2%)", app_id, hook_result.error());
    }
    BROOKESIA_LOGI("App uninstalled: id(%1%)", app_id);
    return input_cleanup_result;
}

std::expected<void, std::string> System::start_app(AppId app_id)
{
    return start_app(app_id, AppStartOptions{});
}

std::expected<void, std::string> System::start_app(AppId app_id, const AppStartOptions &options)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, options]() {
            return start_app(app_id, options);
        },
        std::unexpected("Failed to post app start task")
               );
    }
    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.state == AppState::Running) {
        return {};
    }
    // A fresh lifecycle must never inherit a prior power/input gate.
    record.input_quiesced = false;
    const auto start_profile_started_at = SteadyClock::now();
    auto log_start_profile = [&](const char *stage, SteadyTimePoint stage_started_at) {
        const auto now = SteadyClock::now();
        SYSTEM_CORE_PROFILE_LOGI(
            "App start profile: id(%1%), manifest(%2%), stage(%3%), elapsed_ms(%4%), total_ms(%5%)",
            app_id,
            record.info.manifest.id,
            stage,
            elapsed_ms_since(stage_started_at, now),
            elapsed_ms_since(start_profile_started_at, now)
        );
    };
    auto heap_before_start = heap_trace::capture();
    heap_trace::log("system.start", "before start", record.info.manifest.id, heap_before_start);
    record.info.state = AppState::Starting;
    record.info.last_error.clear();

    std::optional<Impl::TransientOverlayRecord> launch_transition;
    const auto launch_transition_started_at = SteadyClock::now();
    auto transition_result = impl_->show_app_launch_transition(record, options);
    if (transition_result) {
        launch_transition = *transition_result;
    } else {
        BROOKESIA_LOGW(
            "Failed to show app launch transition: app_id(%1%), error(%2%)",
            app_id,
            transition_result.error()
        );
    }
    log_start_profile("launch_transition", launch_transition_started_at);
    heap_trace::log(
        "system.start",
        "after launch transition",
        record.info.manifest.id,
        heap_trace::capture(),
        &heap_before_start
    );
    lib_utils::FunctionGuard transition_cleanup([this, &launch_transition]() {
        impl_->hide_transient_overlay(launch_transition);
    });

    const auto gui_prepare_started_at = SteadyClock::now();
    auto gui_prepare_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                  SYSTEM_GUI_TASK_GROUP,
    [this, &record]() -> std::expected<void, std::string> {
        const auto gui_profile_started_at = SteadyClock::now();
        auto log_gui_profile = [&](const char *stage, SteadyTimePoint stage_started_at)
        {
            const auto now = SteadyClock::now();
            SYSTEM_CORE_PROFILE_LOGI(
                "App GUI prepare profile: manifest(%1%), stage(%2%), elapsed_ms(%3%), total_ms(%4%)",
                record.info.manifest.id,
                stage,
                elapsed_ms_since(stage_started_at, now),
                elapsed_ms_since(gui_profile_started_at, now)
            );
        };
        auto heap_before_gui = heap_trace::capture();
        heap_trace::log("system.gui", "before prepare", record.info.manifest.id, heap_before_gui);
        const auto resource_started_at = SteadyClock::now();
        auto resource_result = impl_->register_app_gui_resources(record);
        if (!resource_result)
        {
            log_gui_profile("resource_registration_failed", resource_started_at);
            return resource_result;
        }
        log_gui_profile("resource_registration", resource_started_at);
        heap_trace::log(
            "system.gui",
            "after resource registration",
            record.info.manifest.id,
            heap_trace::capture(),
            &heap_before_gui
        );
        const auto root_started_at = SteadyClock::now();
        auto gui_result = impl_->ensure_gui_loaded(record);
        if (!gui_result)
        {
            log_gui_profile("root_load_failed", root_started_at);
            impl_->unregister_app_gui_resources(record);
            return gui_result;
        }
        log_gui_profile("root_load", root_started_at);
        heap_trace::log(
            "system.gui",
            "after root load",
            record.info.manifest.id,
            heap_trace::capture(),
            &heap_before_gui
        );
        if (record.document_id.has_value())
        {
            const auto screen_flow_started_at = SteadyClock::now();
            for (const auto &flow_entry : record.gui_descriptor.screen_flows) {
                auto target = map_manifest_target(record.info.manifest.kind, flow_entry);
                if (!target) {
                    log_gui_profile("screen_flow_mount_failed", screen_flow_started_at);
                    auto unload_result = impl_->unload_gui(record);
                    if (!unload_result) {
                        return std::unexpected(
                                   with_cleanup_failure(
                                       target.error(),
                                       "GUI rollback",
                                       unload_result.error()
                                   )
                               );
                    }
                    impl_->unregister_app_gui_resources(record);
                    return std::unexpected(target.error());
                }
                if (!impl_->gui_runtime_->has_screen_flow(*record.document_id, flow_entry.screen_flow)) {
                    log_gui_profile("screen_flow_mount_failed", screen_flow_started_at);
                    auto unload_result = impl_->unload_gui(record);
                    if (!unload_result) {
                        return std::unexpected(
                                   with_cleanup_failure(
                                       "App GUI screen flow is not found: " + flow_entry.screen_flow,
                                       "GUI rollback",
                                       unload_result.error()
                                   )
                               );
                    }
                    impl_->unregister_app_gui_resources(record);
                    return std::unexpected("App GUI screen flow is not found: " + flow_entry.screen_flow);
                }
                auto flow_result = impl_->gui_runtime_->start_screen_flow(
                                       *record.document_id,
                                       flow_entry.screen_flow,
                                       *target
                                   );
                if (!flow_result) {
                    log_gui_profile("screen_flow_mount_failed", screen_flow_started_at);
                    auto unload_result = impl_->unload_gui(record);
                    if (!unload_result) {
                        return std::unexpected(
                                   with_cleanup_failure(
                                       flow_result.error(),
                                       "GUI rollback",
                                       unload_result.error()
                                   )
                               );
                    }
                    impl_->unregister_app_gui_resources(record);
                    return flow_result;
                }
                heap_trace::log(
                    "system.gui",
                    "after screen flow mount",
                    record.info.manifest.id,
                    heap_trace::capture(),
                    &heap_before_gui
                );
            }
            log_gui_profile("screen_flow_mount", screen_flow_started_at);
        }
        log_gui_profile("total", gui_profile_started_at);
        return {};
    },
    std::unexpected("Failed to post app GUI prepare task")
                              );
    log_start_profile("gui_prepare", gui_prepare_started_at);
    if (!gui_prepare_result) {
        record.info.state = AppState::Error;
        record.info.last_error = gui_prepare_result.error();
        on_app_start_failed(record.info, gui_prepare_result.error());
        show_lifecycle_error_dialog(*this, record.info, "start", LIFECYCLE_ON_START, gui_prepare_result.error());
        return gui_prepare_result;
    }

    std::expected<void, std::string> start_result = {};
    if (record.info.manifest.kind == AppKind::Native) {
        if (!record.native_app || !record.context) {
            start_result = std::unexpected("Native app is not available");
        } else {
            const auto native_start_started_at = SteadyClock::now();
            start_result = record.native_app->on_start(*record.context);
            log_start_profile("native_on_start", native_start_started_at);
        }
    } else {
        auto heap_before_runtime = heap_trace::capture();
        heap_trace::log("system.runtime", "before runtime load", record.info.manifest.id, heap_before_runtime);
        const auto runtime_load_started_at = SteadyClock::now();
        start_result = impl_->ensure_runtime_loaded(record);
        log_start_profile("runtime_load", runtime_load_started_at);
        heap_trace::log(
            "system.runtime",
            "after runtime load",
            record.info.manifest.id,
            heap_trace::capture(),
            &heap_before_runtime
        );
        if (start_result) {
            if (!record.runtime_started) {
                auto heap_before_runtime_start = heap_trace::capture();
                heap_trace::log(
                    "system.runtime",
                    "before runtime start_app",
                    record.info.manifest.id,
                    heap_before_runtime_start
                );
                const auto runtime_start_started_at = SteadyClock::now();
                start_result = impl_->runtime_->start_app(*record.runtime_app_id);
                record.runtime_started = start_result.has_value();
                log_start_profile("runtime_start_app", runtime_start_started_at);
                heap_trace::log(
                    "system.runtime",
                    "after runtime start_app",
                    record.info.manifest.id,
                    heap_trace::capture(),
                    &heap_before_runtime_start
                );
            }
            if (start_result) {
                auto heap_before_lifecycle = heap_trace::capture();
                heap_trace::log(
                    "system.runtime",
                    "before lifecycle on_start",
                    record.info.manifest.id,
                    heap_before_lifecycle
                );
                const auto runtime_lifecycle_started_at = SteadyClock::now();
                start_result = impl_->call_runtime_lifecycle(record, LIFECYCLE_ON_START);
                log_start_profile("runtime_lifecycle_on_start", runtime_lifecycle_started_at);
                heap_trace::log(
                    "system.runtime",
                    "after lifecycle on_start",
                    record.info.manifest.id,
                    heap_trace::capture(),
                    &heap_before_lifecycle
                );
            }
        }
    }

    if (!start_result) {
        auto rollback_result = impl_->run_task_sync<std::expected<void, std::string>>(
                                   SYSTEM_GUI_TASK_GROUP,
        [this, &record]() -> std::expected<void, std::string> {
            impl_->clear_pending_gui_bindings(record.info.app_id);
            auto unload_result = impl_->unload_gui(record);
            if (!unload_result) {
                return unload_result;
            }
            impl_->unregister_app_gui_resources(record);
            return {};
        },
        std::unexpected("Failed to post app GUI rollback task")
                               );
        if (!rollback_result) {
            BROOKESIA_LOGW("Failed to rollback app GUI after start failure: %1%", rollback_result.error());
            start_result = std::unexpected(
                               with_cleanup_failure(
                                   start_result.error(),
                                   "GUI rollback",
                                   rollback_result.error()
                               )
                           );
        }
        record.info.state = AppState::Error;
        record.info.last_error = start_result.error();
        on_app_start_failed(record.info, start_result.error());
        show_lifecycle_error_dialog(*this, record.info, "start", LIFECYCLE_ON_START, start_result.error());
        return start_result;
    }
    record.info.state = AppState::Running;
    impl_->active_app_id_ = app_id;
    const auto started_hook_started_at = SteadyClock::now();
    auto hook_result = on_app_started(record.info);
    log_start_profile("on_app_started_hook", started_hook_started_at);
    if (!hook_result) {
        BROOKESIA_LOGW("App started hook failed, stopping app: id(%1%), error(%2%)", app_id, hook_result.error());
        auto rollback_stop_result = stop_app(app_id);
        if (!rollback_stop_result) {
            BROOKESIA_LOGW(
                "Failed to stop app after on_app_started hook failure: id(%1%), error(%2%)",
                app_id,
                rollback_stop_result.error()
            );
            record.info.state = AppState::Error;
            record.info.last_error = hook_result.error();
            if (impl_->active_app_id_ == app_id) {
                impl_->active_app_id_ = INVALID_APP_ID;
            }
            on_app_start_failed(record.info, hook_result.error());
        }
        show_lifecycle_error_dialog(*this, record.info, "start", LIFECYCLE_ON_START, hook_result.error());
        return hook_result;
    }
    impl_->hide_transient_overlay(launch_transition);
    transition_cleanup.release();
    heap_trace::log(
        "system.start",
        "after app started",
        record.info.manifest.id,
        heap_trace::capture(),
        &heap_before_start
    );
    if constexpr (heap_trace::enabled) {
        const auto manifest_id = record.info.manifest.id;
        const auto baseline = heap_before_start;
        const auto post_result = impl_->task_scheduler_->post_delayed(
        [manifest_id, baseline]() {
            heap_trace::log(
                "system.start",
                "after app started delayed 500ms",
                manifest_id,
                heap_trace::capture(),
                &baseline
            );
        },
        500,
        nullptr,
        SYSTEM_APP_TASK_GROUP
                                 );
        if (!post_result) {
            BROOKESIA_LOGW("Failed to post delayed app heap snapshot: manifest(%1%)", manifest_id);
        }
    }
    BROOKESIA_LOGI(
        "App started: id(%1%), manifest(%2%), total_ms(%3%)",
        app_id,
        record.info.manifest.id,
        elapsed_ms_since(start_profile_started_at)
    );
    return {};
}

// Stop an app while closing every app-owned input request and retaining dispatch failures.
std::expected<void, std::string> System::stop_app(AppId app_id)
{
    BROOKESIA_LOG_TRACE_GUARD_WITH_THIS();
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id]() {
            return stop_app(app_id);
        },
        std::unexpected("Failed to post app stop task")
               );
    }
    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.state == AppState::Stopped || record.info.state == AppState::Installed) {
        on_hide_app_loading(app_id);
        return close_app_input_requests(app_id);
    }
    auto heap_before_stop = heap_trace::capture();
    heap_trace::log("system.stop", "before stop", record.info.manifest.id, heap_before_stop);
    record.info.state = AppState::Stopping;
    on_hide_app_loading(app_id);
    auto input_cleanup_result = close_app_input_requests(app_id);
    if (!input_cleanup_result) {
        BROOKESIA_LOGW(
            "Failed to close app input requests while stopping: app_id(%1%), error(%2%)",
            app_id,
            input_cleanup_result.error()
        );
    }
    std::expected<void, std::string> stop_result = {};
    if (record.info.manifest.kind == AppKind::Native) {
        if (record.native_app && record.context) {
            stop_result = record.native_app->on_stop(*record.context);
        }
    } else {
        auto heap_before_lifecycle = heap_trace::capture();
        heap_trace::log(
            "system.runtime",
            "before lifecycle on_stop",
            record.info.manifest.id,
            heap_before_lifecycle
        );
        stop_result = impl_->call_runtime_lifecycle(record, LIFECYCLE_ON_STOP);
        heap_trace::log(
            "system.runtime",
            "after lifecycle on_stop",
            record.info.manifest.id,
            heap_trace::capture(),
            &heap_before_lifecycle
        );
        if (stop_result && impl_->runtime_ && record.runtime_app_id.has_value()) {
            auto heap_before_runtime_stop = heap_trace::capture();
            heap_trace::log(
                "system.runtime",
                "before runtime stop_app",
                record.info.manifest.id,
                heap_before_runtime_stop
            );
            stop_result = impl_->runtime_->stop_app(*record.runtime_app_id);
            heap_trace::log(
                "system.runtime",
                "after runtime stop_app",
                record.info.manifest.id,
                heap_trace::capture(),
                &heap_before_runtime_stop
            );
        }
        record.runtime_started = false;
    }
    if (!input_cleanup_result) {
        if (!stop_result) {
            stop_result = std::unexpected(
                              with_cleanup_failure(
                                  stop_result.error(),
                                  "app input request cleanup",
                                  input_cleanup_result.error()
                              )
                          );
        } else {
            stop_result = std::unexpected(input_cleanup_result.error());
        }
    }
    // AppState::Stopping already prevents timer dispatch. Let the app release
    // its own timer IDs first, then enforce cleanup even if on_stop() failed.
    impl_->cancel_app_timers(record);
    auto cleanup_result = impl_->run_task_sync<std::expected<void, std::string>>(
                              SYSTEM_GUI_TASK_GROUP,
    [this, &record]() -> std::expected<void, std::string> {
        auto heap_before_gui_cleanup = heap_trace::capture();
        heap_trace::log("system.gui", "before cleanup", record.info.manifest.id, heap_before_gui_cleanup);
        auto result = impl_->cleanup_stopped_app_gui(record);
        heap_trace::log(
            "system.gui",
            "after cleanup",
            record.info.manifest.id,
            heap_trace::capture(),
            &heap_before_gui_cleanup
        );
        return result;
    },
    std::unexpected("Failed to post app GUI cleanup task")
                          );
    if (!cleanup_result) {
        BROOKESIA_LOGW("Failed to cleanup app GUI while stopping app: %1%", cleanup_result.error());
        if (!stop_result) {
            stop_result = std::unexpected(
                              with_cleanup_failure(
                                  stop_result.error(),
                                  "GUI cleanup",
                                  cleanup_result.error()
                              )
                          );
        } else {
            stop_result = std::unexpected(cleanup_result.error());
        }
    }
    if (!stop_result) {
        record.info.state = AppState::Error;
        record.info.last_error = stop_result.error();
        if (impl_->active_app_id_ == app_id) {
            impl_->active_app_id_ = INVALID_APP_ID;
        }
        on_app_stop_failed(record.info, stop_result.error());
        show_lifecycle_error_dialog(*this, record.info, "stop", LIFECYCLE_ON_STOP, stop_result.error());
        return stop_result;
    }
    record.info.state = AppState::Stopped;
    if (impl_->active_app_id_ == app_id) {
        impl_->active_app_id_ = INVALID_APP_ID;
    }
    on_app_stopped(record.info);
    heap_trace::log(
        "system.stop",
        "after stopped",
        record.info.manifest.id,
        heap_trace::capture(),
        &heap_before_stop
    );
    BROOKESIA_LOGI("App stopped: id(%1%), manifest(%2%)", app_id, record.info.manifest.id);
    return {};
}

std::expected<void, std::string> System::pause_app(AppId app_id)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id]() {
            return pause_app(app_id);
        },
        std::unexpected("Failed to post app pause task")
               );
    }
    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.manifest.kind == AppKind::Native && record.native_app && record.context) {
        auto result = record.native_app->on_pause(*record.context);
        if (!result) {
            show_lifecycle_error_dialog(*this, record.info, "pause", LIFECYCLE_ON_PAUSE, result.error());
            return result;
        }
    } else if (record.info.manifest.kind == AppKind::Runtime) {
        auto result = impl_->call_runtime_lifecycle(record, LIFECYCLE_ON_PAUSE);
        if (!result) {
            show_lifecycle_error_dialog(*this, record.info, "pause", LIFECYCLE_ON_PAUSE, result.error());
            return result;
        }
    }
    record.info.state = AppState::Paused;
    return {};
}

std::expected<void, std::string> System::resume_app(AppId app_id)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id]() {
            return resume_app(app_id);
        },
        std::unexpected("Failed to post app resume task")
               );
    }
    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    if (record.info.manifest.kind == AppKind::Native && record.native_app && record.context) {
        auto result = record.native_app->on_resume(*record.context);
        if (!result) {
            show_lifecycle_error_dialog(*this, record.info, "resume", LIFECYCLE_ON_RESUME, result.error());
            return result;
        }
    } else if (record.info.manifest.kind == AppKind::Runtime) {
        auto result = impl_->call_runtime_lifecycle(record, LIFECYCLE_ON_RESUME);
        if (!result) {
            show_lifecycle_error_dialog(*this, record.info, "resume", LIFECYCLE_ON_RESUME, result.error());
            return result;
        }
    }
    record.info.state = AppState::Running;
    impl_->active_app_id_ = app_id;
    return {};
}

// Gate app-scoped input, then drain queued AppInput and GUI work asynchronously.
std::expected<void, std::string> System::app_quiesce_input(
    AppId app_id,
    AppInputQuiescedHandler on_drained
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
            SYSTEM_APP_TASK_GROUP,
            [this, app_id, on_drained = std::move(on_drained)]() mutable {
                return app_quiesce_input(app_id, std::move(on_drained));
            },
            std::unexpected("Failed to post app input quiescence task")
        );
    }

    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    auto &record = *record_result.value();
    record.input_quiesced = true;

    auto barrier_result = impl_->post_app_input_barrier(
        app_id,
        [this, app_id, on_drained = std::move(on_drained)]() mutable {
            auto drain_result = gui_drain_pending_work(app_id);
            if (!drain_result) {
                BROOKESIA_LOGW(
                    "App input quiescence drain failed: app_id(%1%), error(%2%)",
                    app_id,
                    drain_result.error()
                );
            }
            if (on_drained) {
                on_drained(std::move(drain_result));
            }
        }
    );
    if (!barrier_result) {
        record.input_quiesced = false;
        BROOKESIA_LOGW(
            "Failed to post app input quiescence barrier: app_id(%1%), error(%2%)",
            app_id,
            barrier_result.error()
        );
        return std::unexpected(
            "Failed to post app input quiescence barrier: " +
            barrier_result.error()
        );
    }
    return {};
}


// Reopen app-scoped input only after the owner has restored its active state.
std::expected<void, std::string> System::app_resume_input(AppId app_id)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
            SYSTEM_APP_TASK_GROUP,
            [this, app_id]() {
                return app_resume_input(app_id);
            },
            std::unexpected("Failed to post app input resume task")
        );
    }

    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }
    record_result.value()->input_quiesced = false;
    return {};
}


std::expected<void, std::string> System::request_close_app(AppId app_id)
{
    return stop_app(app_id);
}

std::expected<void, std::string> System::show_app_loading(AppId app_id)
{
    auto app = get_app(app_id);
    if (!app) {
        return std::unexpected("App not found");
    }
    return on_show_app_loading(app_id);
}

std::expected<void, std::string> System::hide_app_loading(AppId app_id)
{
    auto app = get_app(app_id);
    if (!app) {
        return std::unexpected("App not found");
    }
    on_hide_app_loading(app_id);
    return {};
}

std::expected<KeyboardRequestId, std::string> System::show_app_keyboard(
    AppId app_id,
    KeyboardRequestOptions options,
    KeyboardResultHandler handler
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<KeyboardRequestId, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, options = std::move(options), handler = std::move(handler)]() mutable {
            return show_app_keyboard(app_id, std::move(options), std::move(handler));
        },
        std::unexpected("Failed to post app keyboard request task")
               );
    }

    auto record_result = impl_->get_record(app_id);
    if (!record_result) {
        return std::unexpected(record_result.error());
    }

    KeyboardRequestId request_id = impl_->next_keyboard_request_id_++;
    if (request_id == INVALID_KEYBOARD_REQUEST_ID) {
        request_id = impl_->next_keyboard_request_id_++;
    }
    Impl::KeyboardRequestRecord keyboard_record{
        .request_id = request_id,
        .app_id = app_id,
        .handler = std::move(handler),
    };
    impl_->keyboard_requests_.emplace(request_id, std::move(keyboard_record));

    auto show_result = on_show_app_keyboard(app_id, request_id, options);
    if (!show_result) {
        impl_->keyboard_requests_.erase(request_id);
        return std::unexpected(show_result.error());
    }
    return request_id;
}

// Publish keyboard closure system-wide while dispatching its app callback through AppInput.
std::expected<void, std::string> System::publish_keyboard_closed(
    KeyboardResult result,
    KeyboardResultHandler handler
)
{
    std::expected<void, std::string> callback_result = {};
    if (handler) {
        callback_result = impl_->post_app_input_task(
            result.app_id,
            [handler = std::move(handler), result]() mutable {
                handler(result);
            }
        );
        if (!callback_result) {
            BROOKESIA_LOGW(
                "Failed to post app keyboard completion callback: app_id(%1%), request_id(%2%), error(%3%)",
                result.app_id,
                result.request_id,
                callback_result.error()
            );
        }
    }
    if (impl_->system_service_) {
        impl_->system_service_->publish_keyboard_closed(result);
    }
    if (!callback_result) {
        return std::unexpected(
                   "Failed to post app keyboard completion callback: " + callback_result.error()
               );
    }
    return {};
}

// Close a keyboard request and queue its cancelled completion behind prior app input.
std::expected<void, std::string> System::hide_app_keyboard(AppId app_id, KeyboardRequestId request_id)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, request_id]() {
            return hide_app_keyboard(app_id, request_id);
        },
        std::unexpected("Failed to post app keyboard hide task")
               );
    }

    auto it = impl_->keyboard_requests_.find(request_id);
    if (it == impl_->keyboard_requests_.end()) {
        return std::unexpected("Keyboard request not found");
    }
    if (it->second.app_id != app_id) {
        return std::unexpected("Keyboard request does not belong to app");
    }

    on_hide_app_keyboard(app_id, request_id);
    auto record = std::move(it->second);
    impl_->keyboard_requests_.erase(it);
    KeyboardResult result{
        .request_id = record.request_id,
        .app_id = record.app_id,
        .confirmed = false,
        .text = {},
    };
    return publish_keyboard_closed(std::move(result), std::move(record.handler));
}

// Complete a keyboard request and queue its confirmed result behind prior app input.
std::expected<void, std::string> System::complete_app_keyboard(
    AppId app_id,
    KeyboardRequestId request_id,
    bool confirmed,
    std::string text
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, request_id, confirmed, text = std::move(text)]() mutable {
            return complete_app_keyboard(app_id, request_id, confirmed, std::move(text));
        },
        std::unexpected("Failed to post app keyboard completion task")
               );
    }

    auto it = impl_->keyboard_requests_.find(request_id);
    if (it == impl_->keyboard_requests_.end()) {
        return std::unexpected("Keyboard request not found");
    }
    if (it->second.app_id != app_id) {
        return std::unexpected("Keyboard request does not belong to app");
    }

    auto record = std::move(it->second);
    impl_->keyboard_requests_.erase(it);
    KeyboardResult result{
        .request_id = record.request_id,
        .app_id = record.app_id,
        .confirmed = confirmed,
        .text = std::move(text),
    };
    return publish_keyboard_closed(std::move(result), std::move(record.handler));
}

std::expected<MessageDialogRequestId, std::string> System::show_app_message_dialog(
    AppId app_id,
    MessageDialogOptions options,
    MessageDialogResultHandler handler
)
{
    if (impl_->should_schedule_app_task()) {
        if (is_runtime_service_call_context()) {
            // Runtime sync service calls are handled on the service call strand while the
            // SystemApp task that initiated the call is blocked waiting. Re-posting to the
            // serial SystemApp queue would deadlock.
            return enqueue_message_dialog(app_id, true, std::move(options), std::move(handler));
        }
        return impl_->run_task_sync<std::expected<MessageDialogRequestId, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, options = std::move(options), handler = std::move(handler)]() mutable {
            return show_app_message_dialog(app_id, std::move(options), std::move(handler));
        },
        std::unexpected("Failed to post app message dialog request task")
               );
    }

    return enqueue_message_dialog(app_id, true, std::move(options), std::move(handler));
}

std::expected<void, std::string> System::hide_app_message_dialog(AppId app_id, MessageDialogRequestId request_id)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, request_id]() {
            return hide_app_message_dialog(app_id, request_id);
        },
        std::unexpected("Failed to post app message dialog hide task")
               );
    }

    return close_message_dialog(app_id, request_id, -1, MessageDialogCloseReason::Closed, true, true);
}

std::expected<void, std::string> System::update_app_message_dialog(
    AppId app_id,
    MessageDialogRequestId request_id,
    MessageDialogOptions options
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, request_id, options = std::move(options)]() mutable {
            return update_app_message_dialog(app_id, request_id, std::move(options));
        },
        std::unexpected("Failed to post app message dialog update task")
               );
    }

    return update_message_dialog(app_id, request_id, std::move(options), true);
}

std::expected<void, std::string> System::complete_app_message_dialog(
    AppId app_id,
    MessageDialogRequestId request_id,
    int32_t button_index,
    MessageDialogCloseReason reason
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, app_id, request_id, button_index, reason]() {
            return complete_app_message_dialog(app_id, request_id, button_index, reason);
        },
        std::unexpected("Failed to post app message dialog completion task")
               );
    }

    return close_message_dialog(app_id, request_id, button_index, reason, true, false);
}

std::expected<MessageDialogRequestId, std::string> System::show_system_message_dialog(
    MessageDialogOptions options,
    MessageDialogResultHandler handler
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<MessageDialogRequestId, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, options = std::move(options), handler = std::move(handler)]() mutable {
            return show_system_message_dialog(std::move(options), std::move(handler));
        },
        std::unexpected("Failed to post system message dialog request task")
               );
    }

    return enqueue_message_dialog(INVALID_APP_ID, false, std::move(options), std::move(handler));
}

std::expected<void, std::string> System::hide_system_message_dialog(MessageDialogRequestId request_id)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, request_id]() {
            return hide_system_message_dialog(request_id);
        },
        std::unexpected("Failed to post system message dialog hide task")
               );
    }

    return close_message_dialog(INVALID_APP_ID, request_id, -1, MessageDialogCloseReason::Closed, true, true);
}

std::expected<void, std::string> System::update_system_message_dialog(
    MessageDialogRequestId request_id,
    MessageDialogOptions options
)
{
    if (impl_->should_schedule_app_task()) {
        return impl_->run_task_sync<std::expected<void, std::string>>(
                   SYSTEM_APP_TASK_GROUP,
        [this, request_id, options = std::move(options)]() mutable {
            return update_system_message_dialog(request_id, std::move(options));
        },
        std::unexpected("Failed to post system message dialog update task")
               );
    }

    return update_message_dialog(INVALID_APP_ID, request_id, std::move(options), true);
}

std::expected<MessageDialogRequestId, std::string> System::enqueue_message_dialog(
    AppId app_id,
    bool require_app,
    MessageDialogOptions options,
    MessageDialogResultHandler handler
)
{
    if (options.buttons.size() > 3) {
        return std::unexpected("Message dialog supports at most 3 buttons");
    }
    if (require_app) {
        auto record_result = impl_->get_record(app_id);
        if (!record_result) {
            return std::unexpected(record_result.error());
        }
    }

    MessageDialogRequestId request_id = impl_->next_message_dialog_request_id_++;
    if (request_id == INVALID_MESSAGE_DIALOG_REQUEST_ID) {
        request_id = impl_->next_message_dialog_request_id_++;
    }
    Impl::MessageDialogRequestRecord request{
        .request_id = request_id,
        .app_id = app_id,
        .options = std::move(options),
        .handler = std::move(handler),
    };
    impl_->message_dialog_requests_.emplace(request_id, std::move(request));

    if (impl_->active_message_dialog_request_id_.has_value()) {
        impl_->queued_message_dialog_requests_.push_back(request_id);
        return request_id;
    }

    auto it = impl_->message_dialog_requests_.find(request_id);
    impl_->active_message_dialog_request_id_ = request_id;
    auto show_result = on_show_message_dialog(app_id, request_id, it->second.options);
    if (!show_result) {
        impl_->active_message_dialog_request_id_.reset();
        impl_->message_dialog_requests_.erase(request_id);
        return std::unexpected(show_result.error());
    }

    return request_id;
}

// Advance the dialog queue without losing completion-dispatch failures from rejected entries.
std::expected<void, std::string> System::show_next_message_dialog()
{
    if (impl_->active_message_dialog_request_id_.has_value()) {
        return {};
    }

    std::expected<void, std::string> completion_result = {};
    while (!impl_->queued_message_dialog_requests_.empty()) {
        const auto request_id = impl_->queued_message_dialog_requests_.front();
        impl_->queued_message_dialog_requests_.erase(impl_->queued_message_dialog_requests_.begin());
        auto it = impl_->message_dialog_requests_.find(request_id);
        if (it == impl_->message_dialog_requests_.end()) {
            continue;
        }

        impl_->active_message_dialog_request_id_ = request_id;
        auto show_result = on_show_message_dialog(it->second.app_id, request_id, it->second.options);
        if (show_result) {
            return completion_result;
        }

        BROOKESIA_LOGW("Failed to show queued message dialog: %1%", show_result.error());
        auto record = std::move(it->second);
        impl_->message_dialog_requests_.erase(it);
        impl_->active_message_dialog_request_id_.reset();
        auto publish_result = publish_message_dialog_closed(
            MessageDialogResult{
                .request_id = record.request_id,
                .app_id = record.app_id,
                .button_index = -1,
                .button_role = MessageDialogButtonRole::Invalid,
                .reason = MessageDialogCloseReason::Closed,
            },
            std::move(record.handler)
        );
        if (!publish_result) {
            BROOKESIA_LOGW(
                "Failed to dispatch rejected queued message dialog completion: request_id(%1%), error(%2%)",
                record.request_id,
                publish_result.error()
            );
            append_cleanup_failure(
                completion_result,
                "queued message dialog completion dispatch",
                publish_result.error()
            );
        }
    }

    on_message_dialog_idle();
    return completion_result;
}

void System::remove_queued_message_dialog(MessageDialogRequestId request_id)
{
    auto &queue = impl_->queued_message_dialog_requests_;
    queue.erase(std::remove(queue.begin(), queue.end(), request_id), queue.end());
}

// Publish dialog closure system-wide while gating only app-owned completion callbacks.
std::expected<void, std::string> System::publish_message_dialog_closed(
    MessageDialogResult result,
    MessageDialogResultHandler handler
)
{
    std::expected<void, std::string> callback_result = {};
    if (handler) {
        if (result.app_id == INVALID_APP_ID) {
            handler(result);
        } else {
            callback_result = impl_->post_app_input_task(
                result.app_id,
                [handler = std::move(handler), result]() mutable {
                    handler(result);
                }
            );
            if (!callback_result) {
                BROOKESIA_LOGW(
                    "Failed to post app message dialog completion callback: app_id(%1%), request_id(%2%), error(%3%)",
                    result.app_id,
                    result.request_id,
                    callback_result.error()
                );
            }
        }
    }
    if (impl_->system_service_) {
        impl_->system_service_->publish_message_dialog_closed(result);
    }
    if (!callback_result) {
        return std::unexpected(
                   "Failed to post app message dialog completion callback: " + callback_result.error()
               );
    }
    return {};
}

// Close one active or queued dialog, dispatch completion, then preserve queue progress and errors.
std::expected<void, std::string> System::close_message_dialog(
    AppId app_id,
    MessageDialogRequestId request_id,
    int32_t button_index,
    MessageDialogCloseReason reason,
    bool validate_owner,
    bool notify_shell
)
{
    auto it = impl_->message_dialog_requests_.find(request_id);
    if (it == impl_->message_dialog_requests_.end()) {
        return std::unexpected("Message dialog request not found");
    }
    if (validate_owner && it->second.app_id != app_id) {
        return std::unexpected("Message dialog request does not belong to caller");
    }
    MessageDialogButtonRole button_role = MessageDialogButtonRole::Invalid;
    if (reason == MessageDialogCloseReason::Button) {
        if (button_index < 0 || static_cast<size_t>(button_index) >= it->second.options.buttons.size()) {
            return std::unexpected("Message dialog button_index does not reference an active button");
        }
        button_role = it->second.options.buttons[button_index].role;
    }
    if (reason != MessageDialogCloseReason::Button) {
        button_index = -1;
    }

    const bool was_active = impl_->active_message_dialog_request_id_.has_value() &&
                            *impl_->active_message_dialog_request_id_ == request_id;
    if (was_active) {
        impl_->active_message_dialog_request_id_.reset();
        if (notify_shell) {
            const auto owner_app_id = it->second.app_id;
            if (impl_->should_defer_gui_task(SYSTEM_GUI_INPUT_TASK_GROUP)) {
                auto post_result = impl_->post_gui_input_task([this, owner_app_id, request_id]() {
                    on_hide_message_dialog(owner_app_id, request_id);
                });
                if (!post_result) {
                    BROOKESIA_LOGW(
                        "Failed to defer message dialog hide: request_id(%1%), error(%2%)",
                        request_id, post_result.error()
                    );
                }
            } else {
                on_hide_message_dialog(owner_app_id, request_id);
            }
        }
    } else {
        remove_queued_message_dialog(request_id);
    }

    auto record = std::move(it->second);
    impl_->message_dialog_requests_.erase(it);
    auto publish_result = publish_message_dialog_closed(
        MessageDialogResult{
            .request_id = record.request_id,
            .app_id = record.app_id,
            .button_index = button_index,
            .button_role = button_role,
            .reason = reason,
        },
        std::move(record.handler)
    );

    if (was_active) {
        auto show_result = show_next_message_dialog();
        if (!show_result) {
            if (!publish_result) {
                return std::unexpected(
                           with_cleanup_failure(
                               publish_result.error(),
                               "show next message dialog",
                               show_result.error()
                           )
                       );
            }
            return show_result;
        }
    }
    return publish_result;
}

std::expected<void, std::string> System::update_message_dialog(
    AppId app_id,
    MessageDialogRequestId request_id,
    MessageDialogOptions options,
    bool validate_owner
)
{
    if (options.buttons.size() > 3) {
        return std::unexpected("Message dialog supports at most 3 buttons");
    }
    auto it = impl_->message_dialog_requests_.find(request_id);
    if (it == impl_->message_dialog_requests_.end()) {
        return std::unexpected("Message dialog request not found");
    }
    if (validate_owner && it->second.app_id != app_id) {
        return std::unexpected("Message dialog request does not belong to caller");
    }

    it->second.options = std::move(options);
    const bool is_active = impl_->active_message_dialog_request_id_.has_value() &&
                           *impl_->active_message_dialog_request_id_ == request_id;
    if (!is_active) {
        return {};
    }

    if (impl_->should_defer_gui_task(SYSTEM_GUI_INPUT_TASK_GROUP)) {
        const auto owner_app_id = it->second.app_id;
        const auto options_copy = it->second.options;
        auto post_result = impl_->post_gui_input_task([this, owner_app_id, request_id, options_copy]() {
            auto result = on_update_message_dialog(owner_app_id, request_id, options_copy);
            if (!result) {
                BROOKESIA_LOGW(
                    "Deferred message dialog update failed: request_id(%1%), error(%2%)",
                    request_id, result.error()
                );
            }
        });
        if (!post_result) {
            return std::unexpected(post_result.error());
        }
        return {};
    }

    return on_update_message_dialog(it->second.app_id, request_id, it->second.options);
}

// Close every keyboard and dialog request owned by one app without discarding any failure.
std::expected<void, std::string> System::close_app_input_requests(AppId app_id)
{
    std::expected<void, std::string> close_result = {};
    std::vector<KeyboardRequestId> keyboard_request_ids;
    for (const auto &[request_id, keyboard] : impl_->keyboard_requests_) {
        if (keyboard.app_id == app_id) {
            keyboard_request_ids.push_back(request_id);
        }
    }

    for (const auto request_id : keyboard_request_ids) {
        auto hide_result = hide_app_keyboard(app_id, request_id);
        if (!hide_result) {
            BROOKESIA_LOGW(
                "Failed to close app keyboard request: app_id(%1%), request_id(%2%), error(%3%)",
                app_id,
                request_id,
                hide_result.error()
            );
            append_cleanup_failure(close_result, "keyboard request cleanup", hide_result.error());
        }
    }

    auto dialog_result = close_message_dialogs_for_app(app_id);
    if (!dialog_result) {
        BROOKESIA_LOGW(
            "Failed to close app message dialogs: app_id(%1%), error(%2%)",
            app_id,
            dialog_result.error()
        );
        append_cleanup_failure(close_result, "message dialog cleanup", dialog_result.error());
    }
    return close_result;
}

// Cancel every active or queued dialog owned by one app and retain all dispatch failures.
std::expected<void, std::string> System::close_message_dialogs_for_app(AppId app_id)
{
    std::expected<void, std::string> close_result = {};
    std::vector<MessageDialogRequestId> request_ids;
    for (const auto &[request_id, request] : impl_->message_dialog_requests_) {
        if (request.app_id == app_id) {
            request_ids.push_back(request_id);
        }
    }

    bool closed_active = false;
    for (const auto request_id : request_ids) {
        auto it = impl_->message_dialog_requests_.find(request_id);
        if (it == impl_->message_dialog_requests_.end()) {
            continue;
        }

        const bool was_active = impl_->active_message_dialog_request_id_.has_value() &&
                                *impl_->active_message_dialog_request_id_ == request_id;
        if (was_active) {
            impl_->active_message_dialog_request_id_.reset();
            on_hide_message_dialog(it->second.app_id, request_id);
            closed_active = true;
        } else {
            remove_queued_message_dialog(request_id);
        }

        auto record = std::move(it->second);
        impl_->message_dialog_requests_.erase(it);
        auto publish_result = publish_message_dialog_closed(
            MessageDialogResult{
                .request_id = record.request_id,
                .app_id = record.app_id,
                .button_index = -1,
                .button_role = MessageDialogButtonRole::Invalid,
                .reason = MessageDialogCloseReason::Closed,
            },
            std::move(record.handler)
        );
        if (!publish_result) {
            BROOKESIA_LOGW(
                "Failed to dispatch app message dialog cancellation: app_id(%1%), request_id(%2%), error(%3%)",
                app_id,
                request_id,
                publish_result.error()
            );
            append_cleanup_failure(
                close_result,
                "message dialog completion dispatch",
                publish_result.error()
            );
        }
    }

    if (closed_active) {
        auto show_result = show_next_message_dialog();
        if (!show_result) {
            BROOKESIA_LOGW("Failed to show next message dialog after app cleanup: %1%", show_result.error());
            append_cleanup_failure(close_result, "show next message dialog", show_result.error());
        }
    }
    return close_result;
}

std::vector<AppInfo> System::list_apps() const
{
    std::vector<AppInfo> apps;
    apps.reserve(impl_->apps_.size());
    for (const auto &[_, record] : impl_->apps_) {
        apps.push_back(record.info);
    }
    return apps;
}

std::optional<AppInfo> System::get_app(AppId app_id) const
{
    auto result = impl_->get_record(app_id);
    if (!result) {
        return std::nullopt;
    }
    return result.value()->info;
}

std::optional<AppInfo> System::get_active_app() const
{
    if (impl_->active_app_id_ == INVALID_APP_ID) {
        return std::nullopt;
    }
    return get_app(impl_->active_app_id_);
}

void System::set_system_type(std::string type)
{
    impl_->system_type_ = type.empty() ? BROOKESIA_SYSTEM_CORE_DEFAULT_SYSTEM_TYPE : std::move(type);
}

std::string System::get_system_type() const
{
    return impl_->system_type_;
}

SystemInfo System::get_system_info() const
{
    return on_get_system_info();
}

gui::Environment System::get_environment() const
{
    return impl_->environment_;
}

boost::json::object System::get_environment_json() const
{
    boost::json::object environment;
    environment["width_px"] = impl_->environment_.width_px;
    environment["height_px"] = impl_->environment_.height_px;
    environment["density"] = impl_->environment_.density;
    environment["font_scale"] = impl_->environment_.font_scale;
    environment["language"] = impl_->environment_.language;
    environment["theme_id"] = impl_->environment_.theme_id;
    return environment;
}

} // namespace esp_brookesia::system::core
