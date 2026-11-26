// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/browser/ui/webui/ko_workspace/ko_workspace_ui.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "brave/browser/ko/telemetry/ko_telemetry.h"
#include "brave/browser/ui/sidebar/sidebar_controller.h"
#include "brave/browser/ui/sidebar/sidebar_utils.h"
#include "brave/browser/ui/sidebar/sidebar_web_panel_controller.h"
#include "brave/components/containers/buildflags/buildflags.h"
#include "brave/components/ko/common/agentd_config.h"
#include "brave/components/ko/telemetry/ko_analytics_event.h"
#include "brave/components/ko/workspace/browser/ko_workspace_handler.h"
#include "brave/components/ko/workspace/core/agentd_config.h"
#include "brave/components/ko/workspace/resources/grit/ko_workspace_generated_map.h"
#include "brave/components/ko/workspace/resources/grit/ko_workspace_static_resources.h"
#include "brave/components/ko/workspace/resources/grit/ko_workspace_static_resources_map.h"
#include "brave/components/sidebar/browser/sidebar_item.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "components/grit/brave_components_strings.h"
#include "components/grit/brave_components_webui_strings.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/mhtml_generation_params.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/webui/webui_util.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_CONTAINERS)
#include "brave/browser/containers/containers_service_factory.h"
#include "brave/browser/ui/browser_commands.h"
#include "brave/components/containers/core/browser/containers_service.h"
#include "brave/components/containers/core/common/features.h"
#include "brave/components/containers/core/mojom/containers.mojom.h"
#endif

namespace {

// Stays under agentd's 32 MiB artifact limit once base64-encoded.
constexpr size_t kMaxCaptureBytes = 20 * 1024 * 1024;
constexpr char kAgentdScriptPath[] = "agentd.js";

// Port used for the page's CSP. Updated whenever agentd.js is served; the page
// reloads once if agentd moved to another port.
uint16_t g_last_agentd_port = ko::kAgentdDefaultPort;

std::string SanitizeForFileName(std::string_view host) {
  std::string out;
  for (char c : host) {
    out.push_back(base::IsAsciiAlphaNumeric(c) || c == '.' || c == '-' ? c
                                                                       : '_');
  }
  return out.empty() ? std::string("page") : out;
}

ko_workspace::mojom::CapturePtr MakeCapture(std::string file_name,
                                            std::string mime_type,
                                            std::vector<uint8_t> data,
                                            const GURL& source_url) {
  if (data.empty() || data.size() > kMaxCaptureBytes) {
    return nullptr;
  }
  return ko_workspace::mojom::Capture::New(
      std::move(file_name), std::move(mime_type), std::move(data), source_url);
}

std::optional<std::vector<uint8_t>> ReadAndDeleteFile(
    const base::FilePath& path) {
  std::optional<std::vector<uint8_t>> data;
  const std::optional<int64_t> size = base::GetFileSize(path);
  if (size && *size > 0 && static_cast<size_t>(*size) <= kMaxCaptureBytes) {
    data = base::ReadFileToBytes(path);
  }
  // The snapshot holds page content; never leave it behind.
  base::DeleteFile(path);
  return data;
}

std::optional<base::FilePath> CreateTempFile() {
  base::FilePath path;
  if (!base::CreateTemporaryFile(&path)) {
    return std::nullopt;
  }
  return path;
}

class WorkspaceDelegate : public ko_workspace::WorkspaceHandler::Delegate {
 public:
  explicit WorkspaceDelegate(content::WebContents* web_contents)
      : web_contents_(web_contents),
        profile_(
            Profile::FromBrowserContext(web_contents->GetBrowserContext())) {}
  ~WorkspaceDelegate() override = default;

  std::vector<ko_workspace::mojom::WorkspaceContainerPtr> GetContainers()
      override {
    std::vector<ko_workspace::mojom::WorkspaceContainerPtr> result;
#if BUILDFLAG(ENABLE_CONTAINERS)
    if (auto* service = GetContainersService()) {
      for (const auto& container : service->GetContainers()) {
        result.push_back(ko_workspace::mojom::WorkspaceContainer::New(
            container->id, container->name));
      }
    }
#endif
    return result;
  }

  ko_workspace::mojom::WorkspaceContainerPtr CreateContainer(
      const std::string& name) override {
#if BUILDFLAG(ENABLE_CONTAINERS)
    if (auto* service = GetContainersService()) {
      if (auto container =
              service->CreateContainer(name, containers::mojom::Icon::kWork,
                                       SkColorSetRGB(0x63, 0x5B, 0xFF))) {
        return ko_workspace::mojom::WorkspaceContainer::New(container->id,
                                                            container->name);
      }
    }
#endif
    return nullptr;
  }

