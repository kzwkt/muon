// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/common_web_contents_delegate.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "atom/browser/atom_browser_context.h"
#include "atom/browser/browser.h"
#include "atom/browser/native_window.h"
#include "atom/browser/web_contents_permission_helper.h"
#include "atom/common/atom_constants.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task_scheduler/post_task.h"
#include "brave/browser/brave_javascript_dialog_manager.h"
#include "chrome/browser/certificate_viewer.h"
#include "chrome/browser/extensions/api/file_system/file_entry_picker.h"
#include "chrome/browser/file_select_helper.h"
#include "chrome/browser/ssl/security_state_tab_helper.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_dialogs.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/pref_names.h"
#include "chrome/grit/generated_resources.h"
#include "components/pref_registry/pref_registry_syncable.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/security_state/content/content_utils.h"
#include "components/security_state/core/security_state.h"
#include "components/sessions/core/session_id.h"
#include "components/user_prefs/user_prefs.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/security_style_explanation.h"
#include "content/public/browser/security_style_explanations.h"
#include "extensions/buildflags/buildflags.h"
#include "net/ssl/ssl_cipher_suite_names.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "storage/browser/fileapi/isolated_context.h"
#include "ui/base/l10n/l10n_util.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "atom/browser/api/atom_api_window.h"
#include "atom/browser/extensions/tab_helper.h"
#include "chrome/browser/chrome_notification_types.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"
#include "extensions/browser/api/extensions_api_client.h"
#endif

using content::BrowserThread;

namespace atom {

namespace {

const char kRootName[] = "<root>";

struct FileSystem {
  FileSystem() {
  }
  FileSystem(const std::string& type,
             const std::string& file_system_name,
             const std::string& root_url,
             const std::string& file_system_path)
    : type(type),
      file_system_name(file_system_name),
      root_url(root_url),
      file_system_path(file_system_path) {
  }

