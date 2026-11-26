// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

// K-O has no Brave subscriptions: there is no SKU service and no
// window.chrome.braveSkus, even with the feature forced on.

#include <memory>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/test/scoped_feature_list.h"
#include "brave/browser/skus/skus_service_factory.h"
#include "brave/components/skus/common/features.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/chrome_test_utils.h"
#include "chrome/test/base/platform_browser_test.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_mock_cert_verifier.h"
#include "net/base/net_errors.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "services/network/public/cpp/network_switches.h"
#include "url/gurl.h"

namespace skus {

namespace {

std::unique_ptr<net::test_server::HttpResponse> HandleRequest(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_OK);
  response->set_content("<html><body>OK</body></html>");
  response->set_content_type("text/html");
  return response;
}

}  // namespace

using KoSkusBrowserTest = PlatformBrowserTest;

IN_PROC_BROWSER_TEST_F(KoSkusBrowserTest, OffByDefault) {
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kSkusFeature));
  EXPECT_FALSE(
      SkusServiceFactory::GetForContext(chrome_test_utils::GetProfile(this))
          .is_valid());
}

class KoSkusForcedOnBrowserTest : public PlatformBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    ASSERT_TRUE(https_server_.InitializeAndListen());
    // Serve account.brave.software, where the SKU API would be injected.
    command_line->AppendSwitchASCII(
        network::switches::kHostResolverRules,
        "MAP * " + https_server_.host_port_pair().ToString() +
            ",EXCLUDE localhost");
    PlatformBrowserTest::SetUpCommandLine(command_line);
    mock_cert_verifier_.SetUpCommandLine(command_line);
  }

  void SetUpInProcessBrowserTestFixture() override {
    PlatformBrowserTest::SetUpInProcessBrowserTestFixture();
    mock_cert_verifier_.SetUpInProcessBrowserTestFixture();
  }

  void TearDownInProcessBrowserTestFixture() override {
    mock_cert_verifier_.TearDownInProcessBrowserTestFixture();
    PlatformBrowserTest::TearDownInProcessBrowserTestFixture();
  }

  void SetUpOnMainThread() override {
    PlatformBrowserTest::SetUpOnMainThread();
    mock_cert_verifier_.mock_cert_verifier()->set_default_result(net::OK);
    https_server_.RegisterRequestHandler(base::BindRepeating(&HandleRequest));
    https_server_.StartAcceptingConnections();
  }

 private:
  net::EmbeddedTestServer https_server_{net::EmbeddedTestServer::TYPE_HTTPS};
  content::ContentMockCertVerifier mock_cert_verifier_;
  base::test::ScopedFeatureList scoped_feature_list_{features::kSkusFeature};
};

IN_PROC_BROWSER_TEST_F(KoSkusForcedOnBrowserTest, NoServiceAndNoJsApi) {
  EXPECT_FALSE(
      SkusServiceFactory::GetForContext(chrome_test_utils::GetProfile(this))
          .is_valid());

  content::WebContents* web_contents =
      chrome_test_utils::GetActiveWebContents(this);
  ASSERT_TRUE(content::NavigateToURL(web_contents,
                                     GURL("https://account.brave.software/")));
  EXPECT_EQ(false,
            content::EvalJs(web_contents,
                            "!!(window.chrome && window.chrome.braveSkus)"));
}

}  // namespace skus
