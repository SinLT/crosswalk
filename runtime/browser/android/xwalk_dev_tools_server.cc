// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/browser/android/xwalk_dev_tools_server.h"

#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "base/android/jni_string.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
//#include "blink_upstream_version.h"  // NOLINT
#include "content/browser/devtools/devtools_http_handler.h"
//#include "components/devtools_http_handler/devtools_http_handler_delegate.h"
#include "content/public/browser/android/devtools_auth.h"
#include "content/public/browser/devtools_socket_factory.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_manager_delegate.h"
#include "content/public/browser/favicon_status.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "grit/xwalk_resources.h"
#include "jni/XWalkDevToolsServer_jni.h"
#include "net/base/net_errors.h"
#include "ui/base/resource/resource_bundle.h"
#include "xwalk/runtime/common/xwalk_content_client.h"

using content::DevToolsAgentHost;
using content::RenderViewHost;
using content::WebContents;
using base::android::JavaParamRef;

namespace {

// FIXME(girish): The frontend URL needs to be served from the domain below
// for remote debugging to work in chrome (see chrome's devtools_ui.cc).
// Currently, the chrome version is hardcoded because of this dependancy.
const char kFrontEndURL[] =
    "http://chrome-devtools-frontend.appspot.com/serve_rev/%s/inspector.html";
const int kBackLog = 10;

bool AuthorizeSocketAccessWithDebugPermission(const net::UnixDomainServerSocket::Credentials& credentials) {
  JNIEnv* env = base::android::AttachCurrentThread();
  return xwalk::Java_XWalkDevToolsServer_checkDebugPermission(env, credentials.process_id, credentials.user_id)
      || content::CanUserConnectToDevTools(credentials);
}

// Delegate implementation for the devtools http handler on android. A new
// instance of this gets created each time devtools is enabled.
class XWalkAndroidDevToolsHttpHandlerDelegate
  : public content::DevToolsManagerDelegate {
 public:
  XWalkAndroidDevToolsHttpHandlerDelegate() {
  }

  std::string GetDiscoveryPageHTML() override {
    return ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
        IDR_DEVTOOLS_FRONTEND_PAGE_HTML).as_string();
  }

  std::string GetFrontendResource(const std::string& path) override {
    return std::string();
  }

/* TODO(iotto) check replacement
  std::string GetPageThumbnailData(const GURL& url) override {
    return std::string();
  }
  content::DevToolsExternalAgentProxyDelegate*
      HandleWebSocketConnection(const std::string& path) override {
    return nullptr;
  }
*/

 private:
  DISALLOW_COPY_AND_ASSIGN(XWalkAndroidDevToolsHttpHandlerDelegate);
};

// Factory for UnixDomainServerSocket.
class UnixDomainServerSocketFactory
    : public content::DevToolsSocketFactory {
 public:
  explicit UnixDomainServerSocketFactory(
      const std::string& socket_name,
      const net::UnixDomainServerSocket::AuthCallback& auth_callback)
      : socket_name_(socket_name),
        auth_callback_(auth_callback) {}

 private:
  // content::DevToolsHttpHandler::ServerSocketFactory.
  std::unique_ptr<net::ServerSocket> CreateForHttpServer() override {
    std::unique_ptr<net::UnixDomainServerSocket> socket(
        new net::UnixDomainServerSocket(
            auth_callback_,
            true /* use_abstract_namespace */));
    if (socket->BindAndListen(socket_name_, kBackLog) != net::OK)
      return std::unique_ptr<net::ServerSocket>();

    return std::move(socket);
  }

  // Creates a named socket for reversed tethering implementation (used with
  // remote debugging, primarily for mobile).
  std::unique_ptr<net::ServerSocket> CreateForTethering(std::string* out_name) override {
    return std::unique_ptr<net::ServerSocket>();
  }

  const std::string socket_name_;
  const net::UnixDomainServerSocket::AuthCallback auth_callback_;
  DISALLOW_COPY_AND_ASSIGN(UnixDomainServerSocketFactory);
};

}  // namespace