  std::string type;
  std::string file_system_name;
  std::string root_url;
  std::string file_system_path;
};

std::string RegisterFileSystem(content::WebContents* web_contents,
                               const base::FilePath& path) {
  auto isolated_context = storage::IsolatedContext::GetInstance();
  std::string root_name(kRootName);
  std::string file_system_id = isolated_context->RegisterFileSystemForPath(
      storage::kFileSystemTypeNativeLocal,
      std::string(),
      path,
      &root_name);

  content::ChildProcessSecurityPolicy* policy =
      content::ChildProcessSecurityPolicy::GetInstance();
  content::RenderViewHost* render_view_host = web_contents->GetRenderViewHost();
  int renderer_id = render_view_host->GetProcess()->GetID();
  policy->GrantReadFileSystem(renderer_id, file_system_id);
  policy->GrantWriteFileSystem(renderer_id, file_system_id);
  policy->GrantCreateFileForFileSystem(renderer_id, file_system_id);
  policy->GrantDeleteFromFileSystem(renderer_id, file_system_id);

  if (!policy->CanReadFile(renderer_id, path))
    policy->GrantReadFile(renderer_id, path);

  return file_system_id;
}

FileSystem CreateFileSystemStruct(
    content::WebContents* web_contents,
    const std::string& type,
    const std::string& file_system_id,
    const std::string& file_system_path) {
  const GURL origin = web_contents->GetURL().GetOrigin();
  std::string file_system_name =
      storage::GetIsolatedFileSystemName(origin, file_system_id);
  std::string root_url = storage::GetIsolatedFileSystemRootURIString(
      origin, file_system_id, kRootName);
  return FileSystem(type, file_system_name, root_url, file_system_path);
}

base::DictionaryValue* CreateFileSystemValue(const FileSystem& file_system) {
  auto* file_system_value = new base::DictionaryValue();
  file_system_value->SetString("type", file_system.type);
  file_system_value->SetString("fileSystemName", file_system.file_system_name);
  file_system_value->SetString("rootURL", file_system.root_url);
  file_system_value->SetString("fileSystemPath", file_system.file_system_path);
  return file_system_value;
}

void WriteToFile(const base::FilePath& path,
                 const std::string& content) {
  base::AssertBlockingAllowed();
  DCHECK(!path.empty());

  base::WriteFile(path, content.data(), content.size());
}

void AppendToFile(const base::FilePath& path,
                  const std::string& content) {
  base::AssertBlockingAllowed();
  DCHECK(!path.empty());

  base::AppendToFile(path, content.data(), content.size());
}

PrefService* GetPrefService(content::WebContents* web_contents) {
  auto context = web_contents->GetBrowserContext();
  return static_cast<atom::AtomBrowserContext*>(context)->prefs();
}

std::map<std::string, std::string> GetAddedFileSystemPaths(
    content::WebContents* web_contents) {
  auto pref_service = GetPrefService(web_contents);
  const base::DictionaryValue* file_system_paths_value =
      pref_service->GetDictionary(prefs::kDevToolsFileSystemPaths);
  std::map<std::string, std::string> result;
  if (file_system_paths_value) {
    base::DictionaryValue::Iterator it(*file_system_paths_value);
    for (; !it.IsAtEnd(); it.Advance()) {
      std::string type =
          it.value().is_string() ? it.value().GetString() : std::string();
      result[it.key()] = type;
    }
  }
  return result;
}

bool IsDevToolsFileSystemAdded(
    content::WebContents* web_contents,
    const std::string& file_system_path) {
  auto pref_service = GetPrefService(web_contents);
  const base::DictionaryValue* file_systems_paths_value =
      pref_service->GetDictionary(prefs::kDevToolsFileSystemPaths);
  return file_systems_paths_value->HasKey(file_system_path);
}

}  // namespace

CommonWebContentsDelegate::CommonWebContentsDelegate()
    : html_fullscreen_(false),
      native_fullscreen_(false),
      devtools_file_system_indexer_(new DevToolsFileSystemIndexer),
      weak_ptr_factory_(this),
      file_task_runner_(
          base::CreateSequencedTaskRunnerWithTraits({base::MayBlock()})) {}

CommonWebContentsDelegate::~CommonWebContentsDelegate() {
}

void CommonWebContentsDelegate::InitWithWebContents(
    content::WebContents* web_contents,
    AtomBrowserContext* browser_context) {
  browser_context_ = browser_context;
  web_contents->SetDelegate(this);

  // Create InspectableWebContents.
  web_contents_.reset(brightray::InspectableWebContents::Create(web_contents));
  web_contents_->SetDelegate(this);
}

void CommonWebContentsDelegate::SetOwnerWindow(NativeWindow* owner_window) {
  SetOwnerWindow(GetWebContents(), owner_window);
}

void CommonWebContentsDelegate::SetOwnerWindow(
    content::WebContents* web_contents, NativeWindow* owner_window) {
  owner_window_ = owner_window->GetWeakPtr();
  NativeWindowRelay* relay = new NativeWindowRelay(owner_window_);
  web_contents->SetUserData(relay->key, base::WrapUnique(relay));
#if BUILDFLAG(ENABLE_EXTENSIONS)
  auto tab_helper = extensions::TabHelper::FromWebContents(web_contents);
  if (!tab_helper)
    return;

  int32_t id =
      api::Window::TrackableObject::GetIDFromWrappedClass(owner_window);
  if (id > 0) {
    tab_helper->SetWindowId(id);
    tab_helper->SetBrowser(owner_window->browser());

    content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_TAB_PARENTED,
      content::Source<content::WebContents>(web_contents),
      content::NotificationService::NoDetails());
  }
#endif
}

void CommonWebContentsDelegate::DestroyWebContents() {
  web_contents_.reset();
}

content::WebContents* CommonWebContentsDelegate::GetWebContents() const {
  if (!web_contents_)
    return nullptr;
  return web_contents_->GetWebContents();
}

