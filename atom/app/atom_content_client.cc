// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/app/atom_content_client.h"

#include <string>
#include <vector>

#include "atom/common/atom_version.h"
#include "atom/common/options_switches.h"
#include "atom/common/pepper_flash_util.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/mac/bundle_locations.h"
#include "base/memory/ptr_util.h"
#include "base/path_service.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_version.h"
#include "chrome/common/crash_keys.h"
#include "chrome/common/secure_origin_whitelist.h"
#include "content/public/common/cdm_info.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/pepper_plugin_info.h"
#include "content/public/common/user_agent.h"
#include "extensions/buildflags/buildflags.h"
#include "gpu/config/gpu_crash_keys.h"
#include "gpu/config/gpu_info.h"
#include "gpu/config/gpu_util.h"
#include "media/base/video_codecs.h"
#include "media/media_buildflags.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "url/url_constants.h"
#include "widevine_cdm_version.h"  // NOLINT

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "content/public/common/url_constants.h"
#include "extensions/common/constants.h"
#include "extensions/common/features/feature_util.h"
#endif

#if defined(WIDEVINE_CDM_AVAILABLE) && BUILDFLAG(ENABLE_LIBRARY_CDMS) && \
    !defined(WIDEVINE_CDM_IS_COMPONENT)
#define WIDEVINE_CDM_AVAILABLE_NOT_COMPONENT
#include "chrome/common/widevine_cdm_constants.h"
#endif

#if BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
#include "chrome/common/media/cdm_host_file_path.h"
#endif

#if defined(WIDEVINE_CDM_AVAILABLE_NOT_COMPONENT)
bool IsWidevineAvailable(base::FilePath* adapter_path,
                         base::FilePath* cdm_path,
                         std::vector<media::VideoCodec>* codecs_supported,
                         bool* supports_persistent_license) {
  static enum {
    NOT_CHECKED,
    FOUND,
    NOT_FOUND,
  } widevine_cdm_file_check = NOT_CHECKED;
  // TODO(jrummell): We should add a new path for DIR_WIDEVINE_CDM and use that
  // to locate the CDM and the CDM adapter.
  if (PathService::Get(chrome::FILE_WIDEVINE_CDM_ADAPTER, adapter_path)) {
    *cdm_path = adapter_path->DirName().AppendASCII(
        base::GetNativeLibraryName(kWidevineCdmLibraryName));
    if (widevine_cdm_file_check == NOT_CHECKED) {
      widevine_cdm_file_check =
          (base::PathExists(*adapter_path) && base::PathExists(*cdm_path))
              ? FOUND
              : NOT_FOUND;
    }
    if (widevine_cdm_file_check == FOUND) {
      // Add the supported codecs as if they came from the component manifest.
      // This list must match the CDM that is being bundled with Chrome.
      codecs_supported->push_back(media::VideoCodec::kCodecVP8);
      codecs_supported->push_back(media::VideoCodec::kCodecVP9);
#if BUILDFLAG(USE_PROPRIETARY_CODECS)
      codecs_supported->push_back(media::VideoCodec::kCodecH264);
#endif  // BUILDFLAG(USE_PROPRIETARY_CODECS)

      *supports_persistent_license = false;

      return true;
    }
  }

  return false;
}
#endif  // defined(WIDEVINE_CDM_AVAILABLE_NOT_COMPONENT)

