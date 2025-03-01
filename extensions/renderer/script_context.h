// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_RENDERER_SCRIPT_CONTEXT_H_
#define EXTENSIONS_RENDERER_SCRIPT_CONTEXT_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/macros.h"
#include "base/threading/thread_checker.h"
#include "extensions/common/features/feature.h"
#include "extensions/common/permissions/api_permission_set.h"
#include "extensions/renderer/module_system.h"
#include "extensions/renderer/request_sender.h"
#include "extensions/renderer/safe_builtins.h"
#include "gin/runner.h"
#include "url/gurl.h"
#include "v8/include/v8.h"

namespace blink {
class WebFrame;
class WebLocalFrame;
}

namespace content {
class RenderFrame;
}

namespace extensions {
class Extension;

// Extensions wrapper for a v8::Context.
//
// v8::Contexts can be constructed on any thread, and must only be accessed or
// destroyed that thread.
//
// Note that ScriptContexts bound to worker threads will not have the full
// functionality as those bound to the main RenderThread.
class ScriptContext : public RequestSender::Source {
 public:
  using RunScriptExceptionHandler = base::Callback<void(const v8::TryCatch&)>;

  ScriptContext(const v8::Local<v8::Context>& context,
                blink::WebLocalFrame* frame,
                const Extension* extension,
                Feature::Context context_type,
                const Extension* effective_extension,
                Feature::Context effective_context_type);
  ~ScriptContext() override;

  // Returns whether |url| from any Extension in |extension_set| is sandboxed,
  // as declared in each Extension's manifest.
  // TODO(kalman): Delete this when crbug.com/466373 is fixed.
  // See comment in HasAccessOrThrowError.
  static bool IsSandboxedPage(const GURL& url);

  // Clears the WebFrame for this contexts and invalidates the associated
  // ModuleSystem.
  void Invalidate();

  // Registers |observer| to be run when this context is invalidated. Closures
  // are run immediately when Invalidate() is called, not in a message loop.
  void AddInvalidationObserver(const base::Closure& observer);

  // Returns true if this context is still valid, false if it isn't.
  // A context becomes invalid via Invalidate().
  bool is_valid() const { return is_valid_; }

  v8::Local<v8::Context> v8_context() const {
    return v8::Local<v8::Context>::New(isolate_, v8_context_);
  }

  const Extension* extension() const { return extension_.get(); }

  const Extension* effective_extension() const {
    return effective_extension_.get();
  }

  blink::WebLocalFrame* web_frame() const { return web_frame_; }

  Feature::Context context_type() const { return context_type_; }

  Feature::Context effective_context_type() const {
    return effective_context_type_;
  }

  void set_module_system(std::unique_ptr<ModuleSystem> module_system) {
    module_system_ = std::move(module_system);
  }

  ModuleSystem* module_system() { return module_system_.get(); }

  SafeBuiltins* safe_builtins() { return &safe_builtins_; }

  const SafeBuiltins* safe_builtins() const { return &safe_builtins_; }

  // Returns the ID of the extension associated with this context, or empty
  // string if there is no such extension.
  const std::string& GetExtensionID() const;

  // Returns the RenderFrame associated with this context. Can return NULL if
  // the context is in the process of being destroyed.
  content::RenderFrame* GetRenderFrame() const;

  // DEPRECATED.
  v8::Local<v8::Value> CallFunction(const v8::Local<v8::Function>& function,
                                    int argc,
                                    v8::Local<v8::Value> argv[]) const;

  // Safely calls the v8::Function, respecting the page load deferrer and
  // possibly executing asynchronously.
  // Doesn't catch exceptions; callers must do that if they want.
  // USE THIS METHOD RATHER THAN v8::Function::Call WHEREVER POSSIBLE.
  // TODO(devlin): Remove the above variants in favor of this.
  void SafeCallFunction(const v8::Local<v8::Function>& function,
                        int argc,
                        v8::Local<v8::Value> argv[]);

  void DispatchEvent(const char* event_name, v8::Local<v8::Array> args) const;

  // Returns the availability of the API |api_name|.
  Feature::Availability GetAvailability(const std::string& api_name);

  // Returns a string description of the type of context this is.
  std::string GetContextTypeDescription() const;

  // Returns a string description of the effective type of context this is.
  std::string GetEffectiveContextTypeDescription() const;

  v8::Isolate* isolate() const { return isolate_; }

  // Get the URL of this context's web frame.
  //
  // TODO(kalman): Remove this and replace with a GetOrigin() call which reads
  // of WebDocument::getSecurityOrigin():
  //  - The URL can change (e.g. pushState) but the origin cannot. Luckily it
  //    appears as though callers don't make security decisions based on the
  //    result of url() so it's not a problem... yet.
  //  - Origin is the correct check to be making.
  //  - It might let us remove the about:blank resolving?
  const GURL& url() const { return url_; }