content::WebContents*
CommonWebContentsDelegate::GetDevToolsWebContents() const {
  if (!web_contents_)
    return nullptr;
  return web_contents_->GetDevToolsWebContents();
}

content::WebContents* CommonWebContentsDelegate::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params) {
  content::NavigationController::LoadURLParams load_url_params(params.url);
  load_url_params.source_site_instance = params.source_site_instance;
  load_url_params.referrer = params.referrer;
  load_url_params.frame_tree_node_id = params.frame_tree_node_id;
  load_url_params.redirect_chain = params.redirect_chain;
  load_url_params.transition_type = params.transition;
  load_url_params.extra_headers = params.extra_headers;
  load_url_params.should_replace_current_entry =
    params.should_replace_current_entry;
  load_url_params.is_renderer_initiated = params.is_renderer_initiated;

  if (params.uses_post) {
    load_url_params.load_type =
      content::NavigationController::LOAD_TYPE_HTTP_POST;
    load_url_params.post_data =
      params.post_data;
  }

  source->GetController().LoadURLWithParams(load_url_params);
  return source;
}

bool CommonWebContentsDelegate::CanOverscrollContent() const {
  return false;
}

content::JavaScriptDialogManager*
CommonWebContentsDelegate::GetJavaScriptDialogManager(
    content::WebContents* source) {
  return brave::BraveJavaScriptDialogManager::GetInstance();
}

content::ColorChooser* CommonWebContentsDelegate::OpenColorChooser(
    content::WebContents* web_contents,
    SkColor color,
    const std::vector<blink::mojom::ColorSuggestionPtr>& suggestions) {
  return chrome::ShowColorChooser(web_contents, color);
}

void CommonWebContentsDelegate::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    const content::FileChooserParams& params) {
  FileSelectHelper::RunFileChooser(render_frame_host, params);
}

void CommonWebContentsDelegate::EnumerateDirectory(content::WebContents* guest,
                                                   int request_id,
                                                   const base::FilePath& path) {
  FileSelectHelper::EnumerateDirectory(guest, request_id, path);
}

void CommonWebContentsDelegate::EnterFullscreenModeForTab(
    content::WebContents* source, const GURL& origin) {
  if (!owner_window_)
    return;
  SetHtmlApiFullscreen(true);
  owner_window_->NotifyWindowEnterHtmlFullScreen();
  source->GetRenderViewHost()->GetWidget()->WasResized();
}

void CommonWebContentsDelegate::ExitFullscreenModeForTab(
    content::WebContents* source) {
  if (!owner_window_)
    return;
  SetHtmlApiFullscreen(false);
  owner_window_->NotifyWindowLeaveHtmlFullScreen();
  if (source)
    source->GetRenderViewHost()->GetWidget()->WasResized();
}

bool CommonWebContentsDelegate::IsFullscreenForTabOrPending(
    const content::WebContents* source) const {
  return html_fullscreen_;
}

blink::WebSecurityStyle CommonWebContentsDelegate::GetSecurityStyle(
    content::WebContents* web_contents,
    content::SecurityStyleExplanations* security_style_explanations) {
  SecurityStateTabHelper* helper =
    SecurityStateTabHelper::FromWebContents(web_contents);
  DCHECK(helper);
  security_state::SecurityInfo security_info;
  helper->GetSecurityInfo(&security_info);
  return security_state::GetSecurityStyle(security_info,
                                          security_style_explanations);
}