namespace xwalk {

XWalkDevToolsServer::XWalkDevToolsServer(const std::string& socket_name)
    : socket_name_(socket_name),
      allow_debug_permission_(false),
      allow_socket_access_(false) {
}

XWalkDevToolsServer::~XWalkDevToolsServer() {
  Stop();
}

// Allow connection from uid specified using AllowConnectionFromUid to devtools
// server. This supports the XDK usage: debug bridge wrapper runs in a separate
// process and connects to the devtools server.
bool XWalkDevToolsServer::CanUserConnectToDevTools(
    const net::UnixDomainServerSocket::Credentials& credentials) {
  if (allow_socket_access_)
    return true;
  if (allow_debug_permission_)
    return AuthorizeSocketAccessWithDebugPermission(credentials);
  return content::CanUserConnectToDevTools(credentials);
}

void XWalkDevToolsServer::Start(bool allow_debug_permission,
                                bool allow_socket_access) {

  /*
void AwDevToolsServer::Start() {
  if (is_started_)
    return;
  is_started_ = true;

  std::unique_ptr<content::DevToolsSocketFactory> factory(
      new UnixDomainServerSocketFactory(
          base::StringPrintf(kSocketNameFormat, getpid())));
  DevToolsAgentHost::StartRemoteDebuggingServer(
      std::move(factory),
      base::StringPrintf(kFrontEndURL, content::GetWebKitRevision().c_str()),
      base::FilePath(), base::FilePath());
}
   */
  allow_debug_permission_ = allow_debug_permission;
  allow_socket_access_ = allow_socket_access;
  if (devtools_http_handler_)
    return;
//
//  net::UnixDomainServerSocket::AuthCallback auth_callback =
//      base::Bind(&XWalkDevToolsServer::CanUserConnectToDevTools,
//                 base::Unretained(this));
//
//  std::unique_ptr<content::DevToolsSocketFactory>
//      factory(new UnixDomainServerSocketFactory(socket_name_, auth_callback));
//
//  devtools_http_handler_.reset(new content::DevToolsHttpHandler(
//      new XWalkAndroidDevToolsHttpHandlerDelegate(),
//      std::move(factory),
//      base::StringPrintf(kFrontEndURL, BLINK_UPSTREAM_REVISION),
//      base::FilePath(),
//      base::FilePath(),
//      std::string(),
//      xwalk::GetUserAgent()));
}

void XWalkDevToolsServer::Stop() {
  devtools_http_handler_.reset();
  allow_socket_access_ = false;
  allow_debug_permission_ = false;
}

bool XWalkDevToolsServer::IsStarted() const {
  return !!devtools_http_handler_;
}

bool RegisterXWalkDevToolsServer(JNIEnv* env) {
//  return RegisterNativesImpl(env);
  return false;
}

static jlong JNI_XWalkDevToolsServer_InitRemoteDebugging(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                         const JavaParamRef<jstring>& socketName) {
  XWalkDevToolsServer* server = new XWalkDevToolsServer(base::android::ConvertJavaStringToUTF8(env, socketName));
  return reinterpret_cast<intptr_t>(server);
}

static void JNI_XWalkDevToolsServer_DestroyRemoteDebugging(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                           jlong server) {
  delete reinterpret_cast<XWalkDevToolsServer*>(server);
}

static jboolean JNI_XWalkDevToolsServer_IsRemoteDebuggingEnabled(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                                 jlong server) {
  return reinterpret_cast<XWalkDevToolsServer*>(server)->IsStarted();
}

static void JNI_XWalkDevToolsServer_SetRemoteDebuggingEnabled(JNIEnv* env, const JavaParamRef<jobject>& obj,
                                                              jlong server, jboolean enabled,
                                                              jboolean allow_debug_permission,
                                                              jboolean allow_socket_access) {
  XWalkDevToolsServer* devtools_server = reinterpret_cast<XWalkDevToolsServer*>(server);
  if (enabled == JNI_TRUE) {
    devtools_server->Start(allow_debug_permission == JNI_TRUE, allow_socket_access == JNI_TRUE);
  } else {
    devtools_server->Stop();
  }
}

}  // namespace xwalk