  bool ContainerExists(const std::string& id) override {
#if BUILDFLAG(ENABLE_CONTAINERS)
    auto* service = GetContainersService();
    return service && !!service->GetRuntimeContainerById(id);
#else
    return false;
#endif
  }

  void OpenUrlInContainer(const std::string& container_id,
                          const GURL& url) override {
#if BUILDFLAG(ENABLE_CONTAINERS)
    auto* service = GetContainersService();
    auto* browser = GetBrowser();
    if (!service || !browser) {
      return;
    }
    if (auto container = service->GetRuntimeContainerById(container_id)) {
      brave::OpenUrlInContainer(browser, url, container, /*is_link=*/false);
    }
#endif
  }

  void OpenWorkspaceApp(const GURL& url, bool in_side_panel) override {
    OpenUrl(url, base::UTF8ToUTF16(url.host()), in_side_panel);
  }

  void ShowWorkspace(bool in_side_panel) override {
    OpenUrl(GURL(kKoWorkspaceURL),
            l10n_util::GetStringUTF16(IDS_KO_WORKSPACE_TITLE), in_side_panel);
  }

  void CaptureActiveTab(ko_workspace::mojom::CaptureKind kind,
                        ko_workspace::WorkspaceHandler::CaptureActiveTabCallback
                            callback) override {
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        std::move(callback), ko_workspace::mojom::CapturePtr());
    auto* browser = GetBrowser();
    content::WebContents* target =
        browser && browser->GetTabStripModel()
            ? browser->GetTabStripModel()->GetActiveWebContents()
            : nullptr;
    // Never capture this page itself or non-web pages.
    if (!target || target == web_contents_.get() ||
        !target->GetLastCommittedURL().SchemeIsHTTPOrHTTPS()) {
      std::move(reply).Run(nullptr);
      return;
    }
    const GURL url = target->GetLastCommittedURL();
    const std::string base_name = SanitizeForFileName(url.host());

    if (kind == ko_workspace::mojom::CaptureKind::kScreenshot) {
      auto* view = target->GetRenderWidgetHostView();
      if (!view) {
        std::move(reply).Run(nullptr);
        return;
      }
      view->CopyFromSurface(
          gfx::Rect(), gfx::Size(), base::Seconds(10),
          base::BindOnce(&WorkspaceDelegate::OnScreenshot, url,
                         base::StrCat({"screenshot-", base_name, ".png"}),
                         std::move(reply)));
      return;
    }

    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&CreateTempFile),
        base::BindOnce(&WorkspaceDelegate::OnTempFileCreated,
                       target->GetWeakPtr(), url,
                       base::StrCat({base_name, ".mhtml"}), std::move(reply)));
  }

 private:
#if BUILDFLAG(ENABLE_CONTAINERS)
  containers::ContainersService* GetContainersService() {
    if (!base::FeatureList::IsEnabled(containers::features::kContainers)) {
      return nullptr;
    }
    return ContainersServiceFactory::GetForProfile(profile_);
  }
#endif

  BrowserWindowInterface* GetBrowser() {
    return webui::GetBrowserWindowInterface(web_contents_.get());
  }

  void OpenUrl(const GURL& url, const std::u16string& title, bool in_panel) {
    auto* browser = GetBrowser();
    if (!browser) {
      return;
    }
    if (in_panel && sidebar::IsWebPanelFeatureEnabled()) {
      if (auto* controller = sidebar::SidebarController::From(browser)) {
        if (auto* panel = controller->GetWebPanelController()) {
          panel->ToggleWebPanel(
              sidebar::SidebarItem::CreateWebPanelItem(url, title));
          return;
        }
      }
    }
    NavigateParams params(browser, url, ui::PAGE_TRANSITION_AUTO_BOOKMARK);
    params.disposition = WindowOpenDisposition::kNewForegroundTab;
    Navigate(&params);
  }

  static void OnScreenshot(
      const GURL& url,
      std::string file_name,
      ko_workspace::WorkspaceHandler::CaptureActiveTabCallback reply,
      const content::CopyFromSurfaceResult& result) {
    if (!result.has_value() || result->bitmap.drawsNothing()) {
      std::move(reply).Run(nullptr);
      return;
    }
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](const SkBitmap& bitmap) {
              return gfx::PNGCodec::EncodeBGRASkBitmap(
                  bitmap, /*discard_transparency=*/true);
            },
            result->bitmap),
        base::BindOnce(
            [](const GURL& url, std::string file_name,
               ko_workspace::WorkspaceHandler::CaptureActiveTabCallback reply,
               std::optional<std::vector<uint8_t>> png) {
              std::move(reply).Run(png ? MakeCapture(std::move(file_name),
                                                     "image/png",
                                                     std::move(*png), url)
                                       : nullptr);
            },
            url, std::move(file_name), std::move(reply)));
  }

  static void OnTempFileCreated(
      base::WeakPtr<content::WebContents> target,
      const GURL& url,
      std::string file_name,
      ko_workspace::WorkspaceHandler::CaptureActiveTabCallback reply,
      std::optional<base::FilePath> path) {
    if (!path) {
      std::move(reply).Run(nullptr);
      return;
    }
    if (!target) {
      base::ThreadPool::PostTask(FROM_HERE, {base::MayBlock()},
                                 base::GetDeleteFileCallback(*path));
      std::move(reply).Run(nullptr);
      return;
    }
    const base::FilePath file_path = *path;
    target->GenerateMHTML(
        content::MHTMLGenerationParams(file_path),
        base::BindOnce(
            [](const base::FilePath& path, const GURL& url,
               std::string file_name,
               ko_workspace::WorkspaceHandler::CaptureActiveTabCallback reply,
               int64_t file_size) {
              base::ThreadPool::PostTaskAndReplyWithResult(
                  FROM_HERE,
                  {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
                  base::BindOnce(&ReadAndDeleteFile, path),
                  base::BindOnce(
                      [](const GURL& url, std::string file_name,
                         ko_workspace::WorkspaceHandler::
                             CaptureActiveTabCallback reply,
                         int64_t file_size,
                         std::optional<std::vector<uint8_t>> data) {
                        std::move(reply).Run(
                            file_size > 0 && data
                                ? MakeCapture(std::move(file_name),
                                              "multipart/related",
                                              std::move(*data), url)
                                : nullptr);
                      },
                      url, std::move(file_name), std::move(reply), file_size));
            },
            file_path, url, std::move(file_name), std::move(reply)));
  }

  raw_ptr<content::WebContents> web_contents_;
  raw_ptr<Profile> profile_;
};