void CommonWebContentsDelegate::DevToolsSaveToFile(
    const std::string& url, const std::string& content, bool save_as) {
  base::FilePath path;
  auto it = saved_files_.find(url);
  if (it != saved_files_.end() && !save_as) {
    path = it->second;
    saved_files_[url] = path;
    file_task_runner_->PostTaskAndReply(
        FROM_HERE, base::BindOnce(&WriteToFile, path, content),
        base::BindOnce(&CommonWebContentsDelegate::OnDevToolsSaveToFile,
                       base::Unretained(this), url));
  } else {
    base::FilePath default_path;
    PathService::Get(chrome::DIR_DEFAULT_DOWNLOADS, &default_path);
    default_path = default_path.Append(base::FilePath::FromUTF8Unsafe(url));
    new extensions::FileEntryPicker(
      GetWebContents(), default_path,
      ui::SelectFileDialog::FileTypeInfo(),
      ui::SelectFileDialog::SELECT_SAVEAS_FILE,
      base::Bind(&CommonWebContentsDelegate::OnSaveFileSelected,
                 weak_ptr_factory_.GetWeakPtr(), url, content),
      base::Bind(&CommonWebContentsDelegate::OnSaveFileSelectionCancelled,
                 weak_ptr_factory_.GetWeakPtr(), url));
  }
}

void CommonWebContentsDelegate::DevToolsAppendToFile(
    const std::string& url, const std::string& content) {
  auto it = saved_files_.find(url);
  if (it == saved_files_.end())
    return;

  file_task_runner_->PostTaskAndReply(
      FROM_HERE, base::BindOnce(&AppendToFile, it->second, content),
      base::BindOnce(&CommonWebContentsDelegate::OnDevToolsAppendToFile,
                     base::Unretained(this), url));
}

void CommonWebContentsDelegate::DevToolsRequestFileSystems() {
  auto file_system_paths = GetAddedFileSystemPaths(GetDevToolsWebContents());
  if (file_system_paths.empty()) {
    base::ListValue empty_file_system_value;
    web_contents_->CallClientFunction("DevToolsAPI.fileSystemsLoaded",
                                      &empty_file_system_value,
                                      nullptr, nullptr);
    return;
  }

  std::vector<FileSystem> file_systems;
  for (auto file_system_path : file_system_paths) {
    base::FilePath path =
      base::FilePath::FromUTF8Unsafe(file_system_path.first);
    std::string file_system_id = RegisterFileSystem(GetDevToolsWebContents(),
                                                    path);
    FileSystem file_system = CreateFileSystemStruct(GetDevToolsWebContents(),
                                                    file_system_path.second,
                                                    file_system_id,
                                                    file_system_path.first);
    file_systems.push_back(file_system);
  }

  base::ListValue file_system_value;
  for (const auto& file_system : file_systems)
    file_system_value.Append(std::unique_ptr<base::DictionaryValue>(
                                CreateFileSystemValue(file_system)));
  web_contents_->CallClientFunction("DevToolsAPI.fileSystemsLoaded",
                                    &file_system_value, nullptr, nullptr);
}

void CommonWebContentsDelegate::DevToolsAddFileSystem(
    const base::FilePath& file_system_path, const std::string& type) {
  if (file_system_path.empty()) {
    new extensions::FileEntryPicker(
      GetWebContents(), file_system_path,
      ui::SelectFileDialog::FileTypeInfo(),
      ui::SelectFileDialog::SELECT_FOLDER,
      base::Bind(&CommonWebContentsDelegate::OnAddFileSelected,
                 weak_ptr_factory_.GetWeakPtr(), type),
      base::Bind(&CommonWebContentsDelegate::OnAddFileSelectionCancelled,
                 weak_ptr_factory_.GetWeakPtr()));
  } else {
    DevToolsAddFileSystemInteral(file_system_path, type);
  }
}

void CommonWebContentsDelegate::DevToolsRemoveFileSystem(
    const base::FilePath& file_system_path) {
  if (!web_contents_)
    return;

  std::string path = file_system_path.AsUTF8Unsafe();
  storage::IsolatedContext::GetInstance()->
      RevokeFileSystemByPath(file_system_path);

  auto pref_service = GetPrefService(GetDevToolsWebContents());
  DictionaryPrefUpdate update(pref_service, prefs::kDevToolsFileSystemPaths);
  update.Get()->RemoveWithoutPathExpansion(path, nullptr);

  base::Value file_system_path_value(path);
  web_contents_->CallClientFunction("DevToolsAPI.fileSystemRemoved",
                                    &file_system_path_value,
                                    nullptr, nullptr);
}

