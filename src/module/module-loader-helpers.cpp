#include "module-loader-helpers.h" 
#include "core-modules/file-system-helpers.h"

#include <napa-log.h>
#include <napa/v8-helpers.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/dll.hpp>
#include <boost/filesystem.hpp>
#include <boost/foreach.hpp>
#include <boost/property_tree/json_parser.hpp>

using namespace napa;
using namespace napa::module;

namespace {

    /// <summary> Set up __dirname and __filename at V8 context. </summary>
    /// <param name="exports"> Object to set module paths. </param>
    /// <param name="dirname"> Module directory name. </param>
    /// <param name="filename"> Module file name. </param>
    void SetupModulePath(v8::Local<v8::Object> exports, const std::string& dirname, const std::string& filename);

    /// <summary> Set up module objects at V8 context. </summary>
    /// <param name="parentContext"> Parent context. </param>
    /// <param name="exports"> Object to set module objects. </param>
    /// <param name="id"> Module id. </param>
    void SetupModuleObjects(v8::Local<v8::Context> parentContext, v8::Local<v8::Object> exports, const std::string& id);

}   // End of anonymous namespace.

v8::Local<v8::Object> module_loader_helpers::ExportModule(v8::Local<v8::Object> object,
                                                          const napa::module::ModuleInitializer& initializer) {
    auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);

    auto module = object->Get(v8_helpers::MakeV8String(isolate, "module"))->ToObject();
    auto exports = module->Get(v8_helpers::MakeV8String(isolate, "exports"))->ToObject();
    if (initializer != nullptr) {
        initializer(exports, module);
    }

    return scope.Escape(exports);
}

std::string module_loader_helpers::GetCurrentContextDirectory() {
    auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    auto context = isolate->GetCurrentContext();
    auto contextObject = context->Global();
    auto dirPropertyName = v8_helpers::MakeV8String(isolate, "__dirname");

    if (contextObject.IsEmpty() 
        || contextObject->IsNull()
        || !contextObject->Has(dirPropertyName)) {
        return GetCurrentWorkingDirectory();
    }

    v8::String::Utf8Value callingPath(contextObject->Get(dirPropertyName));
    return std::string(*callingPath);
}

std::string module_loader_helpers::GetModuleDirectory(v8::Local<v8::Object> module) {
    auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    auto filename = module->Get(v8_helpers::MakeV8String(isolate, "filename"));
    if (filename.IsEmpty() || !filename->IsString()) {
        return "";
    }

    boost::filesystem::path moduleFile(v8_helpers::V8ValueTo<std::string>(filename));
    return moduleFile.parent_path().string();
}

const std::string& module_loader_helpers::GetNapaDllPath() {
    static std::string dllPath = boost::dll::this_line_location().string();
    return dllPath;
}

const std::string& module_loader_helpers::GetNapaRuntimeDirectory() {
    static std::string runtimeDirectory = boost::dll::this_line_location().parent_path().string();
    return runtimeDirectory;
}

const std::string& module_loader_helpers::GetCurrentWorkingDirectory() {
    static std::string currentWorkingDirectory = boost::filesystem::current_path().string();
    return currentWorkingDirectory;
}

const std::string& module_loader_helpers::GetLibDirectory() {
    static std::string libDirectory = (boost::dll::this_line_location().parent_path().parent_path() / "lib").string();
    return libDirectory;
}

void module_loader_helpers::SetupTopLevelContext() {
    auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    auto exports = isolate->GetCurrentContext()->Global();

    SetupModulePath(exports, GetCurrentWorkingDirectory(), std::string());
    SetupModuleObjects(v8::Local<v8::Context>(), exports, ".");

    // Set 'global' variable shared by top-level context and module contexts.
    (void)exports->Set(v8_helpers::MakeV8String(isolate, "global"), exports);
}

void module_loader_helpers::SetupModuleContext(v8::Local<v8::Context> parentContext,
                                               v8::Local<v8::Context> moduleContext,
                                               const std::string& path) {
    auto isolate = v8::Isolate::GetCurrent();
    v8::HandleScope scope(isolate);

    auto exports = moduleContext->Global();

    SetupModuleObjects(parentContext, exports, path);
    if (path.empty()) {
        SetupModulePath(exports, GetCurrentWorkingDirectory(), std::string());
    } else {
        boost::filesystem::path current(path);
        SetupModulePath(exports, current.parent_path().string(), path);
    }

    auto global = parentContext->Global()->Get(parentContext, v8_helpers::MakeV8String(isolate, "global")).ToLocalChecked()->ToObject();
    (void)exports->Set(v8_helpers::MakeV8String(isolate, "global"), global);
}