void HandleAgentdScript(const std::string& path,
                        content::WebUIDataSource::GotDataCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&ko::ReadAgentdConfig, ko_workspace::GetKoDataDir()),
      base::BindOnce(
          [](content::WebUIDataSource::GotDataCallback callback,
             ko::AgentdConfig config) {
            g_last_agentd_port = config.port;
            std::move(callback).Run(
                base::MakeRefCounted<base::RefCountedString>(
                    ko_workspace::BuildAgentdScript(config)));
          },
          std::move(callback)));
}

void CreateAndAddDataSource(Profile* profile) {
  content::WebUIDataSource* source =
      content::WebUIDataSource::CreateAndAdd(profile, kKoWorkspaceHost);
  webui::SetupWebUIDataSource(source, kKoWorkspaceGenerated,
                              IDR_KO_WORKSPACE_STATIC_KO_WORKSPACE_HTML);
  source->AddResourcePaths(kKoWorkspaceStaticResources);
  source->AddLocalizedStrings(webui::kKoWorkspaceStrings);

  // Filled in by agentd.js, read from disk on every page load. The token is
  // never stored in prefs.
  source->AddString("agentdBaseUrl", "");
  source->AddString("agentdToken", "");
  source->AddString("agentdTokenStatus", "missing");
  source->AddInteger("agentdCspPort", g_last_agentd_port);
  source->SetRequestFilter(base::BindRepeating([](const std::string& path) {
                             return path == kAgentdScriptPath;
                           }),
                           base::BindRepeating(&HandleAgentdScript));

  // The only network peer is ko-agentd on loopback. Leo icons load from
  // chrome://resources.
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      base::StrCat({"connect-src chrome://resources 'self' http://127.0.0.1:",
                    base::NumberToString(g_last_agentd_port), ";"}));
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ImgSrc,
      "img-src chrome://resources chrome://theme 'self' blob: data:;");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FrameSrc, "frame-src 'none';");
}

}  // namespace

KoWorkspaceUIConfig::KoWorkspaceUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, kKoWorkspaceHost) {}

bool KoWorkspaceUIConfig::IsWebUIEnabled(
    content::BrowserContext* browser_context) {
  // Workspaces and their containers belong to a regular profile.
  return !browser_context->IsOffTheRecord();
}

KoWorkspaceUI::KoWorkspaceUI(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  CreateAndAddDataSource(Profile::FromWebUI(web_ui));
  ko::RecordFeatureUsed(ko::analytics::Feature::kWorkspace);
}

KoWorkspaceUI::~KoWorkspaceUI() = default;

void KoWorkspaceUI::BindInterface(
    mojo::PendingReceiver<ko_workspace::mojom::PageHandler> receiver) {
  Profile* profile = Profile::FromWebUI(web_ui());
  handler_ = std::make_unique<ko_workspace::WorkspaceHandler>(
      std::move(receiver), *profile->GetPrefs(), ko_workspace::GetKoDataDir(),
      std::make_unique<WorkspaceDelegate>(web_ui()->GetWebContents()));
}

WEB_UI_CONTROLLER_TYPE_IMPL(KoWorkspaceUI)