void CommonWebContentsDelegate::DevToolsIndexPath(
    int request_id,
    const std::string& file_system_path) {
  if (!IsDevToolsFileSystemAdded(GetDevToolsWebContents(), file_system_path)) {
    OnDevToolsIndexingDone(request_id, file_system_path);
    return;
  }
  if (devtools_indexing_jobs_.count(request_id) != 0)
    return;
  devtools_indexing_jobs_[request_id] =
      scoped_refptr<DevToolsFileSystemIndexer::FileSystemIndexingJob>(
          devtools_file_system_indexer_->IndexPath(
              file_system_path,
              base::Bind(
                  &CommonWebContentsDelegate::OnDevToolsIndexingWorkCalculated,
                  base::Unretained(this),
                  request_id,
                  file_system_path),
              base::Bind(&CommonWebContentsDelegate::OnDevToolsIndexingWorked,
                         base::Unretained(this),
                         request_id,
                         file_system_path),
              base::Bind(&CommonWebContentsDelegate::OnDevToolsIndexingDone,
                         base::Unretained(this),
                         request_id,
                         file_system_path)));
}

void CommonWebContentsDelegate::DevToolsStopIndexing(int request_id) {
  auto it = devtools_indexing_jobs_.find(request_id);
  if (it == devtools_indexing_jobs_.end())
    return;
  it->second->Stop();
  devtools_indexing_jobs_.erase(it);
}

void CommonWebContentsDelegate::DevToolsSearchInPath(
    int request_id,
    const std::string& file_system_path,
    const std::string& query) {
  if (!IsDevToolsFileSystemAdded(GetDevToolsWebContents(), file_system_path)) {
    OnDevToolsSearchCompleted(request_id,
                              file_system_path,
                              std::vector<std::string>());
    return;
  }
  devtools_file_system_indexer_->SearchInPath(
      file_system_path,
      query,
      base::Bind(&CommonWebContentsDelegate::OnDevToolsSearchCompleted,
                 base::Unretained(this),
                 request_id,
                 file_system_path));
}

void CommonWebContentsDelegate::OnSaveFileSelected(
    const std::string& url,
    const std::string& content,
    const std::vector<base::FilePath>& paths) {
  DCHECK(!paths.empty());
  saved_files_[url] = paths[0];
  file_task_runner_->PostTaskAndReply(
      FROM_HERE, base::BindOnce(&WriteToFile, paths[0], content),
      base::BindOnce(&CommonWebContentsDelegate::OnDevToolsSaveToFile,
                     base::Unretained(this), url));
}
void CommonWebContentsDelegate::OnSaveFileSelectionCancelled(
    const std::string url) {
  base::Value url_value(url);
  web_contents_->CallClientFunction(
    "DevToolsAPI.canceledSaveURL", &url_value, nullptr, nullptr);
}

void CommonWebContentsDelegate::OnAddFileSelected(
    const std::string& type, const std::vector<base::FilePath>& paths) {
  DCHECK(!paths.empty());
  DevToolsAddFileSystemInteral(paths[0], type);
}

void CommonWebContentsDelegate::OnAddFileSelectionCancelled() {}