namespace atom {

namespace {
void ConvertStringWithSeparatorToVector(std::vector<std::string>* vec,
                                        const char* separator,
                                        const char* cmd_switch) {
  auto command_line = base::CommandLine::ForCurrentProcess();
  auto string_with_separator = command_line->GetSwitchValueASCII(cmd_switch);
  if (!string_with_separator.empty())
    *vec = base::SplitString(string_with_separator, separator,
                             base::TRIM_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY);
}

}  // namespace


AtomContentClient::AtomContentClient() {
}

AtomContentClient::~AtomContentClient() {
}

void AtomContentClient::SetActiveURL(const GURL& url, std::string top_origin) {
  static crash_reporter::CrashKeyString<1024> active_url("url-chunk");
  active_url.Set(url.possibly_invalid_spec());

  static crash_reporter::CrashKeyString<64> top_origin_key("top-origin");
  top_origin_key.Set(top_origin);
}

void AtomContentClient::SetGpuInfo(const gpu::GPUInfo& gpu_info) {
  gpu::SetKeysForCrashLogging(gpu_info);
}

std::string AtomContentClient::GetProduct() const {
  return "Chrome/" CHROME_VERSION_STRING;
}

std::string AtomContentClient::GetUserAgent() const {
  return content::BuildUserAgentFromProduct(
      "Chrome/" CHROME_VERSION_STRING);
}

void AtomContentClient::AddAdditionalSchemes(Schemes* schemes) {
  schemes->standard_schemes.push_back(extensions::kExtensionScheme);
  schemes->savable_schemes.push_back(extensions::kExtensionScheme);
  schemes->secure_schemes.push_back(extensions::kExtensionScheme);
  schemes->secure_origins = secure_origin_whitelist::GetWhitelist();

#if BUILDFLAG(ENABLE_EXTENSIONS)
  if (extensions::feature_util::ExtensionServiceWorkersEnabled())
    schemes->service_worker_schemes.push_back(extensions::kExtensionScheme);

  // As far as Blink is concerned, they should be allowed to receive CORS
  // requests. At the Extensions layer, requests will actually be blocked unless
  // overridden by the web_accessible_resources manifest key.
  // TODO(kalman): See what happens with a service worker.
  schemes->cors_enabled_schemes.push_back(extensions::kExtensionScheme);
#endif
}

bool AtomContentClient::AllowScriptExtensionForServiceWorker(
    const GURL& script_url) {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  return script_url.SchemeIs(extensions::kExtensionScheme);
#else
  return false;
#endif
}

content::OriginTrialPolicy* AtomContentClient::GetOriginTrialPolicy() {
  if (!origin_trial_policy_) {
    origin_trial_policy_ = base::WrapUnique(new ChromeOriginTrialPolicy());
  }
  return origin_trial_policy_.get();
}

void AtomContentClient::AddPepperPlugins(
    std::vector<content::PepperPluginInfo>* plugins) {
  AddPepperFlashFromCommandLine(plugins);
}

// TODO(xhwang): Move this to a common place if needed.
const base::FilePath::CharType kSignatureFileExtension[] =
    FILE_PATH_LITERAL(".sig");

// Returns the signature file path given the |file_path|. This function should
// only be used when the signature file and the file are located in the same
// directory.
base::FilePath GetSigFilePath(const base::FilePath& file_path) {
  return file_path.AddExtension(kSignatureFileExtension);
}

void AtomContentClient::AddContentDecryptionModules(
    std::vector<content::CdmInfo>* cdms,
    std::vector<media::CdmHostFilePath>* cdm_host_file_paths) {
  if (cdms) {
// TODO(jrummell): Need to have a better flag to indicate systems Widevine
// is available on. For now we continue to use ENABLE_LIBRARY_CDMS so that
// we can experiment between pepper and mojo.
#if defined(WIDEVINE_CDM_AVAILABLE_NOT_COMPONENT)
    base::FilePath adapter_path;
    base::FilePath cdm_path;
    std::vector<media::VideoCodec> video_codecs_supported;
    bool supports_persistent_license;
    if (IsWidevineAvailable(&adapter_path, &cdm_path, &video_codecs_supported,
                            &supports_persistent_license)) {
      // CdmInfo needs |path| to be the actual Widevine library,
      // not the adapter, so adjust as necessary. It will be in the
      // same directory as the installed adapter.
      const base::Version version(WIDEVINE_CDM_VERSION_STRING);
      DCHECK(version.IsValid());

      cdms->push_back(content::CdmInfo(
          kWidevineCdmDisplayName, kWidevineCdmGuid, version, cdm_path,
          kWidevineCdmFileSystemId, video_codecs_supported,
          supports_persistent_license, kWidevineKeySystem, false));
    }
#endif  // defined(WIDEVINE_CDM_AVAILABLE_NOT_COMPONENT)

    // TODO(jrummell): Add External Clear Key CDM for testing, if it's
    // available.
  }

#if BUILDFLAG(ENABLE_CDM_HOST_VERIFICATION)
  if (cdm_host_file_paths) {
#if defined(OS_WIN)
  base::FilePath brave_exe_dir;
  if (!PathService::Get(base::DIR_EXE, &brave_exe_dir))
    NOTREACHED();
  base::FilePath file_path;
  if (!PathService::Get(base::FILE_EXE, &file_path))
    NOTREACHED();
  cdm_host_file_paths->reserve(1);

  base::FilePath sig_path =
    GetSigFilePath(file_path);
  VLOG(1) << __func__ << ": unversioned file " << " at "
    << file_path.value() << ", signature file " << sig_path.value();
  cdm_host_file_paths->push_back(media::CdmHostFilePath(file_path, sig_path));
#elif defined(OS_MACOSX)
  chrome::AddCdmHostFilePaths(cdm_host_file_paths);
#endif
  }
#endif
}

}  // namespace atom