  // Sets the URL of this ScriptContext. Usually this will automatically be set
  // on construction, unless this isn't constructed with enough information to
  // determine the URL (e.g. frame was null).
  // TODO(kalman): Make this a constructor parameter (as an origin).
  void set_url(const GURL& url) { url_ = url; }

  // Returns whether the API |api| or any part of the API could be
  // available in this context without taking into account the context's
  // extension.
  bool IsAnyFeatureAvailableToContext(const extensions::Feature& api);

  // Utility to get the URL we will match against for a frame. If the frame has
  // committed, this is the commited URL. Otherwise it is the provisional URL.
  // The returned URL may be invalid.
  static GURL GetDataSourceURLForFrame(const blink::WebFrame* frame);

  // Similar to GetDataSourceURLForFrame, but only returns the data source URL
  // if the frame's document url is empty and the frame has a security origin
  // that allows access to the data source url.
  // TODO(asargent/devlin) - there may be places that should switch to using
  // this instead of GetDataSourceURLForFrame.
  static GURL GetAccessCheckedFrameURL(const blink::WebFrame* frame);

  // Returns the first non-about:-URL in the document hierarchy above and
  // including |frame|. The document hierarchy is only traversed if
  // |document_url| is an about:-URL and if |match_about_blank| is true.
  static GURL GetEffectiveDocumentURL(const blink::WebFrame* frame,
                                      const GURL& document_url,
                                      bool match_about_blank);

  // RequestSender::Source implementation.
  ScriptContext* GetContext() override;
  void OnResponseReceived(const std::string& name,
                          int request_id,
                          bool success,
                          const base::ListValue& response,
                          const std::string& error) override;

  // Grants a set of content capabilities to this context.
  void set_content_capabilities(const APIPermissionSet& capabilities) {
    content_capabilities_ = capabilities;
  }

  // Indicates if this context has an effective API permission either by being
  // a context for an extension which has that permission, or by being a web
  // context which has been granted the corresponding capability by an
  // extension.
  bool HasAPIPermission(APIPermission::ID permission) const;

  // Throws an Error in this context's JavaScript context, if this context does
  // not have access to |name|. Returns true if this context has access (i.e.
  // no exception thrown), false if it does not (i.e. an exception was thrown).
  bool HasAccessOrThrowError(const std::string& name);

  // Returns a string representation of this ScriptContext, for debugging.
  std::string GetDebugString() const;

  // Gets the current stack trace as a multi-line string to be logged.
  std::string GetStackTraceAsString() const;

  // Runs |code|, labelling the script that gets created as |name| (the name is
  // used in the devtools and stack traces). |exception_handler| will be called
  // re-entrantly if an exception is thrown during the script's execution.
  v8::Local<v8::Value> RunScript(
      v8::Local<v8::String> name,
      v8::Local<v8::String> code,
      const RunScriptExceptionHandler& exception_handler);

 private:
  class Runner;

  // Whether this context is valid.
  bool is_valid_;

  // The v8 context the bindings are accessible to.
  v8::Global<v8::Context> v8_context_;

  // The WebLocalFrame associated with this context. This can be NULL because
  // this object can outlive is destroyed asynchronously.
  blink::WebLocalFrame* web_frame_;

  // The extension associated with this context, or NULL if there is none. This
  // might be a hosted app in the case that this context is hosting a web URL.
  scoped_refptr<const Extension> extension_;

  // The type of context.
  Feature::Context context_type_;

  // The effective extension associated with this context, or NULL if there is
  // none. This is different from the above extension if this context is in an
  // about:blank iframe for example.
  scoped_refptr<const Extension> effective_extension_;

  // The type of context.
  Feature::Context effective_context_type_;

  // Owns and structures the JS that is injected to set up extension bindings.
  std::unique_ptr<ModuleSystem> module_system_;

  // Contains safe copies of builtin objects like Function.prototype.
  SafeBuiltins safe_builtins_;

  // The set of capabilities granted to this context by extensions.
  APIPermissionSet content_capabilities_;

  // A list of base::Closure instances as an observer interface for
  // invalidation.
  std::vector<base::Closure> invalidate_observers_;

  v8::Isolate* isolate_;

  GURL url_;

  std::unique_ptr<Runner> runner_;

  base::ThreadChecker thread_checker_;
 public:
  base::WeakPtrFactory<ScriptContext> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(ScriptContext);
};

}  // namespace extensions

#endif  // EXTENSIONS_RENDERER_SCRIPT_CONTEXT_H_