void CommonWebContentsDelegate::DevToolsAddFileSystemInteral(
    const base::FilePath& path, const std::string& type) {
  std::string file_system_id = RegisterFileSystem(GetDevToolsWebContents(),
                                                  path);
  if (IsDevToolsFileSystemAdded(GetDevToolsWebContents(), path.AsUTF8Unsafe()))
    return;

  FileSystem file_system = CreateFileSystemStruct(GetDevToolsWebContents(),
                                                  type,
                                                  file_system_id,
                                                  path.AsUTF8Unsafe());
  std::unique_ptr<base::DictionaryValue> file_system_value(
      CreateFileSystemValue(file_system));

  auto pref_service = GetPrefService(GetDevToolsWebContents());
  DictionaryPrefUpdate update(pref_service, prefs::kDevToolsFileSystemPaths);
  update.Get()->SetWithoutPathExpansion(
      path.AsUTF8Unsafe(), base::MakeUnique<base::Value>(type));

  web_contents_->CallClientFunction("DevToolsAPI.fileSystemAdded",
                                    file_system_value.get(),
                                    nullptr);
}

void CommonWebContentsDelegate::OnDevToolsSaveToFile(
    const std::string& url) {
  // Notify DevTools.
  base::Value url_value(url);
  web_contents_->CallClientFunction(
      "DevToolsAPI.savedURL", &url_value, nullptr, nullptr);
}

void CommonWebContentsDelegate::OnDevToolsAppendToFile(
    const std::string& url) {
  // Notify DevTools.
  base::Value url_value(url);
  web_contents_->CallClientFunction(
      "DevToolsAPI.appendedToURL", &url_value, nullptr, nullptr);
}

void CommonWebContentsDelegate::OnDevToolsIndexingWorkCalculated(
    int request_id,
    const std::string& file_system_path,
    int total_work) {
  base::Value request_id_value(request_id);
  base::Value file_system_path_value(file_system_path);
  base::Value total_work_value(total_work);
  web_contents_->CallClientFunction("DevToolsAPI.indexingTotalWorkCalculated",
                                    &request_id_value,
                                    &file_system_path_value,
                                    &total_work_value);
}

void CommonWebContentsDelegate::OnDevToolsIndexingWorked(
    int request_id,
    const std::string& file_system_path,
    int worked) {
  base::Value request_id_value(request_id);
  base::Value file_system_path_value(file_system_path);
  base::Value worked_value(worked);
  web_contents_->CallClientFunction("DevToolsAPI.indexingWorked",
                                    &request_id_value,
                                    &file_system_path_value,
                                    &worked_value);
}

void CommonWebContentsDelegate::OnDevToolsIndexingDone(
    int request_id,
    const std::string& file_system_path) {
  devtools_indexing_jobs_.erase(request_id);
  base::Value request_id_value(request_id);
  base::Value file_system_path_value(file_system_path);
  web_contents_->CallClientFunction("DevToolsAPI.indexingDone",
                                    &request_id_value,
                                    &file_system_path_value,
                                    nullptr);
}

void CommonWebContentsDelegate::OnDevToolsSearchCompleted(
    int request_id,
    const std::string& file_system_path,
    const std::vector<std::string>& file_paths) {
  base::ListValue file_paths_value;
  for (const auto& file_path : file_paths) {
    file_paths_value.AppendString(file_path);
  }
  base::Value request_id_value(request_id);
  base::Value file_system_path_value(file_system_path);
  web_contents_->CallClientFunction("DevToolsAPI.searchCompleted",
                                    &request_id_value,
                                    &file_system_path_value,
                                    &file_paths_value);
}

void CommonWebContentsDelegate::SetHtmlApiFullscreen(bool enter_fullscreen) {
  if (owner_window_) {
    // Window is already in fullscreen mode, save the state.
    if (enter_fullscreen && owner_window_->IsFullscreen()) {
      native_fullscreen_ = true;
      html_fullscreen_ = true;
      return;
    }

    // Exit html fullscreen state but not window's fullscreen mode.
    if (!enter_fullscreen && native_fullscreen_) {
      html_fullscreen_ = false;
      return;
    }

    owner_window_->SetFullScreen(enter_fullscreen);
  }
  html_fullscreen_ = enter_fullscreen;
  native_fullscreen_ = false;
}

}  // namespace atom
