// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/guest_view/mime_handler_view/mime_handler_view_guest.h"

#include <utility>

#include "base/strings/stringprintf.h"
#include "components/guest_view/common/guest_view_constants.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/stream_handle.h"
#include "content/public/browser/stream_info.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/url_constants.h"
#include "extensions/browser/api/extensions_api_client.h"
#include "extensions/browser/api/mime_handler_private/mime_handler_private.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/guest_view/mime_handler_view/mime_handler_stream_manager.h"
#include "extensions/browser/guest_view/mime_handler_view/mime_handler_view_constants.h"
#include "extensions/browser/guest_view/mime_handler_view/mime_handler_view_guest_delegate.h"
#include "extensions/browser/process_manager.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension_messages.h"
#include "extensions/common/guest_view/extensions_guest_view_messages.h"
#include "extensions/strings/grit/extensions_strings.h"
#include "ipc/ipc_message_macros.h"
#include "net/base/url_util.h"
#include "services/service_manager/public/cpp/interface_registry.h"
#include "third_party/WebKit/public/platform/WebGestureEvent.h"

using content::WebContents;
using guest_view::GuestViewBase;

namespace extensions {

StreamContainer::StreamContainer(std::unique_ptr<content::StreamInfo> stream,
                                 int tab_id,
                                 bool embedded,
                                 const GURL& handler_url,
                                 const std::string& extension_id)
    : stream_(std::move(stream)),
      embedded_(embedded),
      tab_id_(tab_id),
      handler_url_(handler_url),
      extension_id_(extension_id),
      weak_factory_(this) {
  DCHECK(stream_);
}

StreamContainer::~StreamContainer() {
}

void StreamContainer::Abort(const base::Closure& callback) {
  if (!stream_->handle) {
    callback.Run();
    return;
  }
  stream_->handle->AddCloseListener(callback);
  stream_->handle.reset();
}

base::WeakPtr<StreamContainer> StreamContainer::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

// static
const char MimeHandlerViewGuest::Type[] = "mimehandler";

// static
GuestViewBase* MimeHandlerViewGuest::Create(WebContents* owner_web_contents) {
  return new MimeHandlerViewGuest(owner_web_contents);
}

MimeHandlerViewGuest::MimeHandlerViewGuest(WebContents* owner_web_contents)
    : GuestView<MimeHandlerViewGuest>(owner_web_contents),
      delegate_(
          ExtensionsAPIClient::Get()->CreateMimeHandlerViewGuestDelegate(this)),
      embedder_frame_process_id_(content::ChildProcessHost::kInvalidUniqueID),
      embedder_frame_routing_id_(MSG_ROUTING_NONE),
      embedder_widget_routing_id_(MSG_ROUTING_NONE) {}

MimeHandlerViewGuest::~MimeHandlerViewGuest() {
}

bool MimeHandlerViewGuest::CanUseCrossProcessFrames() {
  return false;
}

content::RenderWidgetHost* MimeHandlerViewGuest::GetOwnerRenderWidgetHost() {
  DCHECK_NE(embedder_widget_routing_id_, MSG_ROUTING_NONE);
  return content::RenderWidgetHost::FromID(embedder_frame_process_id_,
                                           embedder_widget_routing_id_);
}

content::SiteInstance* MimeHandlerViewGuest::GetOwnerSiteInstance() {
  DCHECK_NE(embedder_frame_routing_id_, MSG_ROUTING_NONE);
  content::RenderFrameHost* rfh = content::RenderFrameHost::FromID(
      embedder_frame_process_id_, embedder_frame_routing_id_);
  return rfh ? rfh->GetSiteInstance() : nullptr;
}

bool MimeHandlerViewGuest::CanBeEmbeddedInsideCrossProcessFrames() {
  return true;
}

void MimeHandlerViewGuest::SetEmbedderFrame(int process_id, int routing_id) {
  DCHECK_NE(MSG_ROUTING_NONE, routing_id);
  DCHECK_EQ(MSG_ROUTING_NONE, embedder_frame_routing_id_);

  embedder_frame_process_id_ = process_id;
  embedder_frame_routing_id_ = routing_id;

  content::RenderFrameHost* rfh =
      content::RenderFrameHost::FromID(process_id, routing_id);

  if (rfh && rfh->GetView()) {
    embedder_widget_routing_id_ =
        rfh->GetView()->GetRenderWidgetHost()->GetRoutingID();
  }

  DCHECK_NE(MSG_ROUTING_NONE, embedder_widget_routing_id_);
}

const char* MimeHandlerViewGuest::GetAPINamespace() const {
  return "mimeHandlerViewGuestInternal";
}

int MimeHandlerViewGuest::GetTaskPrefix() const {
  return IDS_EXTENSION_TASK_MANAGER_MIMEHANDLERVIEW_TAG_PREFIX;
}

void MimeHandlerViewGuest::CreateWebContents(
    const base::DictionaryValue& create_params,
    const WebContentsCreatedCallback& callback) {
  create_params.GetString(mime_handler_view::kViewId, &view_id_);
  if (view_id_.empty()) {
    callback.Run(nullptr);
    return;
  }
  stream_ =
      MimeHandlerStreamManager::Get(browser_context())->ReleaseStream(view_id_);
  if (!stream_) {
    callback.Run(nullptr);
    return;
  }
  const Extension* mime_handler_extension =
      // TODO(lazyboy): Do we need handle the case where the extension is
      // terminated (ExtensionRegistry::TERMINATED)?
      ExtensionRegistry::Get(browser_context())
          ->enabled_extensions()
          .GetByID(stream_->extension_id());
  if (!mime_handler_extension) {
    LOG(ERROR) << "Extension for mime_type not found, mime_type = "
               << stream_->stream_info()->mime_type;
    callback.Run(nullptr);
    return;
  }

  // Use the mime handler extension's SiteInstance to create the guest so it
  // goes under the same process as the extension.
  ProcessManager* process_manager = ProcessManager::Get(browser_context());
  scoped_refptr<content::SiteInstance> guest_site_instance =
      process_manager->GetSiteInstanceForURL(stream_->handler_url());

  // Clear the zoom level for the mime handler extension. The extension is
  // responsible for managing its own zoom. This is necessary for OOP PDF, as
  // otherwise the UI is zoomed and the calculations to determine the PDF size
  // mix zoomed and unzoomed units.
  content::HostZoomMap::Get(guest_site_instance.get())
      ->SetZoomLevelForHostAndScheme(kExtensionScheme, stream_->extension_id(),
                                     0);

  WebContents::CreateParams params(browser_context(),
                                   guest_site_instance.get());
  params.guest_delegate = this;
  callback.Run(WebContents::Create(params));
}

void MimeHandlerViewGuest::DidAttachToEmbedder() {
  web_contents()->GetController().LoadURL(
      stream_->handler_url(), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
  web_contents()->GetMainFrame()->GetInterfaceRegistry()->AddInterface(
      base::Bind(&MimeHandlerServiceImpl::Create, stream_->GetWeakPtr()));
}

void MimeHandlerViewGuest::DidInitialize(
    const base::DictionaryValue& create_params) {
  ExtensionsAPIClient::Get()->AttachWebContentsHelpers(web_contents());
}

bool MimeHandlerViewGuest::ShouldHandleFindRequestsForEmbedder() const {
  return is_full_page_plugin();
}

bool MimeHandlerViewGuest::ZoomPropagatesFromEmbedderToGuest() const {
  return false;
}

WebContents* MimeHandlerViewGuest::OpenURLFromTab(
    WebContents* source,
    const content::OpenURLParams& params) {
  if (!embedder_web_contents())
    return owner_web_contents()->GetDelegate()->OpenURLFromTab(
      owner_web_contents(), params);
  return embedder_web_contents()->GetDelegate()->OpenURLFromTab(
      embedder_web_contents(), params);
}

void MimeHandlerViewGuest::NavigationStateChanged(
    WebContents* source,
    content::InvalidateTypes changed_flags) {
  if (!(changed_flags & content::INVALIDATE_TYPE_TITLE))
    return;

  // Only consider title changes not triggered by URL changes. Otherwise, the
  // URL of the mime handler will be displayed.
  if (changed_flags & content::INVALIDATE_TYPE_URL)
    return;

  if (!is_full_page_plugin())
    return;

  content::NavigationEntry* last_committed_entry =
      embedder_web_contents()->GetController().GetLastCommittedEntry();
  if (last_committed_entry) {
    embedder_web_contents()->UpdateTitleForEntry(last_committed_entry,
                                                 source->GetTitle());
    embedder_web_contents()->GetDelegate()->NavigationStateChanged(
        embedder_web_contents(), changed_flags);
  }
}

bool MimeHandlerViewGuest::HandleContextMenu(
    const content::ContextMenuParams& params) {
  if (delegate_)
    return delegate_->HandleContextMenu(web_contents(), params);

  return false;
}

bool MimeHandlerViewGuest::PreHandleGestureEvent(
    WebContents* source,
    const blink::WebGestureEvent& event) {
  if (event.type == blink::WebGestureEvent::GesturePinchBegin ||
      event.type == blink::WebGestureEvent::GesturePinchUpdate ||
      event.type == blink::WebGestureEvent::GesturePinchEnd) {
    // If we're an embedded plugin we drop pinch-gestures to avoid zooming the
    // guest.
    return !is_full_page_plugin();
  }
  return false;
}

content::JavaScriptDialogManager*
MimeHandlerViewGuest::GetJavaScriptDialogManager(
    WebContents* source) {
  // WebContentsDelegates often service multiple WebContentses, and use the
  // WebContents* parameter to tell which WebContents made the request. If we
  // pass in our own pointer to the delegate call, the delegate will be asked,
  // "What's the JavaScriptDialogManager of this WebContents for which you are
  // not a delegate?" And it won't be able to answer that.
  //
  // So we pretend to be our owner WebContents, but only for the request to
  // obtain the JavaScriptDialogManager. During calls to the
  // JavaScriptDialogManager we will be honest about who we are.
  return owner_web_contents()->GetDelegate()->GetJavaScriptDialogManager(
      owner_web_contents());
}

bool MimeHandlerViewGuest::SaveFrame(const GURL& url,
                                     const content::Referrer& referrer) {
  if (!attached())
    return false;

  embedder_web_contents()->SaveFrame(stream_->stream_info()->original_url,
                                     referrer);
  return true;
}

void MimeHandlerViewGuest::OnRenderFrameHostDeleted(int process_id,
                                                    int routing_id) {
  if (process_id == embedder_frame_process_id_ &&
      routing_id == embedder_frame_routing_id_)
    Destroy();
}

void MimeHandlerViewGuest::DocumentOnLoadCompletedInMainFrame() {
  // Assume the embedder WebContents is valid here.
  DCHECK(embedder_web_contents());

  // If the guest is embedded inside a cross-process frame and the frame is
  // removed before the guest is properly loaded, then owner RenderWidgetHost
  // will be nullptr.
  if (auto* rwh = GetOwnerRenderWidgetHost()) {
    rwh->Send(new ExtensionsGuestViewMsg_MimeHandlerViewGuestOnLoadCompleted(
        element_instance_id()));
  }
}

base::WeakPtr<StreamContainer> MimeHandlerViewGuest::GetStream() const {
  if (!stream_)
    return base::WeakPtr<StreamContainer>();
  return stream_->GetWeakPtr();
}

}  // namespace extensions