std::vector<module_loader_helpers::CoreModuleInfo> module_loader_helpers::ReadCoreModulesJson() {
    static const std::string CORE_MODULES_JSON_PATH =
        (boost::filesystem::path(GetLibDirectory()) / "core\\core-modules.json").string();

    if (!boost::filesystem::exists(CORE_MODULES_JSON_PATH)) {
        return std::vector<CoreModuleInfo>();
    }

    std::vector<CoreModuleInfo> coreModuleInfos;

    // Reserve capacity to help avoiding too frequent allocation.
    coreModuleInfos.reserve(16);

    boost::property_tree::ptree modules;
    try {
        boost::property_tree::json_parser::read_json(CORE_MODULES_JSON_PATH, modules);

        for (const auto& value : modules) {
            const auto& module = value.second;

            coreModuleInfos.emplace_back(module.get<std::string>("name"),
                                         boost::iequals(module.get<std::string>("type"), "builtin"));
        }
    } catch (const std::exception& ex) {
        LOG_ERROR("ModuleLoader", ex.what());
        return std::vector<CoreModuleInfo>();
    }

    return coreModuleInfos;
}

v8::Local<v8::String> module_loader_helpers::ReadModuleFile(const std::string& path) {
    auto isolate = v8::Isolate::GetCurrent();
    v8::EscapableHandleScope scope(isolate);

    std::string content;
    try {
        content = file_system_helpers::ReadFileSync(path);
    } catch (const std::exception& ex) {
        isolate->ThrowException(v8::Exception::Error(v8_helpers::MakeV8String(isolate, ex.what())));
        return scope.Escape(v8::Local<v8::String>());
    }

    JS_ENSURE_WITH_RETURN(isolate,
                          !content.empty(),
                          scope.Escape(v8::Local<v8::String>()),
                          "\"%s\" is empty",
                          path.c_str());

    return scope.Escape(v8_helpers::MakeV8String(isolate, content));
}

namespace {

    void SetupModulePath(v8::Local<v8::Object> exports, const std::string& dirname, const std::string& filename) {
        auto isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);

        (void)exports->Set(v8_helpers::MakeV8String(isolate, "__dirname"), v8_helpers::MakeV8String(isolate, dirname));
        if (filename.empty()) {
            (void)exports->Set(v8_helpers::MakeV8String(isolate, "__filename"), v8::Null(isolate));
        } else {
            (void)exports->Set(v8_helpers::MakeV8String(isolate, "__filename"), v8_helpers::MakeV8String(isolate, filename));
        }
    }

    void SetupModuleObjects(v8::Local<v8::Context> parentContext, v8::Local<v8::Object> exports, const std::string& id) {
        auto isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);

        auto context = isolate->GetCurrentContext();

        auto module = v8::Object::New(isolate);
        (void)module->CreateDataProperty(context, v8_helpers::MakeV8String(isolate, "exports"), v8::Object::New(isolate));
        (void)module->CreateDataProperty(context, v8_helpers::MakeV8String(isolate, "paths"), v8::Array::New(isolate));
        (void)module->CreateDataProperty(context, v8_helpers::MakeV8String(isolate, "id"), v8_helpers::MakeV8String(isolate, id));
        (void)module->CreateDataProperty(context, v8_helpers::MakeV8String(isolate, "filename"), v8_helpers::MakeV8String(isolate, id));
        
        // Setup 'module.require'.
        if (!parentContext.IsEmpty()) {
            auto requireKey = v8_helpers::MakeV8String(isolate, "require");
            (void)module->CreateDataProperty(context, requireKey, parentContext->Global()->Get(requireKey));
        }

        (void)exports->CreateDataProperty(context, v8_helpers::MakeV8String(isolate, "module"), module);
        (void)exports->CreateDataProperty(context, v8_helpers::MakeV8String(isolate, "exports"),
                                          module->Get(v8_helpers::MakeV8String(isolate, "exports"))->ToObject());

        // Set '__in_napa' variable in all modules context to distinguish node and napa runtime.
        (void)exports->Set(v8_helpers::MakeV8String(isolate, "__in_napa"), v8::Boolean::New(isolate, true));
    }

}   // End of anonymous namespace.
